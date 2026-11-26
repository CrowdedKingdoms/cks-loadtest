#include "fleet/FakeRealtime.hpp"

#include "Wire.hpp"
#include "fleet/FleetWire.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lt::fleet {

namespace {

constexpr size_t MAX_BUNDLE = MAX_CLIENT_DATAGRAM; // 1232: the largest bundle the server sends
constexpr int MAX_MEMBERS = 32;
constexpr int64_t CARRY_TIME_WINDOW_MS = 30000;
constexpr double CARRIES_PER_SECOND = 64.0;
constexpr int CARRY_BURST_ABOVE_MAX_PLAYERS = 32;
constexpr uint8_t ERR_INVALID_REQUEST = 15;

std::string isoUtc(int64_t epochMillis) {
    const time_t secs = static_cast<time_t>(epochMillis / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(epochMillis % 1000));
    return buf;
}

uint64_t fnv1a(const uint8_t* d, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) h = (h ^ d[i]) * 1099511628211ULL;
    return h;
}

bool isOrgToken(const std::string& bearer) {
    return bearer == FakeRealtime::ORG_TOKEN || bearer == FakeRealtime::OTHER_ORG_TOKEN;
}

/// The Application API as the client sees it: GraphQL documents in, `data` out.
class FakeTransport final : public ApiTransport {
public:
    explicit FakeTransport(FakeRealtime& fake) : fake_(fake) {}

    nlohmann::json request(const std::string& query, const nlohmann::json& variables,
                           const std::string& bearer) override {
        const std::string app = std::to_string(fake_.appId());
        auto has = [&](const char* field) { return query.find(field) != std::string::npos; };

        if (has("blockPlayer")) {
            // A player's own call, with the player's app token.
            if (!variables.at("a").is_string() || variables["a"].get<std::string>() != app)
                throw GraphQLError("appId must be this app's id", "BAD_USER_INPUT", false);
            fake_.apiBlockPlayer(bearer, std::stoll(variables.at("u").get<std::string>()));
            return {{"blockPlayer", {{"__typename", "PlayerBlock"}}}};
        }
        // Everything else is the organization's server, with an org token.
        if (!isOrgToken(bearer)) throw GraphQLError("Unauthenticated", "UNAUTHENTICATED", false);

        if (has("deleteManagedPlayer")) {
            if (variables.at("o").get<std::string>() != std::to_string(FakeRealtime::ORG_ID))
                throw GraphQLError("another organization", "FORBIDDEN", false);
            return {{"deleteManagedPlayer",
                     fake_.apiDeletePlayer(variables.at("e").get<std::string>())}};
        }

        const nlohmann::json& in = variables.at("i");
        // BigInt values must be decimal strings.
        if (!in.at("appId").is_string() || in["appId"].get<std::string>() != app) {
            throw GraphQLError("appId must be this app's id as a decimal string", "BAD_USER_INPUT",
                               false);
        }
        if (has("openFleetSession")) {
            return {{"openFleetSession", sessionJson(fake_.apiOpenSession(bearer), true)}};
        }
        if (has("refreshFleetSession")) {
            const int64_t id = std::stoll(in.at("gameTokenId").get<std::string>());
            const bool named = in.contains("currentServer") && in["currentServer"].is_object() &&
                               in["currentServer"].contains("ip4") &&
                               in["currentServer"].contains("clientPort");
            return {{"refreshFleetSession",
                     sessionJson(fake_.apiRefreshSession(bearer, id, named), false)}};
        }
        if (has("closeFleetSession")) {
            const int64_t id = std::stoll(in.at("gameTokenId").get<std::string>());
            return {{"closeFleetSession", fake_.apiCloseSession(bearer, id)}};
        }
        if (has("mintManagedPlayerToken")) {
            const std::string externalId = in.at("externalId").get<std::string>();
            if (externalId.empty() || !in.at("ageYears").is_number_integer() ||
                in.at("country") != "US" || !in.contains("gamertag")) {
                throw GraphQLError("unexpected mintManagedPlayerToken input", "BAD_USER_INPUT",
                                   false);
            }
            const PlayerToken t = fake_.apiMintPlayer(externalId, in["ageYears"].get<int>());
            return {{"mintManagedPlayerToken",
                     {{"token", t.token},
                      {"gameTokenId", std::to_string(t.gameTokenId)},
                      {"expiresAt", isoUtc(t.expiresAtMs)}}}};
        }
        throw GraphQLError("unknown operation", "GRAPHQL_VALIDATION_FAILED", false);
    }

    PlayerToken refreshAppToken(const std::string& playerToken, int& httpCalls) override {
        httpCalls = 1;
        const PlayerToken t = fake_.apiRefreshPlayer(playerToken);
        // The API has replaced the token; its answer is still on the way.
        if (fake_.options().refreshAnswerDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(fake_.options().refreshAnswerDelayMs));
        }
        return t;
    }

private:
    static nlohmann::json sessionJson(const FleetSessionInfo& s, bool opened) {
        nlohmann::json j = {{"token", s.token},
                            {"gameTokenId", std::to_string(s.gameTokenId)},
                            {"expiresAt", isoUtc(s.expiresAtMs)},
                            {"server", {{"ip4", s.server.ip4}, {"clientPort", s.server.port}}}};
        if (opened) {
            j["appId"] = std::to_string(s.appId);
            j["maxPlayers"] = s.maxPlayers;
        }
        return j;
    }
    FakeRealtime& fake_;
};

} // namespace

FakeRealtime::FakeRealtime(int64_t appId, Options options)
    : appId_(appId), opt_(options), rng_(options.seed) {
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
}

FakeRealtime::~FakeRealtime() {
    stop();
    for (auto& [id, s] : sessions_) {
        (void)id;
        if (s.fd >= 0) close(s.fd);
    }
    if (epollFd_ >= 0) close(epollFd_);
}

void FakeRealtime::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void FakeRealtime::stop() {
    const bool was = running_.exchange(false);
    if (was && thread_.joinable()) thread_.join();
}

std::unique_ptr<ApiTransport> FakeRealtime::transport() {
    return std::make_unique<FakeTransport>(*this);
}

Connector FakeRealtime::connector() {
    return [this](const FleetSessionInfo& info, std::string& error) { return connect(info, error); };
}

