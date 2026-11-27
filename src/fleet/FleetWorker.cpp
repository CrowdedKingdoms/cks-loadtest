#include "fleet/FleetWorker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lt::fleet {

namespace {

constexpr int64_t MS = 1000000;          // nanoseconds
constexpr int64_t SEC = 1000 * MS;
constexpr double UE5_HEIGHT_OFFSET = -60.0;  // where the existing harness stands its players
constexpr double CONFINED_SPEED = 0.3;       // chunks a second, when nobody is to cross
constexpr int64_t MOVE_COOLDOWN_NS = 5 * SEC;
constexpr int64_t MOVE_RETRY_NS = 5 * SEC;
constexpr int64_t TOKEN_WAIT_RETRY_NS = 10 * SEC;
constexpr int64_t LATE_ENTRY_NS = 15 * SEC;
constexpr int64_t UNPAIRED_ENTRY_NS = 30 * SEC;
constexpr int ERROR_STREAK_MOVE = 20;
constexpr int SEND_ERROR_STREAK_MOVE = 100;
constexpr int MAX_DATAGRAMS_PER_WAKE = 512;

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

void inc(Counter& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

double msBetween(int64_t laterNs, int64_t earlierNs) {
    return std::round(static_cast<double>(laterNs - earlierNs) / 1e4) / 100.0; // two decimals
}

/// A marker written by an earlier version of this tool (2^40 + ...).
bool isEarlierMarker(const uint8_t* state, size_t len) {
    if (len != wire::state::SIZE || state[wire::state::VERSION] != wire::state::VERSION_VALUE)
        return false;
    const double code = wire::readF64(state + wire::state::ROTATION_PITCH);
    return code >= 1099511627776.0 && code < 2199023255552.0 && code == std::floor(code);
}

const char* whereName(int where) {
    return where == 0 ? "signed_bundle" : where == 1 ? "plain_bundle" : "standalone";
}

} // namespace

int connectUdp(const FleetSessionInfo& info, int rcvbufBytes, std::string& error) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(info.server.port);
    if (inet_pton(AF_INET, info.server.ip4.c_str(), &addr.sin_addr) != 1) {
        error = "not an IPv4 address: " + info.server.ip4;
        close(fd);
        return -1;
    }
    if (rcvbufBytes > 0) {
        // Before connect(), as the harness does; the kernel clamps to net.core.rmem_max.
        int want = rcvbufBytes;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    }
    // connect() pins the peer: send() is cheap and recv() returns only that server's datagrams.
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = std::string("connect: ") + std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// SharedCreds
// ---------------------------------------------------------------------------

void SharedCreds::store(const PlayerToken& t) {
    version_.fetch_add(1, std::memory_order_acq_rel); // odd: a write is in progress
    for (int i = 0; i < 8; ++i) {
        uint64_t w = 0;
        std::memcpy(&w, t.token.data() + i * 8, 8);
        words_[i].store(w, std::memory_order_relaxed);
    }
    gameTokenId_.store(t.gameTokenId, std::memory_order_relaxed);
    expiresAtMs_.store(t.expiresAtMs, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_acq_rel); // even: stable
}

bool SharedCreds::load(uint8_t* token64, int64_t& gameTokenId, uint32_t& version) const {
    for (;;) {
        const uint32_t v1 = version_.load(std::memory_order_acquire);
        if (v1 == 0) return false;
        if (v1 & 1) {
            std::this_thread::yield();
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            const uint64_t w = words_[i].load(std::memory_order_relaxed);
            std::memcpy(token64 + i * 8, &w, 8);
        }
        gameTokenId = gameTokenId_.load(std::memory_order_relaxed);
        if (version_.load(std::memory_order_acquire) == v1) {
            version = v1;
            return true;
        }
    }
}

PlayerToken SharedCreds::snapshot() const {
    PlayerToken t;
    uint8_t token[hmac::TOKEN_OCTETS];
    uint32_t v = 0;
    if (!load(token, t.gameTokenId, v)) return t;
    t.token.assign(reinterpret_cast<const char*>(token), sizeof(token));
    t.expiresAtMs = expiresAtMs();
    return t;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

World::World(const FleetConfig& c, FleetStats& s, int workerCount)
    : cfg(c), stats(s), grid(Grid::forServers(c.servers, c.regionChunks)),
      workers(std::max(1, std::min(workerCount, c.servers))),
      creds(static_cast<size_t>(c.players)), shared(static_cast<size_t>(c.players)) {
    // A tag for this run, so an update left over from another run of the tool in
    // the same app is not read as this run's.
    runTag = static_cast<uint16_t>(
        mix64(c.seed ^ static_cast<uint64_t>(epochMs()) ^
              (static_cast<uint64_t>(getpid()) << 32)) &
        MARKER_RUN_TAG_MASK);
    sampleStride = c.latencySample > 0 ? std::max(1, c.players / c.latencySample) : 0;
    char uuid[32];
    for (int n = 0; n < c.players; ++n) {
        actorUuid(n, uuid);
        playerOfActor.emplace(std::string(uuid, sizeof(uuid)), n);
    }
}

void World::actorUuid(int player, char* out32) const {
    static const char* hex = "0123456789ABCDEF";
    const uint64_t a = mix64(static_cast<uint64_t>(cfg.appId) * 0x100000001B3ULL +
                             static_cast<uint64_t>(player));
    const uint64_t b = mix64(a ^ 0xF1EE7F1EE7ULL);
    for (int i = 0; i < 16; ++i) {
        out32[i] = hex[(a >> ((15 - i) * 4)) & 0xF];
        out32[i + 16] = hex[(b >> ((15 - i) * 4)) & 0xF];
    }
}

// ---------------------------------------------------------------------------
// FleetWorker: lifecycle and mail
// ---------------------------------------------------------------------------

FleetWorker::FleetWorker(int index, World& world, Connector connect,
                         std::function<void(const ApiRequest&)> requestApi)
    : index_(index), world_(world), connect_(std::move(connect)),
      requestApi_(std::move(requestApi)) {
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    updateIntervalNs_ = static_cast<int64_t>(1e9 / world_.cfg.updateHz);
    std::random_device entropy;
    nonceRng_.seed((static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^
                   mix64(static_cast<uint64_t>(nowNs()) + static_cast<uint64_t>(index)));
    localOf_.assign(static_cast<size_t>(world_.cfg.servers), -1);
    for (int s = 0; s < world_.cfg.servers; ++s) {
        if (world_.workerOf(s) != index_) continue;
        localOf_[static_cast<size_t>(s)] = static_cast<int>(sessions_.size());
        Session session;
        session.index = s;
        session.io = world_.stats.sessions[static_cast<size_t>(s)].get();
        sessions_.push_back(std::move(session));
    }
}

FleetWorker::~FleetWorker() {
    stop();
    for (auto& s : sessions_) closeSocket(s);
    if (epollFd_ >= 0) close(epollFd_);
}

void FleetWorker::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void FleetWorker::stop() {
    const bool was = running_.exchange(false);
    if (was && thread_.joinable()) thread_.join();
}

void FleetWorker::post(Mail&& m) {
    std::lock_guard<std::mutex> lock(mailMu_);
    mail_.push_back(std::move(m));
}

void FleetWorker::openSession(int session, const FleetSessionInfo& info) {
    Mail m;
    m.kind = Mail::Kind::OpenSession;
    m.session = session;
    m.info = info;
    post(std::move(m));
}

void FleetWorker::sessionMoved(int session, const FleetSessionInfo& info, bool ok) {
    Mail m;
    m.kind = Mail::Kind::SessionMoved;
    m.session = session;
    m.info = info;
    m.ok = ok;
    post(std::move(m));
}

void FleetWorker::admitPlayer(int player, const Walker& start) {
    auto p = std::make_unique<Player>();
    p->index = player;
    p->sampled = world_.sampled(player);
    p->walk = start;
    p->rng.seed(mix64(world_.cfg.seed * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(player)));
    world_.actorUuid(player, p->uuid);
    Mail m;
    m.kind = Mail::Kind::Arrive;
    m.session = world_.grid.regionAt(start.x, start.y);
    m.player = std::move(p);
    m.cause = CarryCause::Join;
    post(std::move(m));
}

void FleetWorker::releaseAll() {
    Mail m;
    m.kind = Mail::Kind::ReleaseAll;
    post(std::move(m));
}

FleetWorker::Session* FleetWorker::local(int session) {
    if (session < 0 || session >= static_cast<int>(localOf_.size())) return nullptr;
    const int i = localOf_[static_cast<size_t>(session)];
    return i < 0 ? nullptr : &sessions_[static_cast<size_t>(i)];
}

void FleetWorker::drainMail(int64_t now) {
    std::vector<Mail> incoming;
    {
        std::lock_guard<std::mutex> lock(mailMu_);
        if (mail_.empty()) return;
        incoming.swap(mail_);
    }
    for (auto& m : incoming) {
        if (m.kind == Mail::Kind::ReleaseAll) {
            // The run is over. From here the sessions only listen: a carry still
            // on its way must not land in the room a release has just made.
            leaving_ = true;
            for (auto& s : sessions_) {
                if (s.state != Session::State::Active) continue;
                for (auto& p : s.players) {
                    if (!p->carried || p->released || p->failed) continue;
                    uint8_t buf[FLEET_RELEASE_SIZE];
                    const size_t len = buildRelease(buf, world_.cfg.appId, s.info.gameTokenId,
                                                    p->curId, s.seq, s.token);
                    if (len && sendOn(s, buf, len, SendKind::Release, p->curId, now))
                        inc(world_.stats.releasesSent);
                    p->released = true;
                }
            }
            continue;
        }
        Session* s = local(m.session);
        if (!s) continue;
        switch (m.kind) {
            case Mail::Kind::OpenSession:
                activate(*s, m.info, now);
                break;
            case Mail::Kind::SessionMoved:
                if (!m.ok) {
                    inc(world_.stats.sessionMoveFailures);
                    if (s->state == Session::State::Moving) s->moveRetryAtNs = now + MOVE_RETRY_NS;
                    break;
                }
                inc(world_.stats.sessionMovesCompleted);
                s->lastMoveNs = now;
                if (m.info.server == s->info.server && !s->toldToMove) {
                    // The same server: the refresh authorized the same token again and
                    // the carries stand. Capabilities again (activate), and no carry.
                    inc(world_.stats.sessionReturnsSameServer);
                } else {
                    // Another server holds no carry: capabilities, then a new carry,
                    // with a new time and proof, for every player, at the carry rate.
                    // (A server that asked the session to leave keeps no carry for it
                    // either, whatever server the answer names.)
                    inc(world_.stats.sessionReturnsOtherServer);
                    forgetCarries(*s);
                    s->needCarryAll = true;
                }
                s->toldToMove = false;
                activate(*s, m.info, now);
                break;
            case Mail::Kind::Arrive:
                if (m.cause == CarryCause::Crossing) {
                    CrossingScope scope;
                    adopt(*s, std::move(m.player), m.cause, now);
                } else {
                    inc(world_.stats.playersAdmitted);
                    adopt(*s, std::move(m.player), m.cause, now);
                }
                break;
            case Mail::Kind::CrossingDone:
                onCrossingDone(*s, m.playerId, m.crossingSeq, m.resultNs);
                break;
            case Mail::Kind::ReleaseAll:
                break;
        }
    }
}

void FleetWorker::activate(Session& s, const FleetSessionInfo& info, int64_t now) {
    const std::string keptToken = s.info.token;
    s.info = info;
    if (s.info.token.empty()) s.info.token = keptToken;
    std::memcpy(s.token, s.info.token.data(),
                std::min(s.info.token.size(), sizeof(s.token)));
    closeSocket(s);
    std::string error;
    const int fd = connect_(s.info, error);
    if (fd < 0) {
        std::fprintf(stderr, "[fleet] session %d: cannot open a socket to %s:%u (%s)\n", s.index,
                     s.info.server.ip4.c_str(), s.info.server.port, error.c_str());
        // Ask the API where the session should be, after a pause.
        s.state = Session::State::Moving;
        s.moveRetryAtNs = now + MOVE_RETRY_NS;
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = static_cast<uint32_t>(localOf_[static_cast<size_t>(s.index)]);
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        s.state = Session::State::Moving;
        s.moveRetryAtNs = now + MOVE_RETRY_NS;
        return;
    }
    s.fd = fd;
    s.state = Session::State::Active;
    s.activateAtNs = now + static_cast<int64_t>(world_.cfg.sessionSettleMs) * MS;
    s.lastCapsNs = 0;       // a new server knows nothing about this session
    s.firstCapsNs = 0;
    s.lastHeartbeatNs = now;
    s.lastRxNs = now;
    s.errStreak = 0;
    s.sendErrStreak = 0;
    s.movePending = false;
    // A new control nonce for each assignment, so the move commands of one never
    // verify in the next.
    const uint64_t nonce = nonceRng_();
    std::memcpy(s.nonce, &nonce, sizeof(s.nonce));
    s.boundAcked = false;
    s.carryTokens = world_.cfg.carryBurst;
    s.carryRefillNs = now;
    s.lastCarriedTallyNs = now;
}

void FleetWorker::forgetCarries(Session& s) {
    for (auto& p : s.players) {
        if (p->carry.active) endCarryEpisode(s, *p);
        if (p->carried)
            world_.shared[static_cast<size_t>(p->index)].holder.store(-1,
                                                                      std::memory_order_relaxed);
        p->carried = false;
        p->retryAtNs = 0;
    }
    s.byId.clear();
}

bool FleetWorker::takeCarryToken(Session& s, int64_t now) {
    // The realtime servers drop a FLEET_CARRY past 64 a second a session,
    // unanswered. Every carry, a resend too, takes one token here.
    const double refill = static_cast<double>(now - s.carryRefillNs) * 1e-9 * world_.cfg.carryRate;
    s.carryRefillNs = now;
    s.carryTokens = std::min(static_cast<double>(world_.cfg.carryBurst), s.carryTokens + refill);
    if (s.carryTokens < 1.0) return false;
    s.carryTokens -= 1.0;
    return true;
}

FleetWorker::MinuteUse& FleetWorker::minuteOf(Session& s) {
    if (s.minuteKey != minuteNow_ || !s.minute) {
        MinuteUse& m = s.minutes[minuteNow_];
        m.session = s.index;
        m.gameTokenId = s.info.gameTokenId;
        m.minute = minuteNow_;
        s.minute = &m;
        s.minuteKey = minuteNow_;
    }
    return *s.minute;
}

void FleetWorker::closeSocket(Session& s) {
    if (s.fd >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, s.fd, nullptr);
        close(s.fd);
        s.fd = -1;
    }
}

void FleetWorker::requestApi(const ApiRequest& r) {
    ApiRequest req = r;
    if (crossingScopeFlag()) {
        // The code that handles a crossing must never reach the API. If a change
        // ever made it do so, this count fails the run.
        req.fromCrossingPath = true;
        inc(world_.stats.api.requestsFromCrossingPath);
    }
    if (requestApi_) requestApi_(req);
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

void FleetWorker::run() {
    constexpr int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];
    while (running_.load(std::memory_order_acquire)) {
        const int64_t now = nowNs();
        minuteNow_ = epochMs() / 60000;
        drainMail(now);
        for (auto& s : sessions_) {
            if (s.state == Session::State::Active && !leaving_) tickSession(s, now);
        }
        if (now - lastSweepNs_ >= 100 * MS) {
            lastSweepNs_ = now;
            sweep(now);
        }
        // One millisecond: a send is at most that late, and the mailbox is read as often.
        const int n = epoll_wait(epollFd_, events, MAX_EVENTS, 1);
        for (int i = 0; i < n; ++i) {
            drainSocket(sessions_[events[i].data.u32]);
        }
    }
}

void FleetWorker::tickSession(Session& s, int64_t now) {
    const FleetConfig& cfg = world_.cfg;
    if (now < s.activateAtNs) return;
    if (s.lastCapsNs == 0 || now - s.lastCapsNs >= static_cast<int64_t>(cfg.capsIntervalS) * SEC)
        sendCapabilities(s, now);
    if (cfg.heartbeatS > 0 &&
        now - s.lastHeartbeatNs >= static_cast<int64_t>(cfg.heartbeatS * 1e9))
        sendHeartbeat(s, now);
    if (s.needCarryAll) {
        s.needCarryAll = false;
        for (auto& p : s.players) {
            if (p->failed || p->waitToken || p->carry.active) continue;
            startCarry(s, *p, p->crossingOpen ? CarryCause::Crossing : CarryCause::SessionMove,
                       now);
        }
    }
    const bool moving = world_.moving.load(std::memory_order_relaxed);
    const bool confined = cfg.crossRate <= 0 || world_.grid.regions() == 1;
    const double speed = confined ? CONFINED_SPEED : world_.speed.load(std::memory_order_relaxed);

    for (size_t i = 0; i < s.players.size();) {
        Player& p = *s.players[i];
        if (p.failed || p.released) {
            ++i;
            continue;
        }
        tickCarry(s, p, now);
        if (now >= p.nextUpdateNs) {
            const int64_t scheduled = p.nextUpdateNs;
            p.nextUpdateNs += updateIntervalNs_;
            if (now - p.nextUpdateNs > updateIntervalNs_) {
                // A whole interval behind: do not send a burst to catch up.
                p.nextUpdateNs = now + updateIntervalNs_;
                if (p.carried) inc(world_.stats.updatesSkipped);
            }
            const double dt = static_cast<double>(now - p.lastMoveNs) * 1e-9;
            p.lastMoveNs = now;
            if (moving) {
                stepWalker(p.walk, world_.grid, speed, dt, p.rng, confined ? s.index : -1);
            }
            const int region = world_.grid.regionAt(p.walk.x, p.walk.y);
            if (region != s.index) {
                depart(s, i, region, now); // the player at slot i is now another one
                continue;
            }
            if (p.carried) sendUpdate(s, p, now, scheduled);
        }
        ++i;
    }
}

void FleetWorker::tickCarry(Session& s, Player& p, int64_t now) {
    const FleetConfig& cfg = world_.cfg;
    FleetStats& st = world_.stats;
    if (p.carry.active) {
        if (p.carry.firstSentNs == 0) {
            // Not sent yet: it waits for the session's carry rate.
            if (takeCarryToken(s, now)) {
                sendCarry(s, p, now);
            } else if (!p.carry.waited) {
                p.carry.waited = true;
                inc(st.carryPacedWaits);
            }
        } else if (now - p.carry.firstSentNs >= static_cast<int64_t>(cfg.carryGiveUpMs) * MS) {
            // No result in five seconds: give up, count it, and start again shortly.
            inc(st.carryTimedOut);
            p.retryCause = p.carry.cause;
            endCarryEpisode(s, p);
            p.retryAtNs = now + static_cast<int64_t>(cfg.carryRetryMs) * MS;
        } else if (now - p.carry.lastSentNs >=
                       (static_cast<int64_t>(cfg.carryResendMs) << std::min(p.carry.resends, 3)) * MS &&
                   takeCarryToken(s, now)) {
            // A 49 can go unanswered: send it again, with a new time and proof.
            // Each wait is twice the last (250 ms, 500 ms, 1 s, then 2 s), so a
            // server that is slow to answer is not sent more because of it.
            ++p.carry.resends;
            inc(st.carryResends);
            sendCarry(s, p, now);
        }
        return;
    }
    const uint32_t v = world_.creds[static_cast<size_t>(p.index)].version();
    if (p.waitToken) {
        if (v != p.waitVersion && !(v & 1)) {
            p.waitToken = false;
            startCarry(s, p, CarryCause::SignInAgain, now);
        } else if (now - p.waitSinceNs >= TOKEN_WAIT_RETRY_NS) {
            requestSignInAgain(p, now);
        }
        return;
    }
    if (p.retryAtNs) {
        if (now >= p.retryAtNs) {
            p.retryAtNs = 0;
            inc(st.carryRetries);
            startCarry(s, p, p.retryCause, now);
        }
        return;
    }
    if (p.carried) {
        // The player's app token was refreshed: carry again with the new one. The
        // carry made with the old one is replaced without a gap.
        if (v != p.curVersion && !(v & 1)) startCarry(s, p, CarryCause::TokenRefresh, now);
        return;
    }
    startCarry(s, p, p.retryCause, now);
}

void FleetWorker::sweep(int64_t now) {
    const FleetConfig& cfg = world_.cfg;
    FleetStats& st = world_.stats;
    int players = 0, carried = 0, pending = 0, awaiting = 0, active = 0;
    for (auto& s : sessions_) {
        int sCarried = 0;
        for (auto& p : s.players) {
            ++players;
            if (p->failed) continue;
            if (p->carried) {
                ++sCarried;
            } else {
                ++pending;
            }
        }
        carried += sCarried;
        s.io->players.store(static_cast<int>(s.players.size()), std::memory_order_relaxed);
        s.io->carried.store(sCarried, std::memory_order_relaxed);
        if (s.state == Session::State::Active && s.lastCarriedTallyNs) {
            // The seconds this session's players were carried, as the tier's usage
            // row counts them for the bill's first band.
            minuteOf(s).carriedPlayerSeconds +=
                sCarried * static_cast<double>(now - s.lastCarriedTallyNs) * 1e-9;
        }
        s.lastCarriedTallyNs = now;

        for (auto it = s.departed.begin(); it != s.departed.end();) {
            Departed& d = it->second;
            if (d.late) {
                if (now - d.departNs > LATE_ENTRY_NS) {
                    if (d.altId) s.departedAlias.erase(d.altId);
                    it = s.departed.erase(it);
                    continue;
                }
            } else if (d.resultNs && !d.releasedNs) {
                if (now - d.resultNs > static_cast<int64_t>(cfg.releaseWaitMs) * MS) {
                    // The next session's carry landed and no FLEET_RELEASED reason 1
                    // came here within the wait. Nothing else is done about it: the
                    // player is carried by the next session, and this one stopped
                    // sending for it when it left. It is counted and described.
                    inc(st.releaseMissing);
                    st.events.add("released_never_arrived",
                                  {{"t_s", world_.phaseSeconds(now)},
                                   {"session", s.index},
                                   {"player", d.player},
                                   {"waited_ms", msBetween(now, d.resultNs)},
                                   {"left_a_token_carry_unanswered", d.altId != 0}});
                    if (d.altId) s.departedAlias.erase(d.altId);
                    it = s.departed.erase(it);
                    continue;
                }
                ++awaiting;
            } else if (now - d.departNs > UNPAIRED_ENTRY_NS) {
                inc(st.releaseUnpaired);
                if (d.altId) s.departedAlias.erase(d.altId);
                it = s.departed.erase(it);
                continue;
            } else if (now - d.departNs < static_cast<int64_t>(cfg.carryGiveUpMs) * MS) {
                ++awaiting; // the new session's result is still on its way
            }
            ++it;
        }

        if (s.state == Session::State::Active) {
            ++active;
            if (s.movePending && now - s.lastMoveNs >= MOVE_COOLDOWN_NS)
                requestMove(s, s.pendingTrigger, now, s.pendingToldToMove);
            if (cfg.rxSilentS > 0 && now >= s.activateAtNs && !s.players.empty() &&
                now - s.lastRxNs > static_cast<int64_t>(cfg.rxSilentS) * SEC) {
                requestMove(s, MoveTrigger::RxSilence, now);
            }
        } else if (s.state == Session::State::Moving && s.moveRetryAtNs &&
                   now >= s.moveRetryAtNs) {
            s.moveRetryAtNs = 0;
            ApiRequest r;
            r.kind = ApiRequest::Kind::MoveSession;
            r.index = s.index;
            if (!s.toldToMove) r.current = s.info.server;
            r.gameTokenId = s.info.gameTokenId;
            r.trigger = s.moveTrigger; // the same move, asked again
            requestApi(r);
        }
    }
    gPlayers_.store(players, std::memory_order_relaxed);
    gCarried_.store(carried, std::memory_order_relaxed);
    gPending_.store(pending, std::memory_order_relaxed);
    gAwaiting_.store(awaiting, std::memory_order_relaxed);
    gActive_.store(active, std::memory_order_relaxed);
}

void FleetWorker::drainSocket(Session& s) {
    if (s.fd < 0) return;
    uint8_t buf[2048];
    for (int i = 0; i < MAX_DATAGRAMS_PER_WAKE; ++i) {
        const ssize_t n = recv(s.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == ECONNREFUSED || errno == EINTR) continue; // a lost datagram; keep reading
            break;                                                 // EAGAIN: drained
        }
        handleDatagram(s, buf, static_cast<size_t>(n), nowNs());
        if (s.fd < 0) break; // the handler moved the session
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool FleetWorker::sendOn(Session& s, const uint8_t* buf, size_t len, SendKind kind, int64_t id,
                         int64_t now, const Player* forPlayer) {
    if (s.fd < 0) return false;
    // The sequence number was written into `buf` by the builder: remember what it
    // was for, so an error that echoes it can be laid at the right message.
    SeqEntry& e = s.ring[s.seq];
    e = SeqEntry{};
    e.id = id;
    e.sentNs = now;
    e.kind = kind;
    e.type = len ? buf[0] : 0;
    if (forPlayer) {
        e.player = forPlayer->index;
        e.tokenCarryPending = forPlayer->carry.active && forPlayer->carry.id != forPlayer->curId;
        e.refreshInFlight =
            world_.shared[static_cast<size_t>(forPlayer->index)].refreshStartedAtNs.load(
                std::memory_order_relaxed) != 0;
    }
    ++s.seq;
    const ssize_t n = send(s.fd, buf, len, 0);
    if (n == static_cast<ssize_t>(len)) {
        inc(s.io->txBytes, len);
        inc(s.io->txDatagrams);
        inc(world_.stats.txBytes, len);
        inc(world_.stats.txDatagrams);
        MinuteUse& minute = minuteOf(s);
        minute.txBytes += len;
        ++minute.txDatagrams;
        s.sendErrStreak = 0;
        return true;
    }
    inc(world_.stats.txSendErrors);
    // A realtime server that went away answers with ICMP, which shows as
    // ECONNREFUSED on the next send. A full socket buffer is this host's own.
    if (errno == ECONNREFUSED && ++s.sendErrStreak >= SEND_ERROR_STREAK_MOVE) {
        // Leave the move to the next sweep: this send may be a crossing's carry,
        // and the socket failing is not something the crossing caused.
        s.sendErrStreak = 0;
        s.movePending = true;
        s.pendingTrigger = MoveTrigger::SendErrors;
        s.pendingToldToMove = false;
    }
    return false;
}

void FleetWorker::sendCapabilities(Session& s, int64_t now) {
    s.lastCapsNs = now;
    if (s.firstCapsNs == 0) s.firstCapsNs = now;
    uint32_t flags = 0;
    if (world_.cfg.signedBundles) flags |= wire::CAP_BUNDLE_SIGNED;
    if (world_.cfg.ownState) flags |= CAP_FLEET_OWN_STATE;
    uint8_t buf[CAPABILITIES_BOUND_SIZE];
    const size_t len =
        buildCapabilities(buf, world_.cfg.appId, s.info.gameTokenId, flags,
                          world_.cfg.boundControl ? s.nonce : nullptr, s.seq, s.token);
    if (len && sendOn(s, buf, len, SendKind::Capabilities, s.info.gameTokenId, now))
        inc(world_.stats.capabilitiesSent);
}

void FleetWorker::sendHeartbeat(Session& s, int64_t now) {
    s.lastHeartbeatNs = now;
    uint8_t buf[HEARTBEAT_SIZE];
    const size_t len = buildHeartbeat(buf, world_.cfg.appId, s.info.gameTokenId, s.seq, s.token);
    if (len && sendOn(s, buf, len, SendKind::Heartbeat, s.info.gameTokenId, now))
        inc(world_.stats.heartbeatsSent);
}

void FleetWorker::sendCarry(Session& s, Player& p, int64_t now) {
    // A carry can wait its turn (the session's carry rate). When it goes out it
    // names the player's newest token: one a refresh has replaced meanwhile would
    // be answered status 1. A carry that was already sent keeps its token when it
    // is sent again, so its result is still the answer to it.
    const uint32_t v = world_.creds[static_cast<size_t>(p.index)].version();
    if (p.carry.firstSentNs == 0 && v != p.carry.version && !(v & 1)) {
        uint8_t token[hmac::TOKEN_OCTETS];
        int64_t id = 0;
        uint32_t version = 0;
        if (world_.creds[static_cast<size_t>(p.index)].load(token, id, version) &&
            id != p.carry.id) {
            if (p.carry.id != p.curId) s.byId.erase(p.carry.id);
            std::memcpy(p.carry.token, token, sizeof(token));
            p.carry.id = id;
            p.carry.version = version;
            s.byId[id] = &p;
            inc(world_.stats.carryTookNewerToken);
        }
    }
    if (p.carry.firstSentNs == 0) {
        p.carry.firstSentNs = now;
        if (p.carry.cause == CarryCause::TokenRefresh) {
            // How long after the API's answer with the new token its first carry
            // goes out.
            const int64_t stored = world_.shared[static_cast<size_t>(p.index)]
                                       .tokenStoredAtNs.load(std::memory_order_relaxed);
            if (stored && now >= stored) world_.stats.tokenCarryDelay.record((now - stored) / 1000);
        }
    }
    p.carry.lastSentNs = now;
    // Each FLEET_CARRY names a later time than the one before: a carry older than
    // the session's last accepted carry of the player is refused as a replay.
    p.lastCarryMs = std::max(epochMs(), p.lastCarryMs + 1);
    uint8_t buf[FLEET_CARRY_SIZE];
    const size_t len = buildCarry(buf, world_.cfg.appId, s.info.gameTokenId, p.carry.id,
                                  p.lastCarryMs, p.carry.token, s.seq, s.token);
    if (len && sendOn(s, buf, len, SendKind::Carry, p.carry.id, now))
        inc(world_.stats.carryDatagrams);
}

void FleetWorker::sendUpdate(Session& s, Player& p, int64_t now, int64_t scheduledNs) {
    const FleetConfig& cfg = world_.cfg;
    const Grid& g = world_.grid;
    const double speedUnits = world_.speed.load(std::memory_order_relaxed) * cfg.chunkSizeUnits;
    wire::ActorPose pose;
    pose.posX = (p.walk.x - std::floor(p.walk.x)) * cfg.chunkSizeUnits;
    pose.posY = (p.walk.y - std::floor(p.walk.y)) * cfg.chunkSizeUnits;
    pose.posZ = UE5_HEIGHT_OFFSET;
    pose.velX = p.walk.dx * speedUnits;
    pose.velY = p.walk.dy * speedUnits;
    // The marker (FleetWire.hpp): who sent this update, and when.
    PlayerShared& shared = world_.shared[static_cast<size_t>(p.index)];
    shared.chunkX.store(static_cast<int32_t>(g.chunkX(p.walk.x)), std::memory_order_relaxed);
    shared.chunkY.store(static_cast<int32_t>(g.chunkY(p.walk.y)), std::memory_order_relaxed);
    const bool first = p.firstUpdateDue;
    p.firstUpdateDue = false;
    if (first) inc(world_.stats.firstUpdatesSent);
    pose.pitch =
        markerCode(static_cast<uint16_t>(s.index), world_.runTag, p.sampled, p.index, first);
    pose.yaw = static_cast<double>(now / 1000);
    pose.roll = std::atan2(p.walk.dy, p.walk.dx) * (180.0 / 3.14159265358979);
    // The tail names the PLAYER (set in the template); the HMAC is keyed with the
    // FLEET token.
    if (!wire::finalizeActorUpdateFmt(p.msg, wire::PoseFormat::UE5, g.chunkX(p.walk.x),
                                      g.chunkY(p.walk.y), 0, pose, s.seq, s.token)) {
        inc(world_.stats.txSendErrors);
        return;
    }
    if (sendOn(s, p.msg, wire::actorUpdateSize(wire::PoseFormat::UE5), SendKind::Update, p.curId,
               now, &p)) {
        inc(world_.stats.updatesSent);
        world_.stats.sendLag.record((now - scheduledNs) / 1000);
    }
}

// ---------------------------------------------------------------------------
// Carrying and crossing
// ---------------------------------------------------------------------------

void FleetWorker::startCarry(Session& s, Player& p, CarryCause cause, int64_t now) {
    int64_t id = 0;
    uint32_t version = 0;
    if (!world_.creds[static_cast<size_t>(p.index)].load(p.carry.token, id, version)) {
        p.retryCause = cause;
        p.retryAtNs = now + 250 * MS; // no token yet
        return;
    }
    p.carry.active = true;
    p.carry.cause = cause;
    p.carry.id = id;
    p.carry.version = version;
    p.carry.firstSentNs = 0;
    p.carry.lastSentNs = 0;
    p.carry.waited = false;
    p.carry.resends = 0;
    p.retryCause = cause;
    p.retryAtNs = 0;
    s.byId[id] = &p;
    inc(world_.stats.carryAttempts[static_cast<size_t>(cause)]);
    // Sent now if the session's carry rate allows, otherwise at its turn (tickCarry).
    if (takeCarryToken(s, now)) {
        sendCarry(s, p, now);
    } else {
        p.carry.waited = true;
        inc(world_.stats.carryPacedWaits);
    }
}

void FleetWorker::endCarryEpisode(Session& s, Player& p) {
    if (p.carry.active && p.carry.id != p.curId) s.byId.erase(p.carry.id);
    p.carry.active = false;
}

void FleetWorker::adopt(Session& s, std::unique_ptr<Player> owned, CarryCause cause,
                        int64_t now) {
    Player& p = *owned;
    s.players.push_back(std::move(owned));
    s.byIndex[p.index] = &p;
    if (p.lastMoveNs == 0) p.lastMoveNs = now;
    if (p.nextUpdateNs == 0) {
        // Spread first sends over one interval so a batch of joins is not a burst.
        std::uniform_int_distribution<int64_t> off(0, updateIntervalNs_);
        p.nextUpdateNs = now + off(p.rng);
    }
    p.retryCause = cause;
    p.retryAtNs = 0;
    // The session's capabilities are its first datagram, so a carry waits for them.
    if (s.state == Session::State::Active && now >= s.activateAtNs && s.lastCapsNs != 0)
        startCarry(s, p, cause, now);
    // Otherwise the carry starts when the session is up (tickCarry, or needCarryAll).
}

void FleetWorker::depart(Session& s, size_t slot, int toRegion, int64_t now) {
    // Everything a crossing does on the side the player leaves. No API call can
    // be made from here: see requestApi.
    CrossingScope scope;
    FleetStats& st = world_.stats;
    std::unique_ptr<Player> p = std::move(s.players[slot]);
    if (slot + 1 != s.players.size()) s.players[slot] = std::move(s.players.back());
    s.players.pop_back();
    s.byIndex.erase(p->index);
    PlayerShared& shared = world_.shared[static_cast<size_t>(p->index)];
    if (p->carried) {
        shared.leftSession.store(s.index, std::memory_order_relaxed);
        shared.leftAtNs.store(now, std::memory_order_relaxed);
        shared.holder.store(-1, std::memory_order_relaxed);
    }
    // A carry the player leaves behind unanswered (one made for a refreshed token):
    // if it lands, the server's release to this session names ITS token id.
    const int64_t leftBehind = p->carry.active && p->carry.id != p->curId ? p->carry.id : 0;

    inc(st.crossingsAttempted);
    if (p->crossingOpen) inc(st.crossingsSuperseded); // crossed again before it was carried
    if (p->carry.active) {
        inc(st.carryAbandoned);
        endCarryEpisode(s, *p);
    }
    if (p->curId) {
        auto it = s.byId.find(p->curId);
        if (it != s.byId.end() && it->second == p.get()) s.byId.erase(it);
    }
    ++p->crossingSeq;
    if (p->carried && world_.cfg.releaseFirst) {
        // The release-first plan: this session lets go of the player itself, so
        // no FLEET_RELEASED is due. One that comes anyway (the release was lost,
        // or overtaken by the new carry) is counted apart.
        uint8_t buf[FLEET_RELEASE_SIZE];
        const size_t len =
            buildRelease(buf, world_.cfg.appId, s.info.gameTokenId, p->curId, s.seq, s.token);
        if (len && sendOn(s, buf, len, SendKind::Release, p->curId, now)) {
            inc(st.releasesSent);
            inc(st.crossingReleasesFirst);
        }
        Departed d;
        d.player = p->index;
        d.departNs = now;
        d.late = true;
        d.releasedFirst = true;
        s.departed[p->curId] = d;
        p->originSession = -1;
    } else if (p->carried) {
        // The default plan: the destination session carries with no word to this
        // one, and FLEET_RELEASED reason 1 is due here once that carry lands.
        auto old = s.departed.find(p->curId);
        if (old != s.departed.end() && !old->second.late) inc(st.releaseUnpaired);
        if (old != s.departed.end()) eraseDeparted(s, old);
        Departed d;
        d.player = p->index;
        d.crossingSeq = p->crossingSeq;
        d.departNs = now;
        d.altId = leftBehind;
        s.departed[p->curId] = d;
        if (leftBehind) s.departedAlias[leftBehind] = p->curId;
        inc(st.releaseExpected);
        p->originSession = s.index;
        p->originId = p->curId;
        p->originSeq = p->crossingSeq;
    }
    // The old session stops sending for the player at once.
    p->carried = false;
    p->curId = 0;
    p->curVersion = 0;
    p->retryAtNs = 0;
    p->waitToken = false;
    p->unauthStreak = 0;
    p->crossingOpen = true;
    p->crossStartNs = now;
    deliver(std::move(p), toRegion, CarryCause::Crossing, now);
}

void FleetWorker::deliver(std::unique_ptr<Player> p, int toSession, CarryCause cause,
                          int64_t now) {
    const int w = world_.workerOf(toSession);
    if (w == index_) {
        adopt(*local(toSession), std::move(p), cause, now);
        return;
    }
    Mail m;
    m.kind = Mail::Kind::Arrive;
    m.session = toSession;
    m.player = std::move(p);
    m.cause = cause;
    (*peers_)[static_cast<size_t>(w)]->post(std::move(m));
}

void FleetWorker::requestSignInAgain(Player& p, int64_t now) {
    SharedCreds& creds = world_.creds[static_cast<size_t>(p.index)];
    p.waitToken = true;
    p.waitVersion = creds.version();
    p.waitSinceNs = now;
    inc(world_.stats.resignRequests);
    if (!creds.busy.exchange(true)) {
        ApiRequest r;
        r.kind = ApiRequest::Kind::SignInAgain;
        r.index = p.index;
        requestApi(r);
    }
}

void FleetWorker::requestMove(Session& s, MoveTrigger trigger, int64_t now, bool toldToMove) {
    if (s.state != Session::State::Active) return;
    if (s.lastMoveNs && now - s.lastMoveNs < MOVE_COOLDOWN_NS) {
        // The last move is still settling. Do it when that is over (sweep), so a
        // command that arrives now is not lost.
        s.movePending = true;
        s.pendingTrigger = trigger;
        s.pendingToldToMove = toldToMove;
        return;
    }
    s.movePending = false;
    inc(world_.stats.sessionMoves[static_cast<size_t>(trigger)]);
    // The session goes quiet until the API has answered. Its carries are kept: if
    // the answer names the same server they stand, and only another server needs
    // every player carried again.
    closeSocket(s);
    s.state = Session::State::Moving;
    s.moveRetryAtNs = 0;
    s.toldToMove = toldToMove;
    s.moveTrigger = trigger;
    ApiRequest r;
    r.kind = ApiRequest::Kind::MoveSession;
    r.index = s.index;
    // Told to move: leave the current server out, so the API names a new one.
    // Otherwise name it, and the session stays if the API still has it serve the app.
    if (!toldToMove) r.current = s.info.server;
    r.gameTokenId = s.info.gameTokenId;
    r.trigger = trigger;
    requestApi(r);
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void FleetWorker::handleDatagram(Session& s, const uint8_t* data, size_t len, int64_t now) {
    FleetStats& st = world_.stats;
    inc(s.io->rxBytes, len);
    inc(s.io->rxDatagrams);
    inc(st.rxBytes, len);
    inc(st.rxDatagrams);
    MinuteUse& minute = minuteOf(s);
    minute.rxBytes += len;
    ++minute.rxDatagrams;
    s.lastRxNs = now;
    if (len == 0) return;
    const uint8_t first = data[0];

    // The commands that tell a session to move are always their own datagram,
    // signed with the fleet token. A session that advertised BOUND_CONTROL is
    // sent 44 and 45, bound to its nonce, in place of 22 and 28.
    if (first == wire::COMMAND_RECONNECT || first == COMMAND_RECONNECT_BOUND) {
        bool ok = false;
        if (first == COMMAND_RECONNECT_BOUND) {
            ok = world_.cfg.boundControl && verifyReconnectBound(data, len, s.token, s.nonce);
        } else if (s.boundAcked) {
            // This server acknowledged the nonce, so it sends only the bound forms:
            // a 22 now was captured earlier and sent again.
            inc(st.legacyControlDropped);
            return;
        } else {
            ok = verifyReconnect(data, len, s.token);
        }
        countControl(first, ok);
        if (!ok) {
            inc(st.rxHmacFailures);
            return;
        }
        inc(st.reconnectCommands);
        requestMove(s, MoveTrigger::ReconnectCommand, now, /*toldToMove*/ true);
        return;
    }
    if (first == COMMAND_SESSION_RELEASED || first == COMMAND_SESSION_RELEASED_BOUND) {
        std::optional<uint8_t> reason;
        if (first == COMMAND_SESSION_RELEASED_BOUND) {
            if (world_.cfg.boundControl)
                reason = verifySessionReleasedBound(data, len, s.token, s.nonce);
        } else if (s.boundAcked) {
            inc(st.legacyControlDropped);
            return;
        } else {
            reason = verifySessionReleased(data, len, s.token);
        }
        countControl(first, reason.has_value());
        if (!reason) {
            inc(st.rxHmacFailures);
            return;
        }
        inc(st.sessionReleasedByReason[*reason]);
        // Reasons 3 (overload) and 4 (rebalance): the server wants the session on
        // another one. Any other reason: ask the API to authorize it again.
        requestMove(s, MoveTrigger::SessionReleased, now, *reason == 3 || *reason == 4);
        return;
    }
    if (first == CONTROL_BOUND_ACK) {
        const bool ok = world_.cfg.boundControl && verifyBoundAck(data, len, s.token, s.nonce);
        countControl(first, ok);
        if (ok) {
            s.boundAcked = true;
            inc(st.boundAcks);
        } else {
            inc(st.rxHmacFailures);
        }
        return;
    }

    const bool signedBundle = first == wire::MESSAGE_BUNDLE_SIGNED;
    if (signedBundle) {
        // One HMAC over the datagram, keyed with the fleet token; the members
        // carry none. forEachMessage strips the tail.
        inc(st.rxSignedBundles);
        if (!wire::verifySignedBundle(data, len, s.token)) {
            inc(st.rxHmacFailures);
            return;
        }
    }
    const bool plainBundle = first == wire::MESSAGE_BUNDLE;
    const Where where =
        signedBundle ? Where::SignedBundle : plainBundle ? Where::PlainBundle : Where::Standalone;
    nlohmann::json members = nlohmann::json::array();
    if (plainBundle) {
        // A plain bundle is not signed as a whole: each member must carry its own
        // HMAC. What its members were, and whether each one's HMAC verified, goes
        // into the summary.
        inc(st.rxPlainBundles);
        bundleMembers_ = &members;
    }
    const bool ok = wire::forEachMessage(data, len, [&](const wire::InboundView& m) {
        handleMessage(s, m, where, now);
    });
    bundleMembers_ = nullptr;
    if (plainBundle) {
        st.events.add("plain_bundles", {{"t_s", world_.phaseSeconds(now)},
                                        {"session", s.index},
                                        {"datagram_bytes", len},
                                        {"well_formed", ok},
                                        {"members", std::move(members)}});
    }
    if (!ok) inc(st.rxMalformed);
}

void FleetWorker::describeNeighbourhood(const Session& s, int64_t cx, int64_t cy, int64_t now,
                                        nlohmann::json& e, bool* nearFormerPlayer) {
    // Which player could put this session in range of the chunk (cx, cy)? The
    // nearest one it carries now (which stands inside its region), and the
    // nearest one it carried before and no longer carries, by the chunk of each
    // one's last update. Read from other threads' atomics, so a position can be
    // one update old.
    int nearestNow = -1, nearestFormer = -1;
    int64_t nowDist = INT64_MAX, formerDist = INT64_MAX;
    for (int n = 0; n < world_.cfg.players; ++n) {
        const PlayerShared& shared = world_.shared[static_cast<size_t>(n)];
        const int holder = shared.holder.load(std::memory_order_relaxed);
        const bool mine = holder == s.index;
        if (!mine && !shared.wasCarriedBy(s.index)) continue;
        const int64_t dx = std::llabs(cx - shared.chunkX.load(std::memory_order_relaxed));
        const int64_t dy = std::llabs(cy - shared.chunkY.load(std::memory_order_relaxed));
        const int64_t d = std::max(dx, dy);
        if (mine) {
            if (d < nowDist) nowDist = d, nearestNow = n;
        } else if (d < formerDist) {
            formerDist = d, nearestFormer = n;
        }
    }
    if (nearestNow >= 0) e["nearest_player_the_session_carries_is_chunks_away"] = nowDist;
    const bool near = nearestFormer >= 0 && formerDist <= world_.cfg.distance + 1;
    e["in_range_of_a_player_the_session_carried_before"] = near;
    if (nearestFormer >= 0) {
        const PlayerShared& shared = world_.shared[static_cast<size_t>(nearestFormer)];
        e["nearest_player_the_session_carried_before"] = nearestFormer;
        e["that_player_is_chunks_away"] = formerDist;
        e["that_player_is_carried_by_session"] = shared.holder.load(std::memory_order_relaxed);
        if (shared.leftSession.load(std::memory_order_relaxed) == s.index) {
            e["ms_since_that_player_left_this_session"] =
                msBetween(now, shared.leftAtNs.load(std::memory_order_relaxed));
        }
    }
    if (nearFormerPlayer) *nearFormerPlayer = near;
}

void FleetWorker::countControl(uint8_t type, bool verified) {
    // 22, 28, 44, 45 and 46 are always alone in their datagram, with an HMAC
    // keyed on the fleet token.
    FleetStats& st = world_.stats;
    inc(st.rxStandalone[type]);
    inc(verified ? st.rxOutsideOwnHmacOk[type] : st.rxOutsideOwnHmacFailed[type]);
}

void FleetWorker::noteOutsideSignedBundle(Session& s, const wire::InboundView& m, Where where,
                                          const char* ownHmac, int64_t now) {
    // A message that arrived outside a signed bundle: how it was authenticated.
    FleetStats& st = world_.stats;
    const uint8_t type = m.type();
    const bool none = ownHmac[0] == 'n';
    const bool verified = ownHmac[0] == 'v';
    inc(none ? st.rxOutsideNoHmac[type]
             : verified ? st.rxOutsideOwnHmacOk[type] : st.rxOutsideOwnHmacFailed[type]);
    if (bundleMembers_) {
        bundleMembers_->push_back({{"type", type},
                                   {"name", messageTypeName(type)},
                                   {"bytes", m.len},
                                   {"own_hmac", ownHmac}});
    }
    if (!(type & wire::SPATIAL_TYPE_BIT) || verified) return;
    // A notification with no HMAC of its own, or one that did not verify, outside
    // a signed bundle: nothing says the realtime server sent it.
    nlohmann::json e = {{"t_s", world_.phaseSeconds(now)},
                        {"session", s.index},
                        {"where", whereName(static_cast<int>(where))},
                        {"type", type},
                        {"name", messageTypeName(type)},
                        {"bytes", m.len},
                        {"own_hmac", ownHmac},
                        {"contains_auth_byte",
                         m.len > wire::off::CONTAINS_AUTH ? m.data[wire::off::CONTAINS_AUTH] : 0},
                        {"ms_since_the_session_first_sent_capabilities",
                         s.firstCapsNs ? msBetween(now, s.firstCapsNs) : -1.0},
                        {"ms_since_the_session_last_sent_capabilities",
                         s.lastCapsNs ? msBetween(now, s.lastCapsNs) : -1.0}};
    if (s.lastCarriedNs)
        e["ms_since_the_session_last_carry_result"] = msBetween(now, s.lastCarriedNs);
    if (s.lastReleasedNs)
        e["ms_since_the_session_last_fleet_released"] = msBetween(now, s.lastReleasedNs);
    if (m.len >= wire::HEADER_SIZE) {
        // How far the notification's chunk is from the receiving session's region.
        // Every player the session carries stands inside its region, so one from
        // farther than the replication distance is out of range of all of them.
        const int64_t far = world_.grid.chunksFromRegion(
            s.index, wire::readI64(m.data + wire::off::CHUNK_X),
            wire::readI64(m.data + wire::off::CHUNK_Y));
        e["chunks_from_the_receiving_sessions_region"] = far;
        e["out_of_range_of_every_player_of_the_session"] = far > world_.cfg.distance + 1;
        if (none && far > world_.cfg.distance + 1) inc(st.unsignedOutOfRange);
        if (far > world_.cfg.distance + 1) {
            describeNeighbourhood(s, wire::readI64(m.data + wire::off::CHUNK_X),
                                  wire::readI64(m.data + wire::off::CHUNK_Y), now, e);
        }
        const auto actor = world_.playerOfActor.find(
            std::string(reinterpret_cast<const char*>(m.data + wire::off::UUID), 32));
        if (actor != world_.playerOfActor.end()) {
            const PlayerShared& shared = world_.shared[static_cast<size_t>(actor->second)];
            e["actor_player"] = actor->second;
            e["actor_player_carried_by_session"] = shared.holder.load(std::memory_order_relaxed);
            e["the_session_carried_that_player_before"] = shared.wasCarriedBy(s.index);
            if (shared.leftSession.load(std::memory_order_relaxed) == s.index) {
                e["ms_since_that_player_left_the_session"] =
                    msBetween(now, shared.leftAtNs.load(std::memory_order_relaxed));
            }
        }
    }
    if (type == wire::ACTOR_UPDATE_NOTIFICATION_2) {
        const auto payload = notificationPayload(m);
        const auto marker = payload ? readMarker(payload->data, payload->len) : std::nullopt;
        if (marker && marker->runTag == world_.runTag && marker->player < world_.cfg.players) {
            e["sent_by_session"] = marker->session;
            e["sent_by_the_receiving_session"] = marker->session == s.index;
            e["first_update_after_a_carry_result"] = marker->first;
            e["update_age_ms"] = msBetween(now, marker->sendUs * 1000);
            if (none) {
                inc(marker->session == s.index ? st.unsignedSentByReceiver
                                               : st.unsignedSentByAnother);
                if (marker->first) inc(st.unsignedFirstUpdate);
            }
        } else if (none) {
            inc(st.unsignedNotThisRun);
        }
    } else {
        if (none) inc(st.unsignedOtherType);
        if (type == ACTOR_LEFT_NOTIFICATION) {
            const auto payload = notificationPayload(m);
            if (payload && payload->len >= 1) e["actor_left_reason"] = payload->data[0];
        }
    }
    st.events.add("notifications_outside_a_signed_bundle_without_a_verified_hmac", std::move(e));
}

void FleetWorker::handleMessage(Session& s, const wire::InboundView& m, Where where,
                                int64_t now) {
    FleetStats& st = world_.stats;
    const uint8_t type = m.type();
    const bool trusted = where == Where::SignedBundle;
    if (s.state != Session::State::Active) return; // a handler in this datagram moved the session
    inc(where == Where::SignedBundle ? st.rxInSignedBundle[type]
                                     : where == Where::PlainBundle ? st.rxInPlainBundle[type]
                                                                   : st.rxStandalone[type]);
    if (where == Where::PlainBundle) inc(st.plainBundleMembers);

    if (type == wire::GENERIC_ERROR_MESSAGE) {
        // [3][sequence number][code]: three bytes, with no HMAC in any form.
        if (!trusted) noteOutsideSignedBundle(s, m, where, "none", now);
        inc(st.rxByType[type]);
        onError(s, m, now);
        return;
    }

    if (type == FLEET_CARRY_RESULT || type == FLEET_RELEASED || type == FLEET_DELIVERY) {
        // In a verified signed bundle the frame has no HMAC of its own. Anywhere
        // else its last 32 bytes are its HMAC under the fleet token, and a frame
        // whose HMAC is missing or wrong is dropped and counted.
        const FleetFrame f = readFleetFrame(m, trusted, s.token);
        if (!trusted)
            noteOutsideSignedBundle(s, m, where, f.auth == FrameAuth::Failed ? "failed" : "verified",
                                    now);
        if (f.auth == FrameAuth::Failed) {
            inc(st.fleetFrameHmacFailures);
            return;
        }
        inc(f.auth == FrameAuth::SignedBundle ? st.fleetFramesInSignedBundle
                                              : st.fleetFramesWithOwnHmac);
        if (type == FLEET_CARRY_RESULT) {
            const auto r = parseCarryResult(f.frame);
            if (!r) {
                inc(st.rxMalformed);
                return;
            }
            inc(st.rxByType[type]);
            onCarryResult(s, *r, now);
        } else if (type == FLEET_RELEASED) {
            const auto r = parseReleased(f.frame);
            if (!r) {
                inc(st.rxMalformed);
                return;
            }
            inc(st.rxByType[type]);
            onReleased(s, *r, now);
        } else {
            const auto d = parseDelivery(f.frame);
            if (!d) {
                inc(st.rxMalformed);
                return;
            }
            // A FLEET_DELIVERY means "only these players": a communication, or a
            // filtered event or app-defined message some of the session's players
            // may not receive.
            inc(st.rxByType[type]);
            inc(st.deliveries);
            inc(st.deliveryPlayers, d->count);
            inc(s.io->deliveries);
            inc(s.io->deliveryPlayers, d->count);
            inc(st.deliveryByType[d->notification.type()]);
            for (size_t i = 0; i < d->count; ++i) {
                // It must list only players this session carries.
                auto it = s.byId.find(d->player(i));
                if (it == s.byId.end() || !it->second->carried) inc(st.deliveryForUncarried);
            }
        }
        return;
    }

    if (type & wire::SPATIAL_TYPE_BIT) {
        if (!trusted) {
            // Outside a signed bundle a notification carries its own HMAC. One
            // whose header says it has none is counted (unsigned_notifications)
            // and still read, so the summary can say what it was; one whose HMAC
            // does not verify is dropped.
            if (m.len > wire::off::CONTAINS_AUTH && m.data[wire::off::CONTAINS_AUTH] == 0) {
                inc(st.rxUnsignedSpatial);
                noteOutsideSignedBundle(s, m, where, "none", now);
            } else if (!wire::verifyNotification(m, s.token)) {
                inc(st.rxHmacFailures);
                noteOutsideSignedBundle(s, m, where, "failed", now);
                return;
            } else {
                noteOutsideSignedBundle(s, m, where, "verified", now);
            }
        }
        inc(st.rxByType[type]);
        if (isCommunication(type)) {
            plainCommunication(s, type);
        } else if (isState(type)) {
            // State arrives plain and once a session, however many of its players
            // may receive it.
            inc(st.plainState);
            inc(s.io->plainState);
        }
        if (type == wire::ACTOR_UPDATE_NOTIFICATION_2) {
            onActorNotification(s, m, where, now);
        } else if (type == ACTOR_LEFT_NOTIFICATION) {
            onActorLeft(s, m, where, now);
        }
        return;
    }
    if (!trusted) noteOutsideSignedBundle(s, m, where, "none", now);
    inc(st.rxByType[type]);
    if (isCommunication(type)) plainCommunication(s, type); // channel text or audio
}

void FleetWorker::plainCommunication(Session& s, uint8_t type) {
    // A communication reaches a fleet session only inside a FLEET_DELIVERY that
    // lists who may receive it. One that arrives plain says nothing about who
    // may, so the game server could not deliver it by the rules: a server defect.
    inc(world_.stats.plainCommunications);
    inc(s.io->plainCommunications);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 5) {
        std::fprintf(stderr,
                     "[fleet] SERVER DEFECT: session %d received a communication (type %u, %s) "
                     "outside a FLEET_DELIVERY. Communications must always arrive in a 53 that "
                     "lists the players who may receive them\n",
                     s.index, type, messageTypeName(type));
    }
}

void FleetWorker::onActorNotification(Session& s, const wire::InboundView& m, Where where,
                                      int64_t now) {
    FleetStats& st = world_.stats;
    const auto payload = notificationPayload(m);
    if (!payload) return;
    const auto marker = readMarker(payload->data, payload->len);
    if (!marker || marker->runTag != world_.runTag || marker->player >= world_.cfg.players) {
        // Not this run's update: another payload, or a marker of another run of
        // this tool (state the platform kept from it).
        inc(marker || isEarlierMarker(payload->data, payload->len) ? st.actorUpdatesOtherRun
                                                                  : st.actorUpdatesOtherPayload);
        return;
    }
    inc(st.markersSeen);
    {
        // How far the update's chunk is from this session's region. Every player
        // the session carries stands inside its region, so an update from farther
        // than the replication distance is out of range of all of them.
        const int64_t cx = wire::readI64(m.data + wire::off::CHUNK_X);
        const int64_t cy = wire::readI64(m.data + wire::off::CHUNK_Y);
        const int64_t far = world_.grid.chunksFromRegion(s.index, cx, cy);
        inc(st.updatesByChunksFromRegion[static_cast<size_t>(std::min<int64_t>(far, 257))]);
        if (far > world_.cfg.distance + 1) {
            inc(st.updatesOutOfRange);
            inc(st.updatesOutOfRangeBytes, m.len);
            inc(s.io->updatesOutOfRange);
            inc(s.io->rxBytesOutOfRange, m.len);
            // One in 499 is described, so the events cover the whole run.
            if (outOfRangeSeen_++ % 499 == 0) {
                nlohmann::json e = {{"t_s", world_.phaseSeconds(now)},
                                    {"session", s.index},
                                    {"sent_by_session", marker->session},
                                    {"player", marker->player},
                                    {"where", whereName(static_cast<int>(where))},
                                    {"chunks_from_the_receiving_sessions_region", far}};
                bool nearFormer = false;
                describeNeighbourhood(s, cx, cy, now, e, &nearFormer);
                inc(st.updatesOutOfRangeSampled);
                if (nearFormer) inc(st.updatesOutOfRangeNearFormerPlayer);
                st.events.add("updates_from_out_of_range", std::move(e));
            }
        }
    }
    if (marker->first) {
        inc(st.firstUpdatesReceived);
        if (where != Where::SignedBundle && m.data[wire::off::CONTAINS_AUTH] == 0)
            inc(st.firstUpdatesReceivedUnsigned);
    }
    const int64_t sentNs = marker->sendUs * 1000;
    const PlayerShared& shared = world_.shared[static_cast<size_t>(marker->player)];
    const auto mine = s.byIndex.find(marker->player);
    const bool carriedHere = mine != s.byIndex.end() && mine->second->carried;

    if (marker->session == s.index) {
        // THIS session sent the update, and it came back to it. With
        // FLEET_OWN_STATE off that must not happen. The two cases are counted
        // apart: the session still carried the player when the update came back,
        // or the player had crossed to another session meanwhile.
        const bool unsignedOutside =
            where != Where::SignedBundle && m.data[wire::off::CONTAINS_AUTH] == 0;
        inc(st.ownStateReceived);
        inc(carriedHere ? st.ownSentWhileCarrying : st.ownSentAfterPlayerLeft);
        if (marker->first) inc(st.ownSentFirstUpdate);
        if (unsignedOutside) inc(st.ownSentUnsigned);
        st.ownSentAge.record(std::max<int64_t>(0, (now - sentNs) / 1000));
        const int64_t leftAt = shared.leftAtNs.load(std::memory_order_relaxed);
        const bool leftHere = shared.leftSession.load(std::memory_order_relaxed) == s.index;
        nlohmann::json e = {
            {"t_s", world_.phaseSeconds(now)},
            {"session", s.index},
            {"player", marker->player},
            {"update_age_ms", msBetween(now, sentNs)},
            {"where", whereName(static_cast<int>(where))},
            {"own_hmac", where == Where::SignedBundle ? "signed_bundle"
                                                      : unsignedOutside ? "none" : "verified"},
            {"first_update_after_a_carry_result", marker->first},
            {"session_still_carried_the_player", carriedHere},
            {"player_carried_by_session_now", shared.holder.load(std::memory_order_relaxed)}};
        if (carriedHere) {
            e["sent_ms_after_the_last_carry_result_here"] =
                msBetween(sentNs, mine->second->lastCarryResultNs);
            e["that_carry_was_for"] = carryCauseName(mine->second->carriedCause);
        } else if (leftHere && leftAt) {
            e["ms_since_the_player_left_this_session"] = msBetween(now, leftAt);
            e["sent_ms_before_the_player_left"] = msBetween(leftAt, sentNs);
            const int64_t nextCarry = shared.carriedAtNs.load(std::memory_order_relaxed);
            if (nextCarry > leftAt)
                e["ms_since_the_next_session_carry_result"] = msBetween(now, nextCarry);
        }
        st.events.add("own_state_sent_by_the_receiving_session", std::move(e));
        return;
    }

    if (carriedHere) {
        // Another session sent it, for a player this session carries now.
        const int64_t carriedAt = mine->second->carriedSinceNs;
        if (sentNs <= carriedAt) {
            // Sent before this session's carry result: the old session's last
            // updates, crossing the handoff. Expected.
            inc(st.otherSentBeforeHandoff);
            st.handoffLead.record((carriedAt - sentNs) / 1000);
        } else {
            // Sent after this session's carry result, by another session. The
            // client never does that (a session stops sending for a player the
            // moment the player leaves it), so this is counted apart.
            inc(st.otherSentAfterCarry);
            st.events.add("state_for_a_carried_player_sent_by_another_session_after_the_carry",
                          {{"t_s", world_.phaseSeconds(now)},
                           {"session", s.index},
                           {"player", marker->player},
                           {"sent_by_session", marker->session},
                           {"sent_ms_after_this_session_carry_result", msBetween(sentNs, carriedAt)},
                           {"update_age_ms", msBetween(now, sentNs)}});
        }
    }
    if (marker->sampled) {
        const int64_t us = now / 1000 - marker->sendUs;
        if (us >= 0) st.replicationLatency.record(us);
    }
}

void FleetWorker::onActorLeft(Session& s, const wire::InboundView& m, Where where, int64_t now) {
    // ACTOR_LEFT (145): the first payload byte is the reason (0 the actor went
    // stale, 1 its session was released).
    FleetStats& st = world_.stats;
    const auto payload = notificationPayload(m);
    const int reason = payload && payload->len >= 1 ? payload->data[0] : 255;
    inc(st.actorLeftByReason[static_cast<size_t>(reason)]);
    const auto actor = world_.playerOfActor.find(
        std::string(reinterpret_cast<const char*>(m.data + wire::off::UUID), 32));
    if (actor == world_.playerOfActor.end()) {
        inc(st.actorLeftUnknownActor);
        return;
    }
    const PlayerShared& shared = world_.shared[static_cast<size_t>(actor->second)];
    const int holder = shared.holder.load(std::memory_order_relaxed);
    inc(holder >= 0 ? st.actorLeftForCarriedPlayer : st.actorLeftForUncarriedPlayer);
    const int64_t far = world_.grid.chunksFromRegion(
        s.index, wire::readI64(m.data + wire::off::CHUNK_X),
        wire::readI64(m.data + wire::off::CHUNK_Y));
    if (far > world_.cfg.distance + 1) inc(st.actorLeftOutOfRange);
    const int64_t leftAt = shared.leftAtNs.load(std::memory_order_relaxed);
    const int64_t carriedAt = shared.carriedAtNs.load(std::memory_order_relaxed);
    nlohmann::json e = {{"t_s", world_.phaseSeconds(now)},
                        {"session", s.index},
                        {"where", whereName(static_cast<int>(where))},
                        {"reason", reason},
                        {"actor_player", actor->second},
                        {"actor_player_carried_by_session", holder},
                        {"chunks_from_the_receiving_sessions_region", far},
                        {"the_session_carried_that_player_before", shared.wasCarriedBy(s.index)}};
    if (carriedAt) e["ms_since_the_player_last_carry_result"] = msBetween(now, carriedAt);
    if (leftAt) e["ms_since_the_player_last_left_a_session"] = msBetween(now, leftAt);
    st.events.add("actor_left", std::move(e));
}

void FleetWorker::onCarryResult(Session& s, const CarryResult& r, int64_t now) {
    const FleetConfig& cfg = world_.cfg;
    FleetStats& st = world_.stats;
    const int64_t id = r.playerGameTokenId;
    auto it = s.byId.find(id);
    if (it == s.byId.end()) {
        // The player left this session before the answer came (or the id is not
        // one this session ever sent).
        inc(st.carryResultsLate);
        if (r.status == CARRY_CARRIED && s.departed.find(id) == s.departed.end() &&
            s.departedAlias.find(id) == s.departedAlias.end()) {
            // The abandoned carry landed after all, so a reason 1 will follow when
            // the next session's carry replaces it.
            Departed d;
            d.late = true;
            d.departNs = now;
            s.departed[id] = d;
        }
        return;
    }
    Player& p = *it->second;
    if (!p.carry.active || p.carry.id != id) {
        inc(st.carryResultsDuplicate); // a resend was answered as well
        return;
    }
    const CarryCause cause = p.carry.cause;

    if (r.status == CARRY_CARRIED) {
        st.carryLatency.record((now - p.carry.firstSentNs) / 1000);
        inc(st.carryCarried[static_cast<size_t>(cause)]);
        const bool idChanged = p.curId != id;
        if (p.curId && idChanged) {
            auto old = s.byId.find(p.curId);
            if (old != s.byId.end() && old->second == &p) s.byId.erase(old);
        }
        p.carry.active = false;
        p.curId = id;
        p.curVersion = p.carry.version;
        p.retryAtNs = 0;
        p.waitToken = false;
        p.unauthStreak = 0;
        if (idChanged) {
            wire::initActorUpdateTemplateFmt(p.msg, wire::PoseFormat::UE5, p.uuid,
                                             world_.cfg.appId, p.curId,
                                             static_cast<uint8_t>(world_.cfg.distance),
                                             static_cast<wire::DecayRate>(world_.cfg.decay));
        }
        p.lastCarryResultNs = now;
        s.lastCarriedNs = now;
        p.status1Retries = 0;
        p.carriedCause = cause;
        p.firstUpdateDue = true;
        if (!p.carried) {
            PlayerShared& shared = world_.shared[static_cast<size_t>(p.index)];
            shared.carriedAtNs.store(now, std::memory_order_relaxed);
            shared.holder.store(s.index, std::memory_order_relaxed);
            shared.noteCarriedBy(s.index);
            p.carried = true;
            p.carriedSinceNs = now;
            p.permCenterX = world_.grid.chunkX(p.walk.x);
            p.permCenterY = world_.grid.chunkY(p.walk.y);
            p.permReloadNs = 0;
            if (p.nextUpdateNs > now) p.nextUpdateNs = now; // take over its updates at once
        }
        if (p.crossingOpen) {
            p.crossingOpen = false;
            inc(st.crossingsCompleted);
            st.crossingLatency.record((now - p.crossStartNs) / 1000);
            if (p.originSession >= 0) {
                // Tell the session the player left when the new carry landed, so it
                // can time its FLEET_RELEASED against it.
                const int w = world_.workerOf(p.originSession);
                if (w == index_) {
                    onCrossingDone(*local(p.originSession), p.originId, p.originSeq, now);
                } else {
                    Mail m;
                    m.kind = Mail::Kind::CrossingDone;
                    m.session = p.originSession;
                    m.playerId = p.originId;
                    m.crossingSeq = p.originSeq;
                    m.resultNs = now;
                    (*peers_)[static_cast<size_t>(w)]->post(std::move(m));
                }
                p.originSession = -1;
            }
        }
        return;
    }

    inc(st.carryRefused[r.status]);
    switch (r.status) {
        case CARRY_NO_LIVE_TOKEN: {
            // "No such live token." Around a token refresh that is a matter of
            // timing, in one of two ways, and the carry is sent again (a new time
            // and proof) before it counts as a refusal:
            //   - the carry named the token the refresh has replaced since (the API
            //     retires it as it answers): carry at once with the new one;
            //   - the carry named the newest token, stored moments ago, or a refresh
            //     of it is in flight: the realtime server may not know the new one
            //     yet, or the old one is already retired. Wait a little and carry
            //     with whatever token is newest then.
            // The carry that stands under the old token id is left alone meanwhile,
            // and the player's updates go on under it.
            // Anything else is a refusal: the player signs in again.
            const uint32_t usedVersion = p.carry.version;
            const int64_t firstSent = p.carry.firstSentNs;
            const PlayerShared& shared = world_.shared[static_cast<size_t>(p.index)];
            const int64_t stored = shared.tokenStoredAtNs.load(std::memory_order_relaxed);
            const int64_t refreshStarted = shared.refreshStartedAtNs.load(std::memory_order_relaxed);
            const bool refreshed = shared.tokenRefreshed.load(std::memory_order_relaxed);
            endCarryEpisode(s, p);
            const uint32_t v = world_.creds[static_cast<size_t>(p.index)].version();
            const bool newer = v != usedVersion && !(v & 1);
            const bool recent =
                stored && now - stored < static_cast<int64_t>(cfg.status1RecentMs) * MS;
            const bool mayWait = (recent || refreshStarted || (v & 1)) &&
                                 p.status1Retries < cfg.status1Retries;
            const char* did = "signed_in_again";
            if (newer) {
                did = "carried_at_once_with_the_newer_token";
                ++p.status1Retries;
                inc(st.carryRetriedAfterRefresh);
                inc(st.carryRetriedWithNewerToken);
                startCarry(s, p, cause, now);
            } else if (mayWait) {
                did = "waited_and_carried_again";
                ++p.status1Retries;
                inc(st.carryRetriedAfterRefresh);
                inc(st.carryRetriedAfterWait);
                p.retryCause = cause;
                p.retryAtNs = now + static_cast<int64_t>(cfg.status1WaitMs) * MS;
            } else {
                inc(st.carryStatus1Refusals);
                p.status1Retries = 0;
                p.retryCause = cause;
                requestSignInAgain(p, now);
            }
            nlohmann::json e = {
                {"t_s", world_.phaseSeconds(now)},
                {"session", s.index},
                {"player", p.index},
                {"carry_cause", carryCauseName(cause)},
                {"answered_after_ms", firstSent ? msBetween(now, firstSent) : -1.0},
                {"named_the_newest_token_when_sent", !newer},
                {"token_came_from_a_refresh", refreshed},
                {"refresh_in_flight_when_answered", refreshStarted != 0},
                {"status_1_in_a_row", p.status1Retries},
                {"player_still_carried_under_the_old_token", p.carried},
                {"client_did", did}};
            if (stored && firstSent) {
                // Positive: the carry went out this long after the API's answer with
                // the newest token was stored. Negative: this long before it.
                e["carry_sent_ms_after_the_newest_token_was_stored"] = msBetween(firstSent, stored);
            }
            st.events.add("carry_status_1", std::move(e));
            break;
        }
        case CARRY_OTHER_ORGANIZATION:
            // Another organization's player: no retry can help.
            endCarryEpisode(s, p);
            if (p.carried)
                world_.shared[static_cast<size_t>(p.index)].holder.store(-1,
                                                                         std::memory_order_relaxed);
            p.carried = false;
            p.failed = true;
            inc(st.playersFailed);
            std::fprintf(stderr,
                         "[fleet] player %d refused for good: FLEET_CARRY_RESULT status 2 "
                         "(another organization's player)\n",
                         p.index);
            break;
        default: {
            // 3 (the proof or its time, or a replay), 4 (the session is full, or
            // there is no place for it) and anything unknown: the resend timer
            // tries again with a new time and proof until the five seconds are up.
            static std::atomic<bool> hinted3{false}, hinted4{false};
            if (r.status == CARRY_PROOF_OR_TIME && !hinted3.exchange(true)) {
                std::fprintf(stderr,
                             "[fleet] FLEET_CARRY_RESULT status 3 (the proof or its time failed, "
                             "or a replay): a time more than 30 s from the server's is refused, "
                             "so check this host's clock\n");
            } else if (r.status == CARRY_SESSION_FULL && !hinted4.exchange(true)) {
                std::fprintf(stderr,
                             "[fleet] FLEET_CARRY_RESULT status 4: session %d carries as many "
                             "players as it may (%d). Use more servers, fewer players, or ask "
                             "an operator to raise the organization's number\n",
                             s.index, s.info.maxPlayers);
            }
            break;
        }
    }
}

void FleetWorker::onCrossingDone(Session& s, int64_t playerId, uint32_t crossingSeq,
                                 int64_t resultNs) {
    auto it = s.departed.find(playerId);
    if (it == s.departed.end() || it->second.late || it->second.crossingSeq != crossingSeq)
        return;
    it->second.resultNs = resultNs;
    if (it->second.releasedNs) pairRelease(s, it);
}

void FleetWorker::pairRelease(Session& s, std::unordered_map<int64_t, Departed>::iterator it) {
    FleetStats& st = world_.stats;
    const int64_t delta = it->second.releasedNs - it->second.resultNs;
    inc(st.releaseReceived);
    if (delta < 0) inc(st.releaseBeforeResult);
    st.releaseDelay.record(delta > 0 ? delta / 1000 : 0);
    eraseDeparted(s, it);
}

std::unordered_map<int64_t, FleetWorker::Departed>::iterator FleetWorker::findDeparted(
    Session& s, int64_t id, bool* byAlias) {
    // A player that left is known by the token id its carry stood under. If it
    // left a carry with a refreshed token unanswered, and that carry landed, the
    // server knows it here under the new id, and its release names that one.
    if (byAlias) *byAlias = false;
    auto it = s.departed.find(id);
    if (it != s.departed.end()) return it;
    const auto alias = s.departedAlias.find(id);
    if (alias == s.departedAlias.end()) return s.departed.end();
    it = s.departed.find(alias->second);
    if (it != s.departed.end() && it->second.altId == id) {
        if (byAlias) *byAlias = true;
        return it;
    }
    return s.departed.end();
}

void FleetWorker::eraseDeparted(Session& s, std::unordered_map<int64_t, Departed>::iterator it) {
    if (it->second.altId) s.departedAlias.erase(it->second.altId);
    s.departed.erase(it);
}

void FleetWorker::onReleased(Session& s, const Released& r, int64_t now) {
    FleetStats& st = world_.stats;
    inc(st.releasedByReason[r.reason]);
    s.lastReleasedNs = now;
    const int64_t id = r.playerGameTokenId;

    if (r.reason == RELEASED_CARRIED_ELSEWHERE) {
        // The expected case: a player that crossed away from this session.
        bool byAlias = false;
        auto d = findDeparted(s, id, &byAlias);
        if (d != s.departed.end() && d->second.releasedNs == 0) {
            if (d->second.late) {
                inc(d->second.releasedFirst ? st.releasedAfterOwnRelease
                                            : st.releasedAfterLateCarry);
                eraseDeparted(s, d);
                return;
            }
            if (byAlias) inc(st.releaseNamedNewerToken);
            d->second.releasedNs = now;
            if (d->second.resultNs) pairRelease(s, d);
            return;
        }
    }

    auto it = s.byId.find(id);
    if (it == s.byId.end() || it->second->curId != id || !it->second->carried) {
        inc(st.releasedUnknownPlayer);
        return;
    }
    // The session holds this player and was told it no longer carries them.
    Player& p = *it->second;
    world_.shared[static_cast<size_t>(p.index)].holder.store(-1, std::memory_order_relaxed);
    p.carried = false;
    p.carryEndedNs = now;
    switch (r.reason) {
        case RELEASED_CARRIED_ELSEWHERE:
            // Another session's carry replaced this one while the world says the
            // player is here (a late datagram of a session the player left).
            inc(st.releasedUnexpected);
            inc(st.recarriedAfterRelease);
            if (!p.carry.active) startCarry(s, p, CarryCause::Recarry, now);
            break;
        case RELEASED_SIGN_IN_ENDED: {
            inc(st.released2Held);
            static std::atomic<bool> said2{false};
            if (!said2.exchange(true)) {
                std::fprintf(stderr,
                             "[fleet] FLEET_RELEASED reason 2 (the sign-in ended) for player %d "
                             "on session %d: signing the player in again. Later ones are "
                             "counted in the summary\n",
                             p.index, s.index);
            }
            if (p.carry.active) break; // a carry with a newer token is on its way
            const uint32_t v = world_.creds[static_cast<size_t>(p.index)].version();
            if (v != p.curVersion && !(v & 1)) {
                startCarry(s, p, CarryCause::TokenRefresh, now);
            } else {
                p.retryCause = CarryCause::SignInAgain;
                requestSignInAgain(p, now);
            }
            break;
        }
        case RELEASED_IDLE: {
            inc(st.released5Held);
            static std::atomic<bool> said5{false};
            if (!said5.exchange(true)) {
                std::fprintf(stderr,
                             "[fleet] FLEET_RELEASED reason 5 (idle) for player %d on session "
                             "%d: carrying the player again. Later ones are counted in the "
                             "summary\n",
                             p.index, s.index);
            }
            inc(st.recarriedAfterRelease);
            if (!p.carry.active) startCarry(s, p, CarryCause::Recarry, now);
            break;
        }
        default:
            inc(st.releasedOtherHeld);
            inc(st.recarriedAfterRelease);
            if (!p.carry.active) startCarry(s, p, CarryCause::Recarry, now);
            break;
    }
}

void FleetWorker::noteSessionError(Session& s, int64_t now) {
    // Twenty errors within two seconds that refuse the session itself (its
    // token, or one of its own messages): ask the API to authorize it again,
    // naming the server it is on.
    if (now - s.errWindowNs > 2 * SEC) {
        s.errWindowNs = now;
        s.errStreak = 0;
    }
    if (++s.errStreak >= ERROR_STREAK_MOVE) {
        s.errStreak = 0;
        requestMove(s, MoveTrigger::ErrorStreak, now);
    }
}

void FleetWorker::onError(Session& s, const wire::InboundView& m, int64_t now) {
    const FleetConfig& cfg = world_.cfg;
    FleetStats& st = world_.stats;
    if (m.len < 3) {
        inc(st.rxMalformed);
        return;
    }
    const uint8_t seq = m.data[1];
    const uint8_t code = m.data[2];
    inc(st.errorCodes[code]);

    // The sequence number wraps every 256 messages, so the entry is trusted only
    // while it is fresh.
    const SeqEntry& e = s.ring[seq];
    const bool fresh = e.kind != SendKind::None && now - e.sentNs < 2 * SEC;

    if (code == wire::ERR_INVALID_TOKEN || code == wire::ERR_TOKEN_EXPIRED ||
        code == wire::ERR_USER_NOT_AUTHENTICATED) {
        // The session's own token is refused: ask the API to authorize it again.
        noteSessionError(s, now);
        return;
    }
    if (code != wire::ERR_UNAUTHORIZED) return;

    const auto describe = [&](const char* cls) {
        nlohmann::json ev = {{"t_s", world_.phaseSeconds(now)},
                             {"session", s.index},
                             {"class", cls},
                             {"sequence_number", seq}};
        if (fresh) {
            ev["message_type"] = e.type;
            ev["message_name"] = messageTypeName(e.type);
            ev["refused_ms_after_it_was_sent"] = msBetween(now, e.sentNs);
        }
        return ev;
    };

    if (!fresh) {
        // The sequence number names no message this session sent in the last two
        // seconds, so nothing says what was refused.
        inc(st.unauthUnexplained);
        st.events.add("unauthorized_unexplained", describe("unexplained"));
        return;
    }
    if (e.kind != SendKind::Update) {
        // One of the session's own messages (a carry, a release, its
        // capabilities or a heartbeat) was refused.
        inc(st.unauthSessionMessages);
        st.events.add("unauthorized_session_messages", describe("session_messages"));
        noteSessionError(s, now);
        return;
    }

    // A message sent for a player was refused. What the client knew when it sent
    // it, and what it knows now, decide the class.
    inc(st.playerMessagesRefused);
    const PlayerShared& shared = world_.shared[static_cast<size_t>(e.player)];
    const auto held = s.byIndex.find(e.player);
    Player* p = held == s.byIndex.end() ? nullptr : held->second;
    const int64_t stored = shared.tokenStoredAtNs.load(std::memory_order_relaxed);
    const bool refreshed = shared.tokenRefreshed.load(std::memory_order_relaxed);

    nlohmann::json ev = describe("");
    ev["player"] = e.player;
    ev["carry_with_a_new_token_unanswered_when_sent"] = e.tokenCarryPending;
    ev["token_refresh_in_flight_when_sent"] = e.refreshInFlight;
    if (stored) {
        // Positive: sent this long after the API's answer with the player's newest
        // token was stored. Negative: this long before it.
        ev["sent_ms_after_the_newest_token_was_stored"] = msBetween(e.sentNs, stored);
    }
    if (p && p->lastCarryResultNs) {
        // Positive: sent this long after the player's last carry result on this
        // session. Negative: that result (the new token's) came this long after.
        ev["sent_ms_after_the_last_carry_result_here"] = msBetween(e.sentNs, p->lastCarryResultNs);
    }
    const auto finish = [&](const char* cls, const char* kind) {
        ev["class"] = cls;
        st.events.add(kind, std::move(ev));
    };

    // 1. Around a token refresh. The update named the token id a refresh was
    //    replacing or has replaced: it was sent while the carry with the new token
    //    was unanswered (the realtime server moves the player to the new id when
    //    it accepts that carry, and the client learns it from the result), or
    //    while the API was answering the refresh.
    uint8_t newestToken[hmac::TOKEN_OCTETS];
    int64_t newestId = 0;
    uint32_t newestVersion = 0;
    const bool haveNewest =
        world_.creds[static_cast<size_t>(e.player)].load(newestToken, newestId, newestVersion);
    const bool namedReplacedToken = haveNewest && newestId != e.id;
    ev["named_a_token_id_replaced_since"] = namedReplacedToken;
    ev["newest_token_came_from_a_refresh"] = refreshed;
    if (e.tokenCarryPending || e.refreshInFlight || namedReplacedToken) {
        inc(st.unauthTokenRefresh);
        const bool afterResult = p ? p->curId != e.id : true;
        inc(afterResult ? st.unauthTokenRefreshAfterResult : st.unauthTokenRefreshBeforeResult);
        ev["new_carry_answered_when_the_refusal_arrived"] = afterResult;
        ev["player_still_on_this_session"] = p != nullptr;
        finish("during_token_refresh", "unauthorized_during_token_refresh");
        return;
    }

    // 2. The player has left this session since: its last update crossed the new
    //    session's carry on the way.
    if (!p) {
        inc(st.unauthAfterHandoff);
        const int64_t leftAt = shared.leftAtNs.load(std::memory_order_relaxed);
        if (leftAt && shared.leftSession.load(std::memory_order_relaxed) == s.index)
            ev["sent_ms_before_the_player_left"] = msBetween(leftAt, e.sentNs);
        finish("after_handoff", "unauthorized_after_handoff");
        return;
    }
    // 3. The session stopped carrying the player after the update was sent (a
    //    FLEET_RELEASED for a player it held, or the session moved).
    if (!p->carried || p->curId != e.id) {
        inc(st.unauthAfterCarryEnded);
        if (p->carryEndedNs) ev["sent_ms_before_the_carry_ended"] = msBetween(p->carryEndedNs, e.sentNs);
        finish("after_the_carry_ended", "unauthorized_after_the_carry_ended");
        return;
    }

    const int64_t cx = world_.grid.chunkX(p->walk.x), cy = world_.grid.chunkY(p->walk.y);
    if (e.sentNs - p->carriedSinceNs < static_cast<int64_t>(cfg.firstContactGraceMs) * MS) {
        // 4. As for a player's own session: the first message makes the server load
        //    the player's permissions, and the messages sent meanwhile are refused.
        inc(st.unauthFirstContact);
        finish("first_contact", "unauthorized_first_contact");
        return;
    }
    const bool sameEpisode =
        p->permReloadNs && now - p->permReloadNs < static_cast<int64_t>(cfg.permReloadGraceMs) * MS;
    const int64_t r = cfg.permWindowRadiusChunks;
    if (sameEpisode || std::llabs(cx - p->permCenterX) > r || std::llabs(cy - p->permCenterY) > r) {
        // 5. The player walked out of the box of chunks its permissions were loaded
        //    for; the server loads the next one (the existing harness's model).
        inc(st.unauthWindowReload);
        ev["chunks_from_the_window_centre"] =
            std::max(std::llabs(cx - p->permCenterX), std::llabs(cy - p->permCenterY));
        if (!sameEpisode) {
            p->permCenterX = cx;
            p->permCenterY = cy;
            p->permReloadNs = now;
        }
        finish("window_reload", "unauthorized_window_reload");
        return;
    }
    // 6. None of those: the session carried the player under this id when it sent
    //    the update, and still does.
    inc(st.unauthUnexplained);
    finish("unexplained", "unauthorized_unexplained");
    // A steady stream means the realtime servers no longer hold this carry (a
    // lost FLEET_RELEASED, a server that restarted): carry the player again.
    const int limit = std::max(5, static_cast<int>(5 * cfg.updateHz));
    if (++p->unauthStreak >= limit && !p->carry.active) {
        p->unauthStreak = 0;
        inc(st.recarriedAfterUnauthorized);
        startCarry(s, *p, CarryCause::Recarry, now);
    }
}

// ---------------------------------------------------------------------------
// After the run
// ---------------------------------------------------------------------------

FleetWorker::End FleetWorker::end() const {
    End e;
    for (const auto& s : sessions_) {
        for (const auto& p : s.players) {
            ++e.players;
            if (p->failed) {
                ++e.failed;
            } else if (p->carried) {
                ++e.carried;
            } else {
                ++e.notCarried;
            }
            if (p->crossingOpen && !p->failed) ++e.crossingsOpen;
        }
        for (const auto& [id, d] : s.departed) {
            (void)id;
            if (!d.late && d.resultNs && !d.releasedNs) ++e.releasesPending;
        }
    }
    return e;
}

std::vector<FleetWorker::MinuteUse> FleetWorker::usage() const {
    std::vector<MinuteUse> out;
    for (const auto& s : sessions_) {
        for (const auto& [minute, use] : s.minutes) {
            (void)minute;
            out.push_back(use);
        }
    }
    return out;
}

std::vector<FleetSessionInfo> FleetWorker::sessionInfos() const {
    std::vector<FleetSessionInfo> out;
    for (const auto& s : sessions_) out.push_back(s.info);
    return out;
}

} // namespace lt::fleet
