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

constexpr size_t MAX_BUNDLE = 1232;
constexpr int MAX_MEMBERS = 32;
constexpr int64_t CARRY_TIME_WINDOW_MS = 30000;

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

/// The Application API as the client sees it: GraphQL documents in, `data` out.
class FakeTransport final : public ApiTransport {
public:
    explicit FakeTransport(FakeRealtime& fake) : fake_(fake) {}

    nlohmann::json request(const std::string& query, const nlohmann::json& variables,
                           const std::string& bearer) override {
        if (bearer != FakeRealtime::ORG_TOKEN) {
            throw GraphQLError("Unauthenticated", "UNAUTHENTICATED", false);
        }
        const nlohmann::json& in = variables.at("i");
        // BigInt values must be decimal strings.
        if (!in.at("appId").is_string() ||
            in["appId"].get<std::string>() != std::to_string(fake_.appId())) {
            throw GraphQLError("appId must be this app's id as a decimal string", "BAD_USER_INPUT",
                               false);
        }
        if (query.find("openFleetSession") != std::string::npos) {
            return {{"openFleetSession", sessionJson(fake_.apiOpenSession(), true)}};
        }
        if (query.find("refreshFleetSession") != std::string::npos) {
            const int64_t id = std::stoll(in.at("gameTokenId").get<std::string>());
            const bool named = in.contains("currentServer") && in["currentServer"].is_object() &&
                               in["currentServer"].contains("ip4") &&
                               in["currentServer"].contains("clientPort");
            return {{"refreshFleetSession", sessionJson(fake_.apiRefreshSession(id, named), false)}};
        }
        if (query.find("closeFleetSession") != std::string::npos) {
            const int64_t id = std::stoll(in.at("gameTokenId").get<std::string>());
            return {{"closeFleetSession", fake_.apiCloseSession(id)}};
        }
        if (query.find("mintManagedPlayerToken") != std::string::npos) {
            const std::string ext = in.at("externalId").get<std::string>();
            const std::string prefix = "fleet-load-";
            if (ext.compare(0, prefix.size(), prefix) != 0 || in.at("ageYears") != 30 ||
                in.at("country") != "US" || !in.contains("gamertag")) {
                throw GraphQLError("unexpected mintManagedPlayerToken input", "BAD_USER_INPUT",
                                   false);
            }
            const PlayerToken t = fake_.apiMintPlayer(std::stoi(ext.substr(prefix.size())));
            return {{"mintManagedPlayerToken",
                     {{"token", t.token},
                      {"gameTokenId", std::to_string(t.gameTokenId)},
                      {"expiresAt", isoUtc(t.expiresAtMs)}}}};
        }
        throw GraphQLError("unknown operation", "GRAPHQL_VALIDATION_FAILED", false);
    }