std::map<std::string, uint64_t> FakeRealtime::snapshot() const {
    return {
        {"datagramsIn", counters_.datagramsIn.load()},
        {"badHmac", counters_.badHmac.load()},
        {"carriesAccepted", counters_.carriesAccepted.load()},
        {"carriesRefused", counters_.carriesRefused.load()},
        {"carriesRepeated", counters_.carriesRepeated.load()},
        {"carriesReplayed", counters_.carriesReplayed.load()},
        {"carriesDropped", counters_.carriesDropped.load()},
        {"releasedSent", counters_.releasedSent.load()},
        {"releasedReason1", counters_.releasedReason1.load()},
        {"releasedReason5", counters_.releasedReason5.load()},
        {"releaseRequests", counters_.releaseRequests.load()},
        {"updatesAccepted", counters_.updatesAccepted.load()},
        {"updatesUnauthorized", counters_.updatesUnauthorized.load()},
        {"notificationsOut", counters_.notificationsOut.load()},
        {"ownStateOut", counters_.ownStateOut.load()},
        {"strayUnsigned", counters_.strayUnsigned.load()},
        {"strayUnsignedToSender", counters_.strayUnsignedToSender.load()},
        {"deliveriesOut", counters_.deliveriesOut.load()},
        {"deliveryPlayersOut", counters_.deliveryPlayersOut.load()},
        {"framesInSignedBundle", counters_.framesInSignedBundle.load()},
        {"framesWithOwnHmac", counters_.framesWithOwnHmac.load()},
        {"framesAloneOversize", counters_.framesAloneOversize.load()},
        {"datagramsOut", counters_.datagramsOut.load()},
        {"datagramsOutDropped", counters_.datagramsOutDropped.load()},
        {"largestDatagramOut", counters_.largestDatagramOut.load()},
        {"capabilities", counters_.capabilities.load()},
        {"boundAcksSent", counters_.boundAcksSent.load()},
        {"heartbeats", counters_.heartbeats.load()},
        {"playerHeartbeats", counters_.playerHeartbeats.load()},
        {"ownIdRefused", counters_.ownIdRefused.load()},
        {"capabilitiesUnderPlayerId", counters_.capabilitiesUnderPlayerId.load()},
        {"actorRefused", counters_.actorRefused.load()},
        {"voiceRefused", counters_.voiceRefused.load()},
        {"apiOpen", counters_.apiOpen.load()},
        {"apiMint", counters_.apiMint.load()},
        {"apiClose", counters_.apiClose.load()},
        {"apiRefreshSession", counters_.apiRefreshSession.load()},
        {"apiRefreshSessionWithoutServer", counters_.apiRefreshSessionWithoutServer.load()},
        {"apiRefreshAppToken", counters_.apiRefreshAppToken.load()},
        {"apiDelete", counters_.apiDelete.load()},
        {"apiBlock", counters_.apiBlock.load()},
        {"apiRefusedOtherOrgToken", counters_.apiRefusedOtherOrgToken.load()},
    };
}

uint64_t FakeRealtime::apiCalls() const {
    return counters_.apiOpen + counters_.apiMint + counters_.apiClose +
           counters_.apiRefreshSession + counters_.apiRefreshAppToken + counters_.apiDelete +
           counters_.apiBlock;
}

int FakeRealtime::carriedPlayers() {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(carries_.size());
}

int FakeRealtime::openSessions() {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(sessions_.size());
}

int FakeRealtime::players() {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(byExternalId_.size());
}

int FakeRealtime::playerOf(int n) {
    auto it = byExternalId_.find(playerExternalId(n));
    return it == byExternalId_.end() ? -1 : it->second;
}

int FakeRealtime::carrierOf(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = carries_.find(playerOf(player));
    if (it == carries_.end()) return -1;
    auto s = sessions_.find(it->second.sessionId);
    return s == sessions_.end() ? -1 : s->second.ordinal;
}

FakeRealtime::Session* FakeRealtime::byOrdinal(int ordinal) {
    if (ordinal < 0 || ordinal >= static_cast<int>(order_.size())) return nullptr;
    auto it = sessions_.find(order_[static_cast<size_t>(ordinal)]);
    return it == sessions_.end() ? nullptr : &it->second;
}

std::string FakeRealtime::randomToken() {
    static const char* alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<int> pick(0, 61);
    std::string t(64, 'x');
    for (auto& c : t) c = alphabet[pick(rng_)];
    return t;
}

double FakeRealtime::roll() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

int64_t FakeRealtime::serverNowMs() const { return epochMs() + opt_.clockSkewMs; }

bool FakeRealtime::blocked(int a, int b) const {
    // A block stops everything between two players, either way.
    return blocks_.count({a, b}) > 0 || blocks_.count({b, a}) > 0;
}

// ---------------------------------------------------------------------------
// The Application API
// ---------------------------------------------------------------------------

FleetSessionInfo FakeRealtime::apiOpenSession(const std::string& orgToken) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiOpen;
    Session s;
    s.ordinal = static_cast<int>(order_.size());
    s.id = nextFleetId_++;
    s.token = randomToken();
    s.openedBy = orgToken;
    s.port = nextPort_++;
    s.carryTokens = opt_.maxPlayers + CARRY_BURST_ABOVE_MAX_PLAYERS;
    s.carryRefillNs = nowNs();
    FleetSessionInfo info;
    info.token = s.token;
    info.gameTokenId = s.id;
    info.appId = appId_;
    info.expiresAtMs = epochMs() + 24LL * 3600 * 1000;
    info.maxPlayers = opt_.maxPlayers;
    info.server = ServerAddr{"127.0.0.1", static_cast<uint16_t>(s.port)};
    order_.push_back(s.id);
    sessions_.emplace(s.id, std::move(s));
    return info;
}

FleetSessionInfo FakeRealtime::apiRefreshSession(const std::string& orgToken, int64_t fleetId,
                                                 bool namedServer) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiRefreshSession;
    if (!namedServer) ++counters_.apiRefreshSessionWithoutServer;
    auto it = sessions_.find(fleetId);
    // Only the org token that opened the session keeps it. To any other the
    // session does not exist.
    if (it != sessions_.end() && it->second.openedBy != orgToken) {
        ++counters_.apiRefusedOtherOrgToken;
        it = sessions_.end();
    }
    if (it == sessions_.end()) {
        throw GraphQLError("Fleet session is not one of this org token's live sessions",
                           "NOT_FOUND", false);
    }
    Session& s = it->second;
    if (s.moved || !namedServer) {
        // Another server: it holds no carry, so the session carries every player again.
        s.moved = false;
        s.port = nextPort_++;
        dropCarriesOf(s);
    }
    // The same server: the token is authorized again and the carries stand.
    s.deauthorized = false;
    FleetSessionInfo info;
    info.token = s.token;
    info.gameTokenId = s.id;
    info.appId = appId_;
    info.expiresAtMs = epochMs() + 24LL * 3600 * 1000;
    info.maxPlayers = opt_.maxPlayers;
    info.server = ServerAddr{"127.0.0.1", static_cast<uint16_t>(s.port)};
    return info;
}

