#include "Provisioner.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

namespace lt {

namespace {

constexpr const char* LOGIN_MUTATION = R"(
mutation Login($input: LoginUserInput!) {
  login(loginUserInput: $input) { token gameTokenId user { userId } }
})";

constexpr const char* REGISTER_MUTATION = R"(
mutation Register($input: RegisterUserInput!) {
  register(registerUserInput: $input) { token gameTokenId user { userId } }
})";

constexpr const char* MINT_MUTATION = R"(
mutation Mint($input: MintAppTokenInput!) {
  mintAppToken(input: $input) { token gameTokenId appId expiresAt gameApiUrl }
})";

constexpr const char* REFRESH_MUTATION = R"(
mutation Refresh {
  refreshAppToken { token gameTokenId appId expiresAt gameApiUrl }
})";

// Request only fields that exist on ServerStatus: an invalid field fails the
// whole query and the Buddy session is never installed.
constexpr const char* ASSIGN_QUERY = R"(
query Assign {
  serverWithLeastClients { serverId ip4 clientPort status clients }
})";

// Read-only, but with an important side effect for UDP clients: the Game API
// mirrors this user's app entitlements into the game database and grants the
// app's open-by-default grid access — without it Buddy rejects spatial
// traffic with UNAUTHORIZED even for entitled users.
constexpr const char* BOOTSTRAP_QUERY = R"(
query Bootstrap($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) { appId maxReplicationDistance }
})";

/// Retry transport-level failures with backoff; GraphQL-level errors are
/// returned to the caller immediately (they are deterministic).
nlohmann::json requestWithRetry(GraphQLClient& client, const std::string& query,
                                const nlohmann::json& variables,
                                const std::string& bearer,
                                std::atomic<bool>* stop = nullptr) {
    int attempts = 0;
    for (;;) {
        try {
            return client.request(query, variables, bearer);
        } catch (const GraphQLError& e) {
            if (!e.isTransport() || ++attempts >= 3 ||
                (stop && stop->load())) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempts));
        }
    }
}

} // namespace

std::chrono::system_clock::time_point parseIso8601Utc(const std::string& iso,
                                                      int fallbackSec) {
    std::tm tm{};
    int year, mon, day, hour, min;
    double sec = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &year, &mon, &day, &hour,
                    &min, &sec) == 6) {
        tm.tm_year = year - 1900;
        tm.tm_mon = mon - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = static_cast<int>(sec);
        time_t t = timegm(&tm);
        if (t != static_cast<time_t>(-1)) {
            return std::chrono::system_clock::from_time_t(t) +
                   std::chrono::milliseconds(
                       static_cast<int>((sec - static_cast<int>(sec)) * 1000));
        }
    }
    return std::chrono::system_clock::now() + std::chrono::seconds(fallbackSec);
}

/// Load a pre-minted session roster.
///
/// THE ORIGIN IS CHECKED, and that is the whole reason the file records one. A
/// session minted against one tier is a syntactically valid bearer token that
/// means nothing on another, so a roster carried between tiers produces
/// authentication failures that read like a broken tier rather than like a
/// misplaced file. Refusing here costs one comparison; diagnosing it later
/// costs an afternoon.
///
/// A MISSING FILE IS A REFUSAL, never an empty roster. "Could not read the
/// roster" and "the roster covers nobody" would otherwise be the same
/// observation, and the second silently signs every client in -- which is
/// exactly the cost the roster exists to avoid.
Provisioner::Provisioner(const Config& config) : config_(config) {
    if (config_.rosterFile.empty()) return;

    std::ifstream f(config_.rosterFile);
    if (!f) {
        throw GraphQLError("cannot open LT_ROSTER_FILE '" + config_.rosterFile +
                               "'. A roster that cannot be read is a refusal, "
                               "not an empty roster: continuing would sign "
                               "every client in and quietly measure bcrypt.",
                           "", false);
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        throw GraphQLError("LT_ROSTER_FILE '" + config_.rosterFile +
                               "' is not valid JSON: " + e.what(),
                           "", false);
    }

    // The roster names the origin it was minted against. Compare on the base
    // URL the harness was pointed at, trailing slash insensitively.
    auto trimSlash = [](std::string s) {
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    };
    if (doc.contains("origin") && doc["origin"].is_string()) {
        std::string rosterOrigin = trimSlash(doc["origin"].get<std::string>());
        std::string ours = trimSlash(config_.managementApiUrl);
        if (rosterOrigin != ours) {
            throw GraphQLError(
                "LT_ROSTER_FILE was minted against '" + rosterOrigin +
                    "' but this run targets '" + ours +
                    "'. A session is only valid on the origin that issued it; "
                    "re-provision the roster for this origin rather than "
                    "pointing the harness at the other one.",
                "", false);
        }
    }

    for (const auto& entry : doc.value("sessions", nlohmann::json::array())) {
        if (!entry.contains("index") || !entry.contains("token")) continue;
        int idx = entry["index"].get<int>();
        std::string token = entry["token"].get<std::string>();
        if (token.empty()) continue;
        roster_[idx] = std::move(token);
        if (entry.contains("email") && entry["email"].is_string()) {
            rosterEmails_[idx] = entry["email"].get<std::string>();
        }
    }
}