    PlayerToken refreshAppToken(const std::string& playerToken, int& httpCalls) override {
        httpCalls = 1;
        return fake_.apiRefreshPlayer(playerToken);
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

uint64_t FakeRealtime::apiCalls() const {
    return counters_.apiOpen + counters_.apiMint + counters_.apiClose +
           counters_.apiRefreshSession + counters_.apiRefreshAppToken;
}

int FakeRealtime::carriedPlayers() {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(carries_.size());
}

int FakeRealtime::openSessions() {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(sessions_.size());
}

int FakeRealtime::carrierOf(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = carries_.find(player);
    if (it == carries_.end()) return -1;
    auto s = sessions_.find(it->second.sessionId);
    return s == sessions_.end() ? -1 : s->second.ordinal;
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

// ---------------------------------------------------------------------------
// The Application API
// ---------------------------------------------------------------------------

FleetSessionInfo FakeRealtime::apiOpenSession() {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiOpen;
    Session s;
    s.ordinal = static_cast<int>(order_.size());
    s.id = nextFleetId_++;
    s.token = randomToken();
    s.port = nextPort_++;
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

FleetSessionInfo FakeRealtime::apiRefreshSession(int64_t fleetId, bool namedServer) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiRefreshSession;
    if (!namedServer) ++counters_.apiRefreshSessionWithoutServer;
    auto it = sessions_.find(fleetId);
    if (it == sessions_.end()) {
        throw GraphQLError("Fleet session is not one of this organization's live sessions",
                           "NOT_FOUND", false);
    }
    Session& s = it->second;
    if (s.moved) {
        s.moved = false;
        s.port = nextPort_++; // a new server to connect to
    }
    FleetSessionInfo info;
    info.token = s.token;
    info.gameTokenId = s.id;
    info.appId = appId_;
    info.expiresAtMs = epochMs() + 24LL * 3600 * 1000;
    info.maxPlayers = opt_.maxPlayers;
    info.server = ServerAddr{"127.0.0.1", static_cast<uint16_t>(s.port)};
    return info;
}

bool FakeRealtime::apiCloseSession(int64_t fleetId) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiClose;
    auto it = sessions_.find(fleetId);
    if (it == sessions_.end()) return false;
    // Ending a session ends its carries.
    const std::vector<int> carried = it->second.carried;
    for (int n : carried) carries_.erase(n);
    if (it->second.fd >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, it->second.fd, nullptr);
        byFd_.erase(it->second.fd);
        close(it->second.fd);
    }
    sessions_.erase(it);
    return true;
}

PlayerToken FakeRealtime::apiMintPlayer(int n) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.apiMint;
    ++mints_;
    if (opt_.mintBusyEvery > 0 && mints_ % opt_.mintBusyEvery == 0) {
        throw GraphQLError("The platform is busy", "PLATFORM_BUSY", false, 5);
    }
    if (opt_.mintRateLimitedEvery > 0 && mints_ % opt_.mintRateLimitedEvery == 0) {
        throw GraphQLError("Too many sign-ins", "RATE_LIMITED", false, 5);
    }
    Player& p = players_[n];
    p.n = n;
    if (p.tokenId) tokenOwner_.erase(p.tokenId); // a new sign-in replaces the token
    if (p.oldTokenId) tokenOwner_.erase(p.oldTokenId);
    p.oldTokenId = 0;
    p.oldToken.clear();
    p.token = randomToken();
    p.tokenId = nextTokenId_++;
    p.expiresAtMs = epochMs() + opt_.playerTokenTtlMs;
    p.ended = false;
    tokenOwner_[p.tokenId] = n;
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
        tokenOwner_[p.tokenId] = n;
        return PlayerToken{p.token, p.tokenId, p.expiresAtMs};
    }
    throw GraphQLError("Unauthenticated", "UNAUTHENTICATED", false);
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------

void FakeRealtime::tellSessionToMove(int ordinal, bool sessionReleased, uint8_t reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (ordinal < 0 || ordinal >= static_cast<int>(order_.size())) return;
    auto it = sessions_.find(order_[static_cast<size_t>(ordinal)]);
    if (it == sessions_.end()) return;
    Session& s = it->second;
    flush(s);
    const auto* key = reinterpret_cast<const uint8_t*>(s.token.data());
    uint8_t frame[2 + hmac::TAG_SIZE];
    size_t len = 0;
    if (sessionReleased) {
        frame[0] = COMMAND_SESSION_RELEASED;
        frame[1] = reason;
        hmacPlain(key, frame, 2, frame + 2);
        len = 2 + hmac::TAG_SIZE;
    } else {
        frame[0] = wire::COMMAND_RECONNECT;
        hmacPlain(key, frame, 1, frame + 1);
        len = 1 + hmac::TAG_SIZE;
    }
    sendRaw(s, frame, len);
    // The session's flow is gone from this server, and so are its carries, until
    // the session comes back through the API and carries its players again.
    s.moved = true;
    const std::vector<int> carried = s.carried;
    for (int n : carried) dropCarry(n);
}

void FakeRealtime::endSignIn(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = players_.find(player);
    if (it == players_.end()) return;
    it->second.ended = true;
    release(player, RELEASED_SIGN_IN_ENDED);
    flushAll();
}

void FakeRealtime::idleRelease(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    release(player, RELEASED_IDLE);
    flushAll();
}

void FakeRealtime::setForeign(int player) {
    std::lock_guard<std::mutex> lock(mu_);
    players_[player].foreign = true;
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
    s.flags = 0; // capabilities belong to the flow: a new one starts without them
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
            sweepTokens();
        }
        flushAll();
    }
}