bool FakeRealtime::apiCloseSession(const std::string& orgToken, int64_t fleetId) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiClose;
    auto it = sessions_.find(fleetId);
    if (it == sessions_.end()) return false;
    if (it->second.openedBy != orgToken) {
        ++counters_.apiRefusedOtherOrgToken;
        return false;
    }
    // When the session itself ends, its carries end unannounced.
    dropCarriesOf(it->second);
    if (it->second.fd >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, it->second.fd, nullptr);
        byFd_.erase(it->second.fd);
        close(it->second.fd);
    }
    sessions_.erase(it);
    return true;
}

PlayerToken FakeRealtime::apiMintPlayer(const std::string& externalId, int ageYears) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiMint;
    ++mints_;
    if (opt_.mintBusyEvery > 0 && mints_ % opt_.mintBusyEvery == 0) {
        throw GraphQLError("The platform is busy", "PLATFORM_BUSY", false, 5);
    }
    if (opt_.mintRateLimitedEvery > 0 && mints_ % opt_.mintRateLimitedEvery == 0) {
        throw GraphQLError("Too many sign-ins", "RATE_LIMITED", false, 5);
    }
    if (ageYears < 13) throw GraphQLError("Age requirement not met", "AGE_REQUIREMENT_NOT_MET", false);
    // The player is created on its first sign-in; a stable externalId is the
    // same player the next time.
    auto known = byExternalId_.find(externalId);
    const bool isNew = known == byExternalId_.end();
    const int index = isNew ? nextPlayer_++ : known->second;
    byExternalId_[externalId] = index;
    Player& p = players_[index];
    p.index = index;
    p.externalId = externalId;
    if (isNew || p.tokenId == 0) p.minor = ageYears < 18; // the bracket is stored at creation
    if (p.tokenId) tokenOwner_.erase(p.tokenId); // a new sign-in replaces the token
    if (p.oldTokenId) tokenOwner_.erase(p.oldTokenId);
    p.oldTokenId = 0;
    p.oldToken.clear();
    p.token = randomToken();
    p.tokenId = nextTokenId_++;
    p.expiresAtMs = epochMs() + opt_.playerTokenTtlMs;
    p.ended = false;
    tokenOwner_[p.tokenId] = index;
    return PlayerToken{p.token, p.tokenId, p.expiresAtMs};
}

PlayerToken FakeRealtime::apiRefreshPlayer(const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiRefreshAppToken;
    for (auto& [n, p] : players_) {
        if (p.token != token) continue;
        if (p.ended || epochMs() >= p.expiresAtMs) break;
        if (p.oldTokenId) tokenOwner_.erase(p.oldTokenId);
        p.oldToken = p.token;
        p.oldTokenId = p.tokenId;
        p.oldDeadMs = epochMs() + opt_.oldTokenGraceMs;
        p.token = randomToken();
        p.tokenId = nextTokenId_++;
        p.expiresAtMs = epochMs() + opt_.playerTokenTtlMs;
        p.tokenKnownAtMs = epochMs() + opt_.newTokenLagMs;
        tokenOwner_[p.tokenId] = n;
        return PlayerToken{p.token, p.tokenId, p.expiresAtMs};
    }
    throw GraphQLError("Unauthenticated", "UNAUTHENTICATED", false);
}

bool FakeRealtime::apiDeletePlayer(const std::string& externalId) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiDelete;
    auto known = byExternalId_.find(externalId);
    if (known == byExternalId_.end()) return false;
    const int index = known->second;
    Player& p = players_[index];
    // Its sessions end, and its blocks go with it.
    p.ended = true;
    release(index, RELEASED_SIGN_IN_ENDED);
    if (p.tokenId) tokenOwner_.erase(p.tokenId);
    if (p.oldTokenId) tokenOwner_.erase(p.oldTokenId);
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        it = (it->first == index || it->second == index) ? blocks_.erase(it) : std::next(it);
    }
    for (auto it = actorOwner_.begin(); it != actorOwner_.end();) {
        it = it->second == index ? actorOwner_.erase(it) : std::next(it);
    }
    players_.erase(index);
    byExternalId_.erase(known);
    flushAll();
    return true;
}

void FakeRealtime::apiBlockPlayer(const std::string& playerToken, int64_t blockedUserId) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiBlock;
    int blocker = -1;
    for (const auto& [n, p] : players_) {
        if (p.token == playerToken && !p.ended && epochMs() < p.expiresAtMs) blocker = n;
    }
    if (blocker < 0) throw GraphQLError("Unauthenticated", "UNAUTHENTICATED", false);
    const int blockedIndex = static_cast<int>(blockedUserId - 5000);
    if (players_.find(blockedIndex) == players_.end())
        throw GraphQLError("The app does not know that player", "NOT_FOUND", false);
    blocks_.insert({blocker, blockedIndex});
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------

void FakeRealtime::tellSessionToMove(int ordinal, bool sessionReleased, uint8_t reason) {
    std::lock_guard<std::mutex> lock(mu_);
    Session* found = byOrdinal(ordinal);
    if (!found) return;
    Session& s = *found;
    flush(s);
    const auto* key = reinterpret_cast<const uint8_t*>(s.token.data());
    uint8_t frame[2 + CONTROL_NONCE_LEN + hmac::TAG_SIZE];
    size_t signedLen = 0;
    // A session that advertised BOUND_CONTROL is sent the bound forms.
    if (sessionReleased) {
        frame[0] = s.hasNonce ? COMMAND_SESSION_RELEASED_BOUND : COMMAND_SESSION_RELEASED;
        frame[1] = reason;
        signedLen = 2;
    } else {
        frame[0] = s.hasNonce ? COMMAND_RECONNECT_BOUND
                              : static_cast<uint8_t>(wire::COMMAND_RECONNECT);
        signedLen = 1;
    }
    if (s.hasNonce) {
        std::memcpy(frame + signedLen, s.nonce, CONTROL_NONCE_LEN);
        signedLen += CONTROL_NONCE_LEN;
    }
    hmacPlain(key, frame, signedLen, frame + signedLen);
    sendRaw(s, frame, signedLen + hmac::TAG_SIZE);
    // The session's flow is gone from this server, and so are its carries. The
    // server it left sends no FLEET_RELEASED for those.
    s.moved = true;
    dropCarriesOf(s);
}

void FakeRealtime::sendLegacyReconnect(int ordinal) {
    std::lock_guard<std::mutex> lock(mu_);
    Session* s = byOrdinal(ordinal);
    if (!s) return;
    flush(*s);
    uint8_t frame[1 + hmac::TAG_SIZE] = {wire::COMMAND_RECONNECT};
    hmacPlain(reinterpret_cast<const uint8_t*>(s->token.data()), frame, 1, frame + 1);
    sendRaw(*s, frame, sizeof(frame));
}

void FakeRealtime::deauthorize(int ordinal) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Session* s = byOrdinal(ordinal)) s->deauthorized = true;
}

void FakeRealtime::endSignIn(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    const int index = playerOf(player);
    auto it = players_.find(index);
    if (it == players_.end()) return;
    it->second.ended = true;
    release(index, RELEASED_SIGN_IN_ENDED);
    flushAll();
}

