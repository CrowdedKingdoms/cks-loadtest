#include "fleet/FleetApi.hpp"

#include "Config.hpp"
#include "Provisioner.hpp"

#include <chrono>
#include <thread>

namespace lt::fleet {

namespace {

constexpr const char* OPEN_MUTATION =
    "mutation($i: OpenFleetSessionInput!) { openFleetSession(input: $i) { token gameTokenId "
    "appId expiresAt maxPlayers server { ip4 clientPort } } }";

constexpr const char* MINT_MUTATION =
    "mutation($i: MintManagedPlayerTokenInput!) { mintManagedPlayerToken(input: $i) { token "
    "gameTokenId expiresAt } }";

constexpr const char* CLOSE_MUTATION =
    "mutation($i: CloseFleetSessionInput!) { closeFleetSession(input: $i) }";

constexpr const char* REFRESH_SESSION_MUTATION =
    "mutation($i: RefreshFleetSessionInput!) { refreshFleetSession(input: $i) { token "
    "gameTokenId expiresAt server { ip4 clientPort } } }";

constexpr const char* DELETE_PLAYER_MUTATION =
    "mutation($o: BigInt!, $e: String!) { deleteManagedPlayer(orgId: $o, externalId: $e) }";

constexpr const char* BLOCK_PLAYER_MUTATION =
    "mutation($a: BigInt!, $u: BigInt!) { blockPlayer(appId: $a, userId: $u) { __typename } }";

int64_t toEpochMs(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

/// BigInt values are decimal strings in JSON; an Int arrives as a number.
int64_t asI64(const nlohmann::json& v, const char* what) {
    if (v.is_string()) return std::stoll(v.get<std::string>());
    if (v.is_number_integer()) return v.get<int64_t>();
    throw GraphQLError(std::string("the API answered no ") + what, "", false);
}

std::string asToken(const nlohmann::json& v, const char* what) {
    if (!v.is_string() || v.get<std::string>().size() != 64) {
        throw GraphQLError(std::string("the API answered a ") + what +
                               " that is not 64 characters",
                           "", false);
    }
    return v.get<std::string>();
}

int64_t asExpiry(const nlohmann::json& v) {
    if (!v.is_string()) return 0;
    return toEpochMs(parseIso8601Utc(v.get<std::string>()));
}

ServerAddr asServer(const nlohmann::json& v) {
    ServerAddr s;
    if (!v.is_object() || !v.contains("ip4") || !v["ip4"].is_string() ||
        !v.contains("clientPort") || !v["clientPort"].is_number_integer()) {
        throw GraphQLError("the API answered no realtime server (ip4, clientPort)", "", false);
    }
    s.ip4 = v["ip4"].get<std::string>();
    s.port = static_cast<uint16_t>(v["clientPort"].get<int>());
    return s;
}

class CurlTransport final : public ApiTransport {
public:
    CurlTransport(const std::string& apiUrl, bool tlsInsecure)
        : client_(apiUrl, tlsInsecure), provisioner_(config(tlsInsecure)) {}

    nlohmann::json request(const std::string& query, const nlohmann::json& variables,
                           const std::string& bearer) override {
        // No extra headers: an org token is refused when the request has an Origin.
        return client_.request(query, variables, bearer);
    }

    PlayerToken refreshAppToken(const std::string& playerToken, int& httpCalls) override {
        // The harness's own refresh. With no realtime server named it sends
        // `mutation { refreshAppToken { token gameTokenId ... expiresAt } }` with the
        // player's app token as the bearer. A carried player has no session of
        // its own to keep, so there is no current server to name.
        ClientCredentials creds;
        creds.appToken = playerToken;
        const uint64_t before = client_.requestCount();
        try {
            provisioner_.refreshToken(client_, creds);
        } catch (...) {
            httpCalls = static_cast<int>(client_.requestCount() - before);
            throw;
        }
        httpCalls = static_cast<int>(client_.requestCount() - before);
        PlayerToken out;
        out.token = creds.appToken;
        out.gameTokenId = creds.gameTokenId;
        out.expiresAtMs = toEpochMs(creds.tokenExpiresAt);
        if (out.token.size() != 64) {
            throw GraphQLError("refreshAppToken answered a token that is not 64 characters", "",
                               false);
        }
        return out;
    }

private:
    static const Config& config(bool tlsInsecure) {
        // The Provisioner reads only tlsInsecure and the settle time on this path.
        static const Config secure;
        static const Config insecure = [] {
            Config c;
            c.tlsInsecure = true;
            return c;
        }();
        return tlsInsecure ? insecure : secure;
    }
    GraphQLClient client_;
    Provisioner provisioner_;
};

thread_local bool tlsCrossingScope = false;

} // namespace

bool& crossingScopeFlag() { return tlsCrossingScope; }

std::unique_ptr<ApiTransport> makeCurlTransport(const std::string& apiUrl, bool tlsInsecure) {
    return std::make_unique<CurlTransport>(apiUrl, tlsInsecure);
}

std::string playerExternalId(int n) { return "fleet-load-" + std::to_string(n); }

FleetApi::FleetApi(std::unique_ptr<ApiTransport> transport, std::string orgToken, int64_t appId,
                   ApiCounters& counters, std::atomic<bool>* stop,
                   std::function<void(int)> sleepMs)
    : transport_(std::move(transport)), orgToken_(std::move(orgToken)), appId_(appId),
      counters_(counters), stop_(stop), sleepMs_(std::move(sleepMs)) {
    if (!sleepMs_) {
        sleepMs_ = [this](int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                if (stop_ && stop_->load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
    }
}

std::string FleetApi::redact(std::string text) const {
    if (orgToken_.empty()) return text;
    for (size_t at = text.find(orgToken_); at != std::string::npos; at = text.find(orgToken_, at)) {
        text.replace(at, orgToken_.size(), "[org token]");
    }
    return text;
}

template <typename Fn> auto FleetApi::call(ApiKind kind, ApiCause cause, Fn&& once) {
    const size_t k = static_cast<size_t>(kind);
    if (crossingScopeFlag()) {
        // Never expected. Count it where the summary and the exit code look.
        counters_.requestsFromCrossingPath.fetch_add(1, std::memory_order_relaxed);
        cause = ApiCause::Crossing;
    }
    int busy = 0;
    int transportAttempts = 0;
    for (;;) {
        // `once` reports how many HTTP requests it made (1 unless a helper retried).
        int httpCalls = 1;
        try {
            // An answer without the fields asked for is an API fault, reported
            // like one: not retried, counted as a failure.
            auto guarded = [&]() {
                try {
                    return once(httpCalls);
                } catch (const GraphQLError&) {
                    throw;
                } catch (const std::exception& e) {
                    throw GraphQLError(std::string(apiKindName(kind)) +
                                           " answered something unexpected: " + e.what(),
                                       "", false);
                }
            };
            auto out = guarded();
            for (int i = 0; i < httpCalls; ++i) counters_.count(kind, cause);
            return out;
        } catch (const GraphQLError& e) {
            for (int i = 0; i < httpCalls; ++i) counters_.count(kind, cause);
            const bool stopping = stop_ && stop_->load(std::memory_order_relaxed);
            const bool isBusy = e.code() == "PLATFORM_BUSY";
            const bool isLimited = e.code() == "RATE_LIMITED";
            if (isBusy || isLimited) {
                (isBusy ? counters_.platformBusy : counters_.rateLimited)[k].fetch_add(
                    1, std::memory_order_relaxed);
                // Teardown still retries while stopping: the sessions must be closed.
                if (++busy <= MAX_BUSY_RETRIES && (!stopping || cause == ApiCause::Teardown)) {
                    sleepMs_(e.retryAfterMs() >= 0 ? e.retryAfterMs() : DEFAULT_RETRY_AFTER_MS);
                    continue;
                }
            } else if (e.isTransport() && ++transportAttempts < MAX_TRANSPORT_ATTEMPTS &&
                       (!stopping || cause == ApiCause::Teardown)) {
                sleepMs_(500 * transportAttempts);
                continue;
            }
            counters_.failures[k].fetch_add(1, std::memory_order_relaxed);
            throw GraphQLError(redact(e.what()), e.code(), e.isTransport(), e.retryAfterMs());
        }
    }
}

FleetSessionInfo FleetApi::openFleetSession(ApiCause cause) {
    return call(ApiKind::OpenFleetSession, cause, [&](int&) {
        const nlohmann::json vars = {{"i", {{"appId", std::to_string(appId_)}}}};
        const auto data = transport_->request(OPEN_MUTATION, vars, orgToken_);
        const auto& s = data.at("openFleetSession");
        FleetSessionInfo out;
        out.token = asToken(s.at("token"), "fleet token");
        out.gameTokenId = asI64(s.at("gameTokenId"), "gameTokenId");
        out.appId = s.contains("appId") ? asI64(s["appId"], "appId") : appId_;
        out.expiresAtMs = asExpiry(s.at("expiresAt"));
        out.maxPlayers = s.contains("maxPlayers") && s["maxPlayers"].is_number_integer()
                             ? s["maxPlayers"].get<int>()
                             : 0;
        out.server = asServer(s.at("server"));
        return out;
    });
}

PlayerToken FleetApi::mintManagedPlayerToken(int n, ApiCause cause) {
    return mintManagedPlayerToken(playerExternalId(n), 30, cause);
}

PlayerToken FleetApi::mintManagedPlayerToken(const std::string& externalId, int ageYears,
                                             ApiCause cause) {
    return call(ApiKind::MintManagedPlayerToken, cause, [&](int&) {
        const nlohmann::json vars = {{"i",
                                      {{"appId", std::to_string(appId_)},
                                       {"externalId", externalId},
                                       {"gamertag", externalId},
                                       {"ageYears", ageYears},
                                       {"country", "US"}}}};
        const auto data = transport_->request(MINT_MUTATION, vars, orgToken_);
        const auto& t = data.at("mintManagedPlayerToken");
        PlayerToken out;
        out.token = asToken(t.at("token"), "player token");
        out.gameTokenId = asI64(t.at("gameTokenId"), "gameTokenId");
        out.expiresAtMs = asExpiry(t.at("expiresAt"));
        return out;
    });
}

bool FleetApi::closeFleetSession(int64_t gameTokenId, ApiCause cause) {
    return call(ApiKind::CloseFleetSession, cause, [&](int&) {
        const nlohmann::json vars = {{"i",
                                      {{"appId", std::to_string(appId_)},
                                       {"gameTokenId", std::to_string(gameTokenId)}}}};
        const auto data = transport_->request(CLOSE_MUTATION, vars, orgToken_);
        const auto& v = data.at("closeFleetSession");
        return v.is_boolean() ? v.get<bool>() : false;
    });
}

FleetSessionInfo FleetApi::refreshFleetSession(int64_t gameTokenId, const ServerAddr& current,
                                               ApiCause cause) {
    return call(ApiKind::RefreshFleetSession, cause, [&](int&) {
        nlohmann::json input = {{"appId", std::to_string(appId_)},
                                {"gameTokenId", std::to_string(gameTokenId)}};
        if (!current.ip4.empty()) {
            input["currentServer"] = {{"ip4", current.ip4}, {"clientPort", current.port}};
        }
        const auto data =
            transport_->request(REFRESH_SESSION_MUTATION, {{"i", input}}, orgToken_);
        const auto& s = data.at("refreshFleetSession");
        FleetSessionInfo out;
        out.token = asToken(s.at("token"), "fleet token");
        out.gameTokenId = asI64(s.at("gameTokenId"), "gameTokenId");
        out.appId = appId_;
        out.expiresAtMs = asExpiry(s.at("expiresAt"));
        out.server = asServer(s.at("server"));
        return out;
    });
}

bool FleetApi::deleteManagedPlayer(int64_t orgId, const std::string& externalId,
                                   ApiCause cause) {
    return call(ApiKind::DeleteManagedPlayer, cause, [&](int&) {
        const nlohmann::json vars = {{"o", std::to_string(orgId)}, {"e", externalId}};
        const auto data = transport_->request(DELETE_PLAYER_MUTATION, vars, orgToken_);
        const auto& v = data.at("deleteManagedPlayer");
        return v.is_boolean() ? v.get<bool>() : false;
    });
}

void FleetApi::blockPlayer(const std::string& playerToken, int64_t blockedUserId,
                           ApiCause cause) {
    call(ApiKind::BlockPlayer, cause, [&](int&) {
        const nlohmann::json vars = {{"a", std::to_string(appId_)},
                                     {"u", std::to_string(blockedUserId)}};
        // The bearer is the blocking player's own app token, as from the player's device.
        transport_->request(BLOCK_PLAYER_MUTATION, vars, playerToken);
        return true;
    });
}

PlayerToken FleetApi::refreshAppToken(const std::string& playerToken, ApiCause cause) {
    return call(ApiKind::RefreshAppToken, cause, [&](int& httpCalls) {
        return transport_->refreshAppToken(playerToken, httpCalls);
    });
}

} // namespace lt::fleet