void FakeRealtime::onDatagram(Session& s, const uint8_t* d, size_t len) {
    if (s.moved) return; // this server no longer answers the session
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
    if (wire::readI64(d + wire::off::APP_ID) != appId_) {
        queueError(s, seq, wire::ERR_INVALID_APP_ID);
        return;
    }
    switch (d[0]) {
        case wire::CLIENT_CAPABILITIES:
            if (tailId != s.id || len != wire::CAPABILITIES_SIZE) {
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
            }
            ++counters_.capabilities;
            if (!opt_.ignoreCapabilities) std::memcpy(&s.flags, d + wire::HEADER_SIZE, 4);
            return;
        case wire::CLIENT_ACTOR_HEARTBEAT:
            if (tailId != s.id) {
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
            }
            ++counters_.heartbeats;
            return;
        case FLEET_CARRY:
            if (tailId != s.id || len != FLEET_CARRY_SIZE) {
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
            }
            onCarry(s, d, len);
            return;
        case FLEET_RELEASE: {
            if (tailId != s.id || len != FLEET_RELEASE_SIZE) {
                queueError(s, seq, wire::ERR_UNAUTHORIZED);
                return;
            }
            ++counters_.releaseRequests;
            auto owner = tokenOwner_.find(wire::readI64(d + wire::HEADER_SIZE));
            if (owner == tokenOwner_.end()) return;
            auto c = carries_.find(owner->second);
            if (c != carries_.end() && c->second.sessionId == s.id) dropCarry(owner->second);
            return;
        }
        case wire::ACTOR_UPDATE_REQUEST_2:
            onUpdate(s, d, len, tailId, seq);
            return;
        default:
            return;
    }
}

const std::string* FakeRealtime::liveToken(const Player& p, int64_t tokenId,
                                           int64_t nowMs) const {
    if (p.ended) return nullptr;
    if (tokenId == p.tokenId && nowMs < p.expiresAtMs) return &p.token;
    if (tokenId == p.oldTokenId && nowMs < p.oldDeadMs) return &p.oldToken;
    return nullptr;
}

void FakeRealtime::onCarry(Session& s, const uint8_t* d, size_t /*len*/) {
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
    if (!token) {
        status = CARRY_NO_LIVE_TOKEN;
    } else if (players_[n].foreign) {
        status = CARRY_OTHER_ORG_OR_APP;
    } else {
        uint8_t proof[hmac::TAG_SIZE];
        const int64_t skew = when - serverNowMs();
        if (skew > CARRY_TIME_WINDOW_MS || skew < -CARRY_TIME_WINDOW_MS ||
            !carryProof(reinterpret_cast<const uint8_t*>(token->data()), s.id, playerId, when,
                        proof) ||
            !hmac::tagEquals(proof, payload + 16)) {
            status = CARRY_PROOF_OR_TIME;
        } else {
            auto c = carries_.find(n);
            const bool mine = c != carries_.end() && c->second.sessionId == s.id;
            if (!mine && static_cast<int>(s.carried.size()) >= opt_.maxPlayers)
                status = CARRY_SESSION_FULL;
        }
    }

    if (status == CARRY_CARRIED) {
        // The newest carry wins. The session that carried before is told, unless
        // it is this one.
        auto c = carries_.find(n);
        Carry next;
        next.sessionId = s.id;
        next.tokenId = playerId;
        if (c != carries_.end()) {
            next.cx = c->second.cx;
            next.cy = c->second.cy;
            next.cz = c->second.cz;
            next.placed = c->second.placed;
            if (c->second.sessionId != s.id) {
                const int64_t wasToken = c->second.tokenId;
                auto prev = sessions_.find(c->second.sessionId);
                if (prev != sessions_.end()) {
                    Session& old = prev->second;
                    old.carried.erase(std::remove(old.carried.begin(), old.carried.end(), n),
                                      old.carried.end());
                    old.boxDirty = true;
                    uint8_t body[9];
                    wire::writeI64(body, wasToken);
                    body[8] = RELEASED_CARRIED_ELSEWHERE;
                    queueFleetFrame(old, FLEET_RELEASED, body, sizeof(body));
                    ++counters_.releasedSent;
                    ++counters_.releasedReason1;
                }
            }
        }
        carries_[n] = next;
        if (std::find(s.carried.begin(), s.carried.end(), n) == s.carried.end())
            s.carried.push_back(n);
        s.boxDirty = true;
        ++counters_.carriesAccepted;
    } else {
        ++counters_.carriesRefused;
    }
    uint8_t body[17];
    wire::writeI64(body, playerId);
    wire::writeI64(body + 8, status == CARRY_CARRIED ? 5000 + n : 0); // the user id
    body[16] = status;
    queueFleetFrame(s, FLEET_CARRY_RESULT, body, sizeof(body));
}