void FakeRealtime::idleRelease(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    release(playerOf(player), RELEASED_IDLE);
    flushAll();
}

void FakeRealtime::injectText(int ordinal, bool insideDelivery) {
    std::lock_guard<std::mutex> lock(mu_);
    Session* s = byOrdinal(ordinal);
    if (!s || s->carried.empty()) return;
    // An unsigned text notification, as a player's own session would be sent it.
    uint8_t notification[wire::HEADER_SIZE + 5 + wire::TAIL_NO_HMAC] = {};
    notification[0] = CLIENT_TEXT_NOTIFICATION;
    wire::writeI64(notification + wire::off::APP_ID, appId_);
    notification[wire::off::DISTANCE] = 8;
    std::memset(notification + wire::off::UUID, 'x', 32);
    std::memcpy(notification + wire::HEADER_SIZE, "hello", 5);
    wire::writeI64(notification + wire::HEADER_SIZE + 5, serverNowMs());
    if (!insideDelivery) {
        queueMember(*s, notification, sizeof(notification));
    } else {
        uint8_t frame[2 + 8 + sizeof(notification)] = {FLEET_DELIVERY, 1};
        wire::writeI64(frame + 2, carries_[s->carried.front()].tokenId);
        std::memcpy(frame + 10, notification, sizeof(notification));
        queueFleetFrame(*s, frame, sizeof(frame));
        ++counters_.deliveriesOut;
        ++counters_.deliveryPlayersOut;
    }
    flushAll();
}

void FakeRealtime::setForeign(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    // The player may not have signed in yet: make it, so the flag is there when it does.
    const std::string externalId = playerExternalId(player);
    auto known = byExternalId_.find(externalId);
    const int index = known != byExternalId_.end() ? known->second : nextPlayer_++;
    byExternalId_[externalId] = index;
    players_[index].index = index;
    players_[index].externalId = externalId;
    players_[index].foreign = true;
}

// ---------------------------------------------------------------------------
// The realtime side
// ---------------------------------------------------------------------------

int FakeRealtime::connect(const FleetSessionInfo& info, std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(info.gameTokenId);
    if (it == sessions_.end()) {
        error = "no such fleet session";
        return -1;
    }
    Session& s = it->second;
    if (info.server.port != s.port) {
        error = "the session is not authorized on that server";
        return -1;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) != 0) {
        error = std::string("socketpair: ") + std::strerror(errno);
        return -1;
    }
    for (int fd : pair) {
        int size = 4 * 1024 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
    if (s.fd >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, s.fd, nullptr);
        byFd_.erase(s.fd);
        close(s.fd);
    }
    s.fd = pair[0];
    // Capabilities and the control nonce belong to the flow: a new one starts without them.
    s.flags = 0;
    s.hasNonce = false;
    s.out.clear();
    s.members = 0;
    byFd_[s.fd] = s.id;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = s.fd;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, s.fd, &ev);
    return pair[1];
}

void FakeRealtime::run() {
    constexpr int MAX_EVENTS = 128;
    epoll_event events[MAX_EVENTS];
    uint8_t buf[2048];
    int64_t lastSweep = 0;
    while (running_.load(std::memory_order_acquire)) {
        const int n = epoll_wait(epollFd_, events, MAX_EVENTS, 1);
        std::lock_guard<std::mutex> lock(mu_);
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            for (int k = 0; k < 256; ++k) {
                auto owner = byFd_.find(fd);
                if (owner == byFd_.end()) break;
                auto sit = sessions_.find(owner->second);
                if (sit == sessions_.end()) break;
                const ssize_t got = recv(fd, buf, sizeof(buf), 0);
                if (got <= 0) break;
                ++counters_.datagramsIn;
                if (opt_.dropToServer > 0 && roll() < opt_.dropToServer) continue;
                onDatagram(sit->second, buf, static_cast<size_t>(got));
            }
        }
        const int64_t now = nowNs();
        if (now - lastSweep > 50 * 1000000LL) {
            lastSweep = now;
            sweepPlayers();
        }
        deliverHeld(now);
        flushAll();
    }
}

void FakeRealtime::onDatagram(Session& s, const uint8_t* d, size_t len) {
    if (s.moved) return; // this server no longer answers the session
    if (len > MAX_CLIENT_DATAGRAM) return;
    if (len < wire::HEADER_SIZE + wire::TAIL_WITH_HMAC || d[wire::off::CONTAINS_AUTH] != 1) return;
    const auto* key = reinterpret_cast<const uint8_t*>(s.token.data());
    const size_t hmacOff = len - wire::TAIL_WITH_HMAC;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmac::spatialSign(d, hmacOff, key, expected) ||
        !hmac::tagEquals(expected, d + hmacOff)) {
        ++counters_.badHmac; // not signed with this session's fleet token
        return;
    }
    const int64_t tailId = wire::readI64(d + len - 9);
    const uint8_t seq = d[len - 1];
    const uint8_t type = d[0];
    if (s.deauthorized) {
        queueError(s, seq, wire::ERR_INVALID_TOKEN);
        return;
    }
    if (wire::readI64(d + wire::off::APP_ID) != appId_) {
        queueError(s, seq, wire::ERR_INVALID_APP_ID);
        return;
    }

    if (tailId == s.id) {
        // Under its own game token id a session sends only 29, 49, 50 and 26.
        switch (type) {
            case wire::CLIENT_CAPABILITIES:
                onCapabilities(s, d, len);
                return;
            case wire::CLIENT_ACTOR_HEARTBEAT:
                ++counters_.heartbeats; // a keepalive: it registers no actor
                return;
            case FLEET_CARRY:
                if (len != FLEET_CARRY_SIZE) {
                    queueError(s, seq, ERR_INVALID_REQUEST);
                    return;
                }
                onCarry(s, d);
                return;
            case FLEET_RELEASE: {
                if (len != FLEET_RELEASE_SIZE) {
                    queueError(s, seq, ERR_INVALID_REQUEST);
                    return;
                }
                ++counters_.releaseRequests; // not answered; idempotent
                auto owner = tokenOwner_.find(wire::readI64(d + wire::HEADER_SIZE));
                if (owner == tokenOwner_.end()) return;
                auto c = carries_.find(owner->second);
                if (c != carries_.end() && c->second.sessionId == s.id) dropCarry(owner->second);
                return;
            }
            default:
                ++counters_.ownIdRefused;
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
        }
    }

    // A message for a player: the tail names the player, and the session must carry them.
    if (type == wire::CLIENT_CAPABILITIES) {
        ++counters_.capabilitiesUnderPlayerId;
        queueError(s, seq, ERR_INVALID_REQUEST);
        return;
    }
    auto owner = tokenOwner_.find(tailId);
    auto c = owner == tokenOwner_.end() ? carries_.end() : carries_.find(owner->second);
    if (c == carries_.end() || c->second.sessionId != s.id ||
        !liveToken(players_[owner->second], tailId, epochMs()) ||
        (opt_.strictTokenId && c->second.tokenId != tailId)) {
        // The session does not carry the player, or not under this token id: after
        // a carry with a refreshed token the player continues under the new id.
        if (type == wire::ACTOR_UPDATE_REQUEST_2) ++counters_.updatesUnauthorized;
        queueError(s, seq, wire::ERR_UNAUTHORIZED);
        return;
    }
    const int player = owner->second;

    // A message under an actor is accepted only if the player it names owns that actor.
    bool hasUuid = false;
    for (size_t i = wire::off::UUID; i < wire::HEADER_SIZE; ++i) hasUuid = hasUuid || d[i] != 0;
    if (hasUuid) {
        const std::string uuid(reinterpret_cast<const char*>(d + wire::off::UUID), 32);
        auto [it, fresh] = actorOwner_.emplace(uuid, player);
        (void)fresh;
        if (it->second != player) {
            ++counters_.actorRefused;
            if (type == wire::ACTOR_UPDATE_REQUEST_2) ++counters_.updatesUnauthorized;
            queueError(s, seq, wire::ERR_UNAUTHORIZED);
            return;
        }
    }

    switch (type) {
        case wire::CLIENT_ACTOR_HEARTBEAT:
            ++counters_.playerHeartbeats; // keeps the actor present; it is not gameplay input
            return;
        case wire::ACTOR_UPDATE_REQUEST_2:
            onUpdate(s, player, d, len, seq);
            return;
        case wire::CLIENT_TEXT_PACKET_2:
            onCommunication(s, player, d, len, seq, CLIENT_TEXT_NOTIFICATION);
            return;
        case CLIENT_AUDIO_PACKET:
            if (players_[player].minor) {
                // A player under 18 has voice off until a parent agrees.
                ++counters_.voiceRefused;
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
            }
            onCommunication(s, player, d, len, seq, CLIENT_AUDIO_NOTIFICATION);
            return;
        default:
            return;
    }
}