std::string Provisioner::rosterSession(int index, const std::string& email) const {
    auto it = roster_.find(index);
    if (it == roster_.end()) return "";
    // A roster entry whose email disagrees with the pattern this run derives is
    // a roster for a different population. Using it would authenticate as
    // somebody else and report success, so it is a refusal.
    auto e = rosterEmails_.find(index);
    if (e != rosterEmails_.end() && e->second != email) {
        throw GraphQLError(
            "LT_ROSTER_FILE entry " + std::to_string(index) + " is for '" +
                e->second + "' but this run derives '" + email +
                "' from LT_EMAIL/LT_EMAIL_PATTERN. The roster belongs to a "
                "different bot population; regenerate it or fix the pattern.",
            "", false);
    }
    return it->second;
}

std::string Provisioner::signIn(GraphQLClient& mgmt, const std::string& email) {
    nlohmann::json creds = {{"email", email}, {"password", config_.password}};

    // 1. Existing account: plain login.
    try {
        auto data = requestWithRetry(mgmt, LOGIN_MUTATION, {{"input", creds}}, "");
        return data["login"]["token"].get<std::string>();
    } catch (const GraphQLError& e) {
        if (e.isTransport()) throw;
        // Fall through: most likely the account does not exist yet.
    }

    // 2. First run: register (a fresh password-only account gets a session
    //    immediately, no email confirmation required).
    try {
        auto data = requestWithRetry(mgmt, REGISTER_MUTATION, {{"input", creds}}, "");
        return data["register"]["token"].get<std::string>();
    } catch (const GraphQLError& e) {
        if (e.isTransport()) throw;
        // CONFLICT: account exists (e.g. created concurrently, or it exists
        // with a different password / another sign-in method). Try login once
        // more; if that also fails, surface an actionable error.
    }

    try {
        auto data = requestWithRetry(mgmt, LOGIN_MUTATION, {{"input", creds}}, "");
        return data["login"]["token"].get<std::string>();
    } catch (const GraphQLError& e) {
        if (e.isTransport()) throw;
        throw GraphQLError(
            "cannot sign in as '" + email +
                "': the account exists but this password is rejected (" +
                e.what() +
                "). Derived load-test accounts must be password accounts created "
                "by this tool; pick a different LT_EMAIL_PATTERN or fix "
                "LT_PASSWORD.",
            e.code(), false);
    }
}

void Provisioner::mintAppToken(GraphQLClient& mgmt, ClientCredentials& c) {
    nlohmann::json vars = {{"input", {{"appId", std::to_string(config_.appId)}}}};
    nlohmann::json data;
    try {
        data = requestWithRetry(mgmt, MINT_MUTATION, vars, c.sessionToken);
    } catch (const GraphQLError& e) {
        if (!e.isTransport() && e.code() == "FORBIDDEN") {
            throw GraphQLError(
                "mintAppToken(appId=" + std::to_string(config_.appId) +
                    ") was FORBIDDEN for '" + c.email +
                    "'. The load tester does not manage entitlements: grant the "
                    "derived load-test accounts access to the app (they follow "
                    "your LT_EMAIL_PATTERN) before running.",
                e.code(), false);
        }
        throw;
    }
    const auto& mint = data["mintAppToken"];
    c.appToken = mint["token"].get<std::string>();
    c.gameTokenId = std::stoll(mint["gameTokenId"].get<std::string>());
    c.tokenExpiresAt = parseIso8601Utc(mint["expiresAt"].get<std::string>());
    if (!config_.gameApiUrl.empty()) {
        c.gameApiUrl = config_.gameApiUrl;
    } else if (mint.contains("gameApiUrl") && mint["gameApiUrl"].is_string()) {
        c.gameApiUrl = mint["gameApiUrl"].get<std::string>();
    }
    if (c.gameApiUrl.empty()) {
        throw GraphQLError(
            "mintAppToken returned no gameApiUrl for app " +
                std::to_string(config_.appId) +
                " and LT_GAME_API_URL is not set. Set LT_GAME_API_URL to your "
                "app's Game API base URL.",
            "", false);
    }
    if (c.appToken.size() != 64) {
        throw GraphQLError("unexpected app token length " +
                               std::to_string(c.appToken.size()) +
                               " (expected 64 characters)",
                           "", false);
    }
}