void FakeRealtime::onUpdate(Session& s, const uint8_t* d, size_t len, int64_t tailId,
                            uint8_t seq) {
    if (len > MAX_BUNDLE) return; // larger than any datagram a client may send
    // The tail names the player the message is for. The session must carry them.
    auto owner = tokenOwner_.find(tailId);
    auto c = owner == tokenOwner_.end() ? carries_.end() : carries_.find(owner->second);
    if (c == carries_.end() || c->second.sessionId != s.id ||
        !liveToken(players_[owner->second], tailId, epochMs())) {
        ++counters_.updatesUnauthorized;
        queueError(s, seq, wire::ERR_UNAUTHORIZED);
        return;
    }
    ++counters_.updatesAccepted;
    const int n = owner->second;
    const int64_t cx = wire::readI64(d + wire::off::CHUNK_X);
    const int64_t cy = wire::readI64(d + wire::off::CHUNK_Y);
    const int64_t cz = wire::readI64(d + wire::off::CHUNK_Z);
    if (!c->second.placed || c->second.cx != cx || c->second.cy != cy || c->second.cz != cz) {
        c->second.cx = cx;
        c->second.cy = cy;
        c->second.cz = cz;
        c->second.placed = true;
        s.boxDirty = true;
    }
    const int distance = d[wire::off::DISTANCE];

    // The notification: the same bytes with the type rewritten. Inside a signed
    // bundle a member has no HMAC of its own and its tail is epoch millis + seq.
    uint8_t unsignedMember[MAX_BUNDLE + 64];
    const size_t bodyLen = len - wire::TAIL_WITH_HMAC;
    std::memcpy(unsignedMember, d, bodyLen);
    unsignedMember[0] = wire::ACTOR_UPDATE_NOTIFICATION_2;
    unsignedMember[wire::off::CONTAINS_AUTH] = 0;
    wire::writeI64(unsignedMember + bodyLen, serverNowMs());
    unsignedMember[bodyLen + 8] = seq;
    const size_t unsignedLen = bodyLen + wire::TAIL_NO_HMAC;

    for (auto& [id, r] : sessions_) {
        if (r.fd < 0 || r.moved) continue;
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

void FakeRealtime::release(int player, uint8_t reason) {
    auto c = carries_.find(player);
    if (c == carries_.end()) return;
    auto s = sessions_.find(c->second.sessionId);
    if (s != sessions_.end()) {
        uint8_t body[9];
        wire::writeI64(body, c->second.tokenId);
        body[8] = reason;
        queueFleetFrame(s->second, FLEET_RELEASED, body, sizeof(body));
        ++counters_.releasedSent;
    }
    dropCarry(player);
}

void FakeRealtime::sweepTokens() {
    // A carried player whose token expired, and who was not carried again with a
    // live one, is released with reason 2.
    const int64_t now = epochMs();
    std::vector<int> gone;
    for (const auto& [n, c] : carries_) {
        auto p = players_.find(n);
        if (p == players_.end() || !liveToken(p->second, c.tokenId, now)) gone.push_back(n);
    }
    for (int n : gone) release(n, RELEASED_SIGN_IN_ENDED);
}

// ---------------------------------------------------------------------------
// Frames to a session
// ---------------------------------------------------------------------------

void FakeRealtime::queueFleetFrame(Session& s, uint8_t type, const uint8_t* payload,
                                   size_t len) {
    uint8_t frame[256];
    if (opt_.longFormFleetFrames) {
        // The same bytes as the app payload of a long-form message.
        const size_t total = wire::HEADER_SIZE + len + wire::TAIL_NO_HMAC;
        std::memset(frame, 0, total);
        frame[0] = type;
        wire::writeI64(frame + wire::off::APP_ID, appId_);
        std::memcpy(frame + wire::HEADER_SIZE, payload, len);
        wire::writeI64(frame + wire::HEADER_SIZE + len, serverNowMs());
        queueMember(s, frame, total);
        return;
    }
    frame[0] = type;
    std::memcpy(frame + 1, payload, len);
    queueMember(s, frame, len + 1);
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
    } else {
        ++counters_.datagramsOutDropped; // the session's queue is full, as a lost datagram
    }
}

} // namespace lt::fleet