void FakeRealtime::onCapabilities(Session& s, const uint8_t* d, size_t len) {
    if (len != wire::CAPABILITIES_SIZE && len != CAPABILITIES_BOUND_SIZE) return;
    ++counters_.capabilities;
    uint32_t flags = 0;
    std::memcpy(&flags, d + wire::HEADER_SIZE, 4);
    if (!opt_.ignoreCapabilities) s.flags = flags;
    if ((flags & CAP_BOUND_CONTROL) && len == CAPABILITIES_BOUND_SIZE) {
        // The nonce follows the flags. Acknowledge it: from now on this session's
        // move commands come only in the bound forms.
        std::memcpy(s.nonce, d + wire::HEADER_SIZE + 4, CONTROL_NONCE_LEN);
        s.hasNonce = true;
        flush(s);
        uint8_t ack[1 + CONTROL_NONCE_LEN + hmac::TAG_SIZE] = {CONTROL_BOUND_ACK};
        std::memcpy(ack + 1, s.nonce, CONTROL_NONCE_LEN);
        hmacPlain(reinterpret_cast<const uint8_t*>(s.token.data()), ack, 1 + CONTROL_NONCE_LEN,
                  ack + 1 + CONTROL_NONCE_LEN);
        sendRaw(s, ack, sizeof(ack));
        ++counters_.boundAcksSent;
    }
}

const std::string* FakeRealtime::liveToken(const Player& p, int64_t tokenId,
                                           int64_t nowMs) const {
    if (p.ended) return nullptr;
    if (tokenId == p.tokenId && nowMs < p.expiresAtMs) return &p.token;
    if (tokenId == p.oldTokenId && nowMs < p.oldDeadMs) return &p.oldToken;
    return nullptr;
}

void FakeRealtime::onCarry(Session& s, const uint8_t* d) {
    // 64 carries a second sustained, burst max_players + 32. Past that a 49 is
    // dropped unanswered, and the session sends it again.
    if (opt_.carryRateLimit) {
        const int64_t now = nowNs();
        const double cap = opt_.maxPlayers + CARRY_BURST_ABOVE_MAX_PLAYERS;
        s.carryTokens = std::min(
            cap, s.carryTokens + static_cast<double>(now - s.carryRefillNs) * 1e-9 * CARRIES_PER_SECOND);
        s.carryRefillNs = now;
        if (s.carryTokens < 1.0) {
            ++counters_.carriesDropped;
            return;
        }
        s.carryTokens -= 1.0;
    }

    const uint8_t* payload = d + wire::HEADER_SIZE;
    const int64_t playerId = wire::readI64(payload);
    const int64_t when = wire::readI64(payload + 8);
    uint8_t status = CARRY_CARRIED;
    int n = -1;

    auto owner = tokenOwner_.find(playerId);
    const std::string* token = nullptr;
    if (owner != tokenOwner_.end()) {
        n = owner->second;
        token = liveToken(players_[n], playerId, epochMs());
    }
    if (token && opt_.oldTokenCarryRefused && playerId == players_[n].oldTokenId) {
        // A refresh has replaced this token: no new carry is made with it, though
        // the carry that stands under it goes on until the old token's time is up.
        token = nullptr;
    }
    if (token && playerId == players_[n].tokenId && epochMs() < players_[n].tokenKnownAtMs) {
        token = nullptr; // Options::newTokenLagMs: the realtime side has not heard of it yet
    }
    if (!token) {
        status = CARRY_NO_LIVE_TOKEN;
    } else if (players_[n].foreign) {
        status = CARRY_OTHER_ORGANIZATION;
    } else {
        uint8_t proof[hmac::TAG_SIZE];
        const int64_t skew = when - serverNowMs();
        auto standing = carries_.find(n);
        auto last = s.lastAcceptedMs.find(n);
        if (skew > CARRY_TIME_WINDOW_MS || skew < -CARRY_TIME_WINDOW_MS ||
            !carryProof(reinterpret_cast<const uint8_t*>(token->data()), s.id, playerId, when,
                        proof) ||
            !hmac::tagEquals(proof, payload + 16)) {
            status = CARRY_PROOF_OR_TIME;
        } else if (standing != carries_.end() && standing->second.sessionId == s.id &&
                   standing->second.tokenId == playerId && standing->second.acceptedMs == when) {
            // The same 49 again while its carry stands: answered as it was.
            ++counters_.carriesRepeated;
            uint8_t frame[CARRY_RESULT_SIZE] = {FLEET_CARRY_RESULT};
            wire::writeI64(frame + 1, playerId);
            wire::writeI64(frame + 9, 5000 + n);
            frame[17] = CARRY_CARRIED;
            sendCarryResult(s, frame);
            return;
        } else if (last != s.lastAcceptedMs.end() && when <= last->second) {
            // Sent again after that carry ended, or older than the session's last
            // accepted carry of the player: a replay.
            ++counters_.carriesReplayed;
            status = CARRY_PROOF_OR_TIME;
        } else {
            const bool mine = standing != carries_.end() && standing->second.sessionId == s.id;
            if (!mine && static_cast<int>(s.carried.size()) >= opt_.maxPlayers)
                status = CARRY_SESSION_FULL;
        }
    }

    if (status == CARRY_CARRIED) {
        // A carry belongs to the player, and the newest wins. The session that
        // carried before is told, unless it is this one.
        auto c = carries_.find(n);
        Carry next;
        next.sessionId = s.id;
        next.tokenId = playerId;
        next.acceptedMs = when;
        next.lastInputNs = nowNs();
        next.firstUpdatePending = c == carries_.end() || c->second.sessionId != s.id ||
                                  c->second.firstUpdatePending;
        if (c != carries_.end()) {
            next.cx = c->second.cx;
            next.cy = c->second.cy;
            next.cz = c->second.cz;
            next.placed = c->second.placed;
            next.lastInputNs = c->second.lastInputNs; // the idle clock is the player's
            next.lastStateHash = c->second.lastStateHash;
            if (c->second.sessionId != s.id) {
                const int64_t wasToken = c->second.tokenId;
                auto prev = sessions_.find(c->second.sessionId);
                if (prev != sessions_.end()) {
                    Session& old = prev->second;
                    old.carried.erase(std::remove(old.carried.begin(), old.carried.end(), n),
                                      old.carried.end());
                    old.boxDirty = true;
                    uint8_t frame[RELEASED_SIZE] = {FLEET_RELEASED};
                    wire::writeI64(frame + 1, wasToken);
                    frame[9] = RELEASED_CARRIED_ELSEWHERE;
                    queueFleetFrame(old, frame, sizeof(frame));
                    ++counters_.releasedSent;
                    ++counters_.releasedReason1;
                }
            }
        }
        carries_[n] = next;
        s.lastAcceptedMs[n] = when;
        if (std::find(s.carried.begin(), s.carried.end(), n) == s.carried.end())
            s.carried.push_back(n);
        s.boxDirty = true;
        ++counters_.carriesAccepted;
    } else {
        ++counters_.carriesRefused;
    }
    uint8_t frame[CARRY_RESULT_SIZE] = {FLEET_CARRY_RESULT};
    wire::writeI64(frame + 1, playerId);
    wire::writeI64(frame + 9, status == CARRY_CARRIED ? 5000 + n : 0); // the user id
    frame[17] = status;
    sendCarryResult(s, frame);
}