void Provisioner::bootstrapGameClient(ClientCredentials& c) {
    GraphQLClient game(c.gameApiUrl, config_.tlsInsecure);
    nlohmann::json vars = {{"appId", std::to_string(config_.appId)}};
    requestWithRetry(game, BOOTSTRAP_QUERY, vars, c.appToken);
}

void Provisioner::assignServer(ClientCredentials& c) {
    GraphQLClient game(c.gameApiUrl, config_.tlsInsecure);
    auto data = requestWithRetry(game, ASSIGN_QUERY, nlohmann::json::object(),
                                 c.appToken);
    const auto& server = data["serverWithLeastClients"];
    c.serverIp4 = server["ip4"].get<std::string>();
    c.serverPort = static_cast<uint16_t>(server["clientPort"].get<int>());
    c.udpReadyAt = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(config_.sessionSettleMs);
}

void Provisioner::refreshToken(GraphQLClient& mgmt, ClientCredentials& c) {
    auto data = requestWithRetry(mgmt, REFRESH_MUTATION, nlohmann::json::object(),
                                 c.appToken);
    const auto& fresh = data["refreshAppToken"];
    c.appToken = fresh["token"].get<std::string>();
    c.gameTokenId = std::stoll(fresh["gameTokenId"].get<std::string>());
    c.tokenExpiresAt = parseIso8601Utc(fresh["expiresAt"].get<std::string>());
    // The new gameTokenId has no Buddy session yet; the caller must reassign.
}

std::vector<ClientCredentials> Provisioner::provisionAll(std::atomic<bool>& stop) {
    const int total = config_.clients;

    // Say what the roster covers BEFORE doing any work, and refuse a short one
    // when asked to. A roster covering 40 of 100 clients is the dangerous
    // middle: the run succeeds and sixty sign-ins disappear into the numbers.
    if (!config_.rosterFile.empty()) {
        int covered = 0;
        for (int i = 0; i < total; ++i) {
            if (roster_.count(i)) ++covered;
        }
        std::printf("[provision] roster: %d of %d client(s) have a pre-minted "
                    "session (%d entr%s in %s)\n",
                    covered, total, rosterSize(), rosterSize() == 1 ? "y" : "ies",
                    config_.rosterFile.c_str());
        if (covered < total && config_.rosterRequired) {
            std::fprintf(stderr,
                         "[provision] FATAL: LT_ROSTER_REQUIRED is set and the "
                         "roster covers %d of %d clients. The remaining %d would "
                         "each cost a bcrypt sign-in inside the process you are "
                         "timing. Re-provision the roster for %d clients, or "
                         "unset LT_ROSTER_REQUIRED to accept the cost.\n",
                         covered, total, total - covered, total);
            return {};
        }
    }
    std::vector<ClientCredentials> out(static_cast<size_t>(total));
    std::atomic<int> nextIndex{0};
    std::atomic<int> completed{0};
    std::atomic<bool> failed{false};
    std::mutex logMutex;
    std::string firstError;

    auto workerFn = [&]() {
        GraphQLClient mgmt(config_.managementApiUrl, config_.tlsInsecure);
        for (;;) {
            int i = nextIndex.fetch_add(1);
            if (i >= total || failed.load() || stop.load()) return;
            ClientCredentials c;
            c.index = i;
            c.email = config_.derivedEmail(i);
            try {
                c.sessionToken = rosterSession(i, c.email);
                if (!c.sessionToken.empty()) {
                    signIns_.reused.fetch_add(1, std::memory_order_relaxed);
                } else {
                    c.sessionToken = signIn(mgmt, c.email);
                    // Which counter this lands in is the difference between a
                    // cost paid before the clock started and one inside the
                    // steady state.
                    if (runStarted_.load(std::memory_order_relaxed)) {
                        signIns_.mintedMidRun.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        signIns_.mintedProvisioning.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                mintAppToken(mgmt, c);
                bootstrapGameClient(c);
                assignServer(c);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(logMutex);
                if (!failed.exchange(true)) firstError = e.what();
                return;
            }
            out[static_cast<size_t>(i)] = std::move(c);
            int done = completed.fetch_add(1) + 1;
            if (done % 50 == 0 || done == total) {
                std::lock_guard<std::mutex> lock(logMutex);
                std::printf("[provision] %d/%d clients ready\n", done, total);
                std::fflush(stdout);
            }
        }
    };

    int threadCount = std::min(config_.provisionConcurrency, total);
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(threadCount));
    for (int t = 0; t < threadCount; ++t) threads.emplace_back(workerFn);
    for (auto& t : threads) t.join();

    if (stop.load()) {
        std::printf("[provision] aborted by signal\n");
        return {};
    }
    if (failed.load()) {
        std::fprintf(stderr, "[provision] FATAL: %s\n", firstError.c_str());
        return {};
    }
    return out;
}

} // namespace lt