void FakeRealtime::sendCarryResult(Session& s, const uint8_t* frame) {
    if (opt_.carryResultDelayMs <= 0) {
        queueFleetFrame(s, frame, CARRY_RESULT_SIZE);
        return;
    }
    Held h;
    h.dueNs = nowNs() + opt_.carryResultDelayMs * 1000000LL;
    h.frame = true;
    h.sessionId = s.id;
    h.bytes.assign(frame, frame + CARRY_RESULT_SIZE);
    held_.push_back(std::move(h));
}

void FakeRealtime::deliverHeld(int64_t now) {
    // Each kind is held for one fixed time, but the two times differ, so the
    // queue is not strictly in order: take whatever is due.
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->dueNs > now) {
            ++it;
            continue;
        }
        auto s = sessions_.find(it->sessionId);
        if (s != sessions_.end()) {
            if (it->frame) {
                queueFleetFrame(s->second, it->bytes.data(), it->bytes.size());
            } else if (carries_.count(it->player)) {
                fanOut(s->second, it->player, it->bytes.data(), it->bytes.size(), it->seq);
            }
        }
        it = held_.erase(it);
    }
}

void FakeRealtime::onUpdate(Session& s, int n, const uint8_t* d, size_t len, uint8_t seq) {
    if (len > MAX_CLIENT_DATAGRAM || len < wire::HEADER_SIZE + wire::TAIL_WITH_HMAC) return;
    ++counters_.updatesAccepted;
    Carry& carry = carries_[n];
    const int64_t cx = wire::readI64(d + wire::off::CHUNK_X);
    const int64_t cy = wire::readI64(d + wire::off::CHUNK_Y);
    const int64_t cz = wire::readI64(d + wire::off::CHUNK_Z);
    if (!carry.placed || carry.cx != cx || carry.cy != cy || carry.cz != cz) {
        carry.cx = cx;
        carry.cy = cy;
        carry.cz = cz;
        carry.placed = true;
        s.boxDirty = true;
    }
    // An update that repeats an unchanged state is not gameplay input.
    const size_t bodyLen = len - wire::TAIL_WITH_HMAC;
    const uint64_t state = fnv1a(d + wire::off::CHUNK_X, bodyLen - wire::off::CHUNK_X);
    if (state != carry.lastStateHash) {
        carry.lastStateHash = state;
        carry.lastInputNs = nowNs();
    }
    if (opt_.stateDelayMs > 0) {
        // Held back: who receives it is decided when it goes out, and "its own
        // session" stays the session that sent it.
        Held h;
        h.dueNs = nowNs() + opt_.stateDelayMs * 1000000LL;
        h.sessionId = s.id;
        h.player = n;
        h.seq = seq;
        h.bytes.assign(d, d + len);
        held_.push_back(std::move(h));
        return;
    }
    fanOut(s, n, d, len, seq);
}

void FakeRealtime::fanOut(Session& s, int n, const uint8_t* d, size_t len, uint8_t seq) {
    const size_t bodyLen = len - wire::TAIL_WITH_HMAC;
    const int64_t cx = wire::readI64(d + wire::off::CHUNK_X);
    const int64_t cy = wire::readI64(d + wire::off::CHUNK_Y);
    const int64_t cz = wire::readI64(d + wire::off::CHUNK_Z);
    const int distance = d[wire::off::DISTANCE];

    // The notification: the same bytes with the type rewritten. Inside a signed
    // bundle a member has no HMAC of its own and its tail is epoch millis + seq.
    uint8_t unsignedMember[MAX_BUNDLE + 64];
    std::memcpy(unsignedMember, d, bodyLen);
    unsignedMember[0] = wire::ACTOR_UPDATE_NOTIFICATION_2;
    unsignedMember[wire::off::CONTAINS_AUTH] = 0;
    wire::writeI64(unsignedMember + bodyLen, serverNowMs());
    unsignedMember[bodyLen + 8] = seq;
    const size_t unsignedLen = bodyLen + wire::TAIL_NO_HMAC;

    auto carry = carries_.find(n);
    if (carry != carries_.end()) carry->second.firstUpdatePending = false;
    if (opt_.strayUnsignedEvery > 0 && ++updatesFannedOut_ % opt_.strayUnsignedEvery == 0 &&
        !order_.empty()) {
        // The defect: one more copy, alone in a datagram, with no HMAC, to the
        // sender itself or to any session at all.
        const bool toSender = rng_() % 4 == 0;
        const int64_t target = toSender ? s.id : order_[rng_() % order_.size()];
        auto r = sessions_.find(target);
        if (r != sessions_.end() && r->second.fd >= 0) {
            ++counters_.strayUnsigned;
            if (target == s.id) ++counters_.strayUnsignedToSender;
            sendRaw(r->second, unsignedMember, unsignedLen);
        }
    }

    // State goes out plain and ONCE to a session, however many of its players
    // are in range.
    for (auto& [id, r] : sessions_) {
        if (r.fd < 0 || r.moved || r.deauthorized) continue;
        const bool own = id == s.id;
        if (own) {
            // A session's own players' state comes back only if it asked, and
            // only when another of its players is in range.
            if (!((r.flags & CAP_FLEET_OWN_STATE) || opt_.ownStateBug)) continue;
            if (!anyInRange(r, cx, cy, cz, distance, n)) continue;
            ++counters_.ownStateOut;
        } else if (!anyInRange(r, cx, cy, cz, distance, -1)) {
            continue;
        }
        ++counters_.notificationsOut;
        if (r.flags & wire::CAP_BUNDLE_SIGNED) {
            queueMember(r, unsignedMember, unsignedLen);
        } else {
            // A session that did not advertise signed bundles is sent each
            // notification signed with its own token.
            uint8_t signedMember[MAX_BUNDLE + 64];
            std::memcpy(signedMember, d, bodyLen);
            signedMember[0] = wire::ACTOR_UPDATE_NOTIFICATION_2;
            hmac::spatialSign(signedMember, bodyLen,
                              reinterpret_cast<const uint8_t*>(r.token.data()),
                              signedMember + bodyLen);
            wire::writeI64(signedMember + bodyLen + hmac::TAG_SIZE, serverNowMs());
            signedMember[bodyLen + hmac::TAG_SIZE + 8] = seq;
            queueMember(r, signedMember, bodyLen + wire::TAIL_WITH_HMAC);
        }
    }
}

void FakeRealtime::onCommunication(Session& /*from*/, int sender, const uint8_t* d, size_t len,
                                   uint8_t seq, uint8_t notificationType) {
    if (len > MAX_CLIENT_DATAGRAM || len < wire::HEADER_SIZE + wire::TAIL_WITH_HMAC) return;
    const int64_t cx = wire::readI64(d + wire::off::CHUNK_X);
    const int64_t cy = wire::readI64(d + wire::off::CHUNK_Y);
    const int64_t cz = wire::readI64(d + wire::off::CHUNK_Z);
    const int distance = d[wire::off::DISTANCE];
    const bool voice = notificationType == CLIENT_AUDIO_NOTIFICATION;

    // What a player's own session would have been sent, unsigned: the long-spatial
    // message with its 9-byte tail, containsAuth 0 and no HMAC.
    uint8_t notification[MAX_BUNDLE + 64];
    const size_t bodyLen = len - wire::TAIL_WITH_HMAC;
    std::memcpy(notification, d, bodyLen);
    notification[0] = notificationType;
    notification[wire::off::CONTAINS_AUTH] = 0;
    wire::writeI64(notification + bodyLen, serverNowMs());
    notification[bodyLen + 8] = seq;
    const size_t notificationLen = bodyLen + wire::TAIL_NO_HMAC;

    // A communication always goes inside a FLEET_DELIVERY that lists the carried
    // players who may receive it. A message nobody may receive is not sent.
    for (auto& [id, r] : sessions_) {
        if (r.fd < 0 || r.moved || r.deauthorized) continue;
        std::vector<int64_t> listed;
        for (int n : r.carried) {
            auto c = carries_.find(n);
            if (c == carries_.end() || !c->second.placed) continue;
            if (std::llabs(c->second.cx - cx) > distance || std::llabs(c->second.cy - cy) > distance ||
                std::llabs(c->second.cz - cz) > distance)
                continue;
            if (n == sender) {
                // A carried sender's own text lists it; its own voice does not.
                if (voice) continue;
            } else {
                if (blocked(sender, n)) continue;                // a block, either way
                if (voice && players_[n].minor) continue;        // a minor's voice is off
            }
            listed.push_back(c->second.tokenId);
        }
        (void)id;
        // At most 32 players a frame; more are split over frames. A notification
        // so large that the frame would pass 1,242 bytes lists fewer.
        size_t perFrame = MAX_DELIVERY_PLAYERS;
        while (perFrame > 1 && 2 + 8 * perFrame + notificationLen > MAX_BUNDLED_DELIVERY &&
               2 + 8 * perFrame + notificationLen + hmac::TAG_SIZE > MAX_SERVER_DATAGRAM)
            --perFrame;
        for (size_t at = 0; at < listed.size(); at += perFrame) {
            const size_t count = std::min(perFrame, listed.size() - at);
            uint8_t frame[MAX_SERVER_DATAGRAM + 64];
            frame[0] = FLEET_DELIVERY;
            frame[1] = static_cast<uint8_t>(count);
            for (size_t i = 0; i < count; ++i) wire::writeI64(frame + 2 + 8 * i, listed[at + i]);
            std::memcpy(frame + 2 + 8 * count, notification, notificationLen);
            queueFleetFrame(r, frame, 2 + 8 * count + notificationLen);
            ++counters_.deliveriesOut;
            counters_.deliveryPlayersOut += count;
        }
        if (opt_.plainCommunicationBug && !listed.empty() && (r.flags & wire::CAP_BUNDLE_SIGNED)) {
            queueMember(r, notification, notificationLen); // the defect: plain as well
        }
    }
}

void FakeRealtime::refreshBox(Session& s) {
    s.boxDirty = false;
    s.hasBox = false;
    for (int n : s.carried) {
        auto c = carries_.find(n);
        if (c == carries_.end() || !c->second.placed) continue;
        if (!s.hasBox) {
            s.minX = s.maxX = c->second.cx;
            s.minY = s.maxY = c->second.cy;
            s.hasBox = true;
        } else {
            s.minX = std::min(s.minX, c->second.cx);
            s.maxX = std::max(s.maxX, c->second.cx);
            s.minY = std::min(s.minY, c->second.cy);
            s.maxY = std::max(s.maxY, c->second.cy);
        }
    }
}

bool FakeRealtime::anyInRange(Session& s, int64_t cx, int64_t cy, int64_t cz, int distance,
                              int except) {
    if (s.boxDirty) refreshBox(s);
    if (!s.hasBox) return false;
    if (cx < s.minX - distance || cx > s.maxX + distance || cy < s.minY - distance ||
        cy > s.maxY + distance)
        return false;
    for (int n : s.carried) {
        if (n == except) continue;
        auto c = carries_.find(n);
        if (c == carries_.end() || !c->second.placed) continue;
        if (std::llabs(c->second.cx - cx) <= distance && std::llabs(c->second.cy - cy) <= distance &&
            std::llabs(c->second.cz - cz) <= distance)
            return true;
    }
    return false;
}

void FakeRealtime::dropCarry(int player) {
    auto c = carries_.find(player);
    if (c == carries_.end()) return;
    auto s = sessions_.find(c->second.sessionId);
    if (s != sessions_.end()) {
        auto& list = s->second.carried;
        list.erase(std::remove(list.begin(), list.end(), player), list.end());
        s->second.boxDirty = true;
    }
    carries_.erase(c);
}

void FakeRealtime::dropCarriesOf(Session& s) {
    const std::vector<int> carried = s.carried;
    for (int n : carried) dropCarry(n);
    s.lastAcceptedMs.clear();
}

void FakeRealtime::release(int player, uint8_t reason) {
    auto c = carries_.find(player);
    if (c == carries_.end()) return;
    auto s = sessions_.find(c->second.sessionId);
    if (s != sessions_.end()) {
        uint8_t frame[RELEASED_SIZE] = {FLEET_RELEASED};
        wire::writeI64(frame + 1, c->second.tokenId);
        frame[9] = reason;
        queueFleetFrame(s->second, frame, sizeof(frame));
        ++counters_.releasedSent;
        if (reason == RELEASED_IDLE) ++counters_.releasedReason5;
    }
    dropCarry(player);
}

void FakeRealtime::sweepPlayers() {
    // A carried player whose token expired, and who was not carried again with a
    // live one, is released with reason 2. One with no gameplay input for the
    // app's idle timeout is released with reason 5.
    const int64_t nowMs = epochMs();
    const int64_t now = nowNs();
    std::vector<int> ended, idle;
    for (const auto& [n, c] : carries_) {
        auto p = players_.find(n);
        if (p == players_.end() || !liveToken(p->second, c.tokenId, nowMs)) {
            ended.push_back(n);
        } else if (opt_.idleTimeoutMs > 0 &&
                   now - c.lastInputNs > opt_.idleTimeoutMs * 1000000LL) {
            idle.push_back(n);
        }
    }
    for (int n : ended) release(n, RELEASED_SIGN_IN_ENDED);
    for (int n : idle) release(n, RELEASED_IDLE);
}

// ---------------------------------------------------------------------------
// Frames to a session
// ---------------------------------------------------------------------------

void FakeRealtime::queueFleetFrame(Session& s, const uint8_t* frame, size_t len) {
    const bool signedBundles = (s.flags & wire::CAP_BUNDLE_SIGNED) != 0;
    if (signedBundles && len <= MAX_BUNDLED_DELIVERY) {
        // A member of a MESSAGE_BUNDLE_SIGNED, with no HMAC of its own.
        ++counters_.framesInSignedBundle;
        queueMember(s, frame, len);
        return;
    }
    // Its own HMAC follows the frame: HMAC(fleet token, frame || fleet token).
    uint8_t withHmac[MAX_SERVER_DATAGRAM + 64];
    std::memcpy(withHmac, frame, len);
    hmac::spatialSign(withHmac, len, reinterpret_cast<const uint8_t*>(s.token.data()),
                      withHmac + len);
    ++framesWithHmac_;
    if (opt_.corruptFrameHmacEvery > 0 && framesWithHmac_ % opt_.corruptFrameHmacEvery == 0)
        withHmac[len + 5] ^= 0x40;
    ++counters_.framesWithOwnHmac;
    const size_t total = len + hmac::TAG_SIZE;
    if (signedBundles || opt_.framesAlone || total + 3 > MAX_BUNDLE) {
        // Alone in its datagram. For a session that advertised signed bundles
        // this is a FLEET_DELIVERY too large to be a member of one.
        if (signedBundles) ++counters_.framesAloneOversize;
        flush(s);
        sendRaw(s, withHmac, total);
        return;
    }
    // A member of a MESSAGE_BUNDLE, whose length prefix counts the frame and its HMAC.
    queueMember(s, withHmac, total);
}

void FakeRealtime::queueError(Session& s, uint8_t seq, uint8_t code) {
    const uint8_t frame[3] = {wire::GENERIC_ERROR_MESSAGE, seq, code};
    queueMember(s, frame, sizeof(frame));
}

void FakeRealtime::queueMember(Session& s, const uint8_t* m, size_t len) {
    const bool signedBundle = (s.flags & wire::CAP_BUNDLE_SIGNED) != 0;
    const size_t tail = signedBundle ? hmac::TAG_SIZE : 0;
    if (s.members > 0 && (s.out.size() + 2 + len + tail > MAX_BUNDLE || s.members >= MAX_MEMBERS))
        flush(s);
    if (s.members == 0) {
        s.out.clear();
        s.out.push_back(signedBundle ? wire::MESSAGE_BUNDLE_SIGNED : wire::MESSAGE_BUNDLE);
        dirty_.push_back(s.id);
    }
    s.out.push_back(static_cast<uint8_t>(len & 0xFF));
    s.out.push_back(static_cast<uint8_t>(len >> 8));
    s.out.insert(s.out.end(), m, m + len);
    ++s.members;
}

void FakeRealtime::flush(Session& s) {
    if (s.members == 0) return;
    if (s.out[0] == wire::MESSAGE_BUNDLE_SIGNED) {
        uint8_t mac[hmac::TAG_SIZE];
        hmac::spatialSign(s.out.data(), s.out.size(),
                          reinterpret_cast<const uint8_t*>(s.token.data()), mac);
        s.out.insert(s.out.end(), mac, mac + hmac::TAG_SIZE);
    }
    sendRaw(s, s.out.data(), s.out.size());
    s.out.clear();
    s.members = 0;
}

void FakeRealtime::flushAll() {
    for (int64_t id : dirty_) {
        auto it = sessions_.find(id);
        if (it != sessions_.end()) flush(it->second);
    }
    dirty_.clear();
}

void FakeRealtime::sendRaw(Session& s, const uint8_t* d, size_t len) {
    if (s.fd < 0) return;
    if (opt_.dropToClient > 0 && roll() < opt_.dropToClient) {
        ++counters_.datagramsOutDropped;
        return;
    }
    if (send(s.fd, d, len, 0) == static_cast<ssize_t>(len)) {
        ++counters_.datagramsOut;
        uint64_t prev = counters_.largestDatagramOut.load();
        while (len > prev && !counters_.largestDatagramOut.compare_exchange_weak(prev, len)) {
        }
    } else {
        ++counters_.datagramsOutDropped; // the session's queue is full, as a lost datagram
    }
}

} // namespace lt::fleet
