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
      creds(static_cast<size_t>(c.players)) {
    // A tag for this run, so an update left over from another run of the tool in
    // the same app is not read as this run's.
    runTag = static_cast<uint16_t>(
        mix64(c.seed ^ static_cast<uint64_t>(epochMs()) ^
              (static_cast<uint64_t>(getpid()) << 32)) &
        0xFFFF);
    sampleStride = c.latencySample > 0 ? std::max(1, c.players / c.latencySample) : 0;
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
                if (s->state == Session::State::Active) {
                    // The API named another server while the session was up (a keep
                    // refresh): leave the old one as a move does.
                    closeSocket(*s);
                    for (auto& p : s->players) {
                        if (p->carry.active) endCarryEpisode(*s, *p);
                        p->carried = false;
                        p->retryAtNs = 0;
                    }
                    s->byId.clear();
                }
                inc(world_.stats.sessionMovesCompleted);
                s->lastMoveNs = now;
                s->needCarryAll = true;
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
    s.lastHeartbeatNs = now;
    s.lastRxNs = now;
    s.errStreak = 0;
    s.sendErrStreak = 0;
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
        drainMail(now);
        for (auto& s : sessions_) {
            if (s.state == Session::State::Active) tickSession(s, now);
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
        if (now - p.carry.firstSentNs >= static_cast<int64_t>(cfg.carryGiveUpMs) * MS) {
            // No result in five seconds: give up, count it, and start again shortly.
            inc(st.carryTimedOut);
            p.retryCause = p.carry.cause;
            endCarryEpisode(s, p);
            p.retryAtNs = now + static_cast<int64_t>(cfg.carryRetryMs) * MS;
        } else if (now - p.carry.lastSentNs >= static_cast<int64_t>(cfg.carryResendMs) * MS) {
            sendCarry(s, p, now); // a fresh time and proof
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

        for (auto it = s.departed.begin(); it != s.departed.end();) {
            Departed& d = it->second;
            if (d.late) {
                if (now - d.departNs > LATE_ENTRY_NS) {
                    it = s.departed.erase(it);
                    continue;
                }
            } else if (d.resultNs && !d.releasedNs) {
                if (now - d.resultNs > static_cast<int64_t>(cfg.releaseWaitMs) * MS) {
                    inc(st.releaseMissing);
                    it = s.departed.erase(it);
                    continue;
                }
                ++awaiting;
            } else if (now - d.departNs > UNPAIRED_ENTRY_NS) {
                inc(st.releaseUnpaired);
                it = s.departed.erase(it);
                continue;
            } else if (now - d.departNs < static_cast<int64_t>(cfg.carryGiveUpMs) * MS) {
                ++awaiting; // the new session's result is still on its way
            }
            ++it;
        }

        if (s.state == Session::State::Active) {
            ++active;
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
            r.current = s.info.server;
            r.gameTokenId = s.info.gameTokenId;
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
                         int64_t now) {
    if (s.fd < 0) return false;
    // The sequence number was written into `buf` by the builder: remember what it
    // was for, so an error that echoes it can be laid at the right message.
    s.ring[s.seq] = SeqEntry{id, now, kind};
    ++s.seq;
    const ssize_t n = send(s.fd, buf, len, 0);
    if (n == static_cast<ssize_t>(len)) {
        inc(s.io->txBytes, len);
        inc(s.io->txDatagrams);
        inc(world_.stats.txBytes, len);
        inc(world_.stats.txDatagrams);
        s.sendErrStreak = 0;
        return true;
    }
    inc(world_.stats.txSendErrors);
    // A realtime server that went away answers with ICMP, which shows as
    // ECONNREFUSED on the next send. A full socket buffer is this host's own.
    if (errno == ECONNREFUSED && ++s.sendErrStreak >= SEND_ERROR_STREAK_MOVE) {
        s.sendErrStreak = 0;
        requestMove(s, MoveTrigger::SendErrors, now);
    }
    return false;
}

void FleetWorker::sendCapabilities(Session& s, int64_t now) {
    s.lastCapsNs = now;
    uint32_t flags = wire::CAP_BUNDLE_SIGNED;
    if (world_.cfg.ownState) flags |= CAP_FLEET_OWN_STATE;
    uint8_t buf[wire::CAPABILITIES_SIZE];
    if (wire::buildCapabilities(buf, world_.cfg.appId, s.info.gameTokenId, flags, s.seq, s.token) &&
        sendOn(s, buf, sizeof(buf), SendKind::Capabilities, s.info.gameTokenId, now)) {
        inc(world_.stats.capabilitiesSent);
    }
}

void FleetWorker::sendHeartbeat(Session& s, int64_t now) {
    s.lastHeartbeatNs = now;
    uint8_t buf[HEARTBEAT_SIZE];
    const size_t len = buildHeartbeat(buf, world_.cfg.appId, s.info.gameTokenId, s.seq, s.token);
    if (len && sendOn(s, buf, len, SendKind::Heartbeat, s.info.gameTokenId, now))
        inc(world_.stats.heartbeatsSent);
}

void FleetWorker::sendCarry(Session& s, Player& p, int64_t now) {
    p.carry.lastSentNs = now;
    uint8_t buf[FLEET_CARRY_SIZE];
    const size_t len = buildCarry(buf, world_.cfg.appId, s.info.gameTokenId, p.carry.id,
                                  epochMs(), p.carry.token, s.seq, s.token);
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
    pose.pitch = markerCode(static_cast<uint16_t>(s.index), world_.runTag, p.sampled);
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
               now)) {
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
    p.carry.firstSentNs = now;
    p.retryCause = cause;
    p.retryAtNs = 0;
    s.byId[id] = &p;
    inc(world_.stats.carryAttempts[static_cast<size_t>(cause)]);
    sendCarry(s, p, now);
}

void FleetWorker::endCarryEpisode(Session& s, Player& p) {
    if (p.carry.active && p.carry.id != p.curId) s.byId.erase(p.carry.id);
    p.carry.active = false;
}

void FleetWorker::adopt(Session& s, std::unique_ptr<Player> owned, CarryCause cause,
                        int64_t now) {
    Player& p = *owned;
    s.players.push_back(std::move(owned));
    if (p.lastMoveNs == 0) p.lastMoveNs = now;
    if (p.nextUpdateNs == 0) {
        // Spread first sends over one interval so a batch of joins is not a burst.
        std::uniform_int_distribution<int64_t> off(0, updateIntervalNs_);
        p.nextUpdateNs = now + off(p.rng);
    }
    p.retryCause = cause;
    p.retryAtNs = 0;
    if (s.state == Session::State::Active && now >= s.activateAtNs) startCarry(s, p, cause, now);
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
    if (p->carried) {
        // This session carried the player: FLEET_RELEASED reason 1 is due here
        // once the new session's carry lands.
        auto old = s.departed.find(p->curId);
        if (old != s.departed.end() && !old->second.late) inc(st.releaseUnpaired);
        Departed d;
        d.player = p->index;
        d.crossingSeq = p->crossingSeq;
        d.departNs = now;
        s.departed[p->curId] = d;
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

void FleetWorker::requestMove(Session& s, MoveTrigger trigger, int64_t now) {
    if (s.state != Session::State::Active) return;
    if (s.lastMoveNs && now - s.lastMoveNs < MOVE_COOLDOWN_NS) return; // the last move settles
    inc(world_.stats.sessionMoves[static_cast<size_t>(trigger)]);
    closeSocket(s);
    s.state = Session::State::Moving;
    s.moveRetryAtNs = 0;
    for (auto& p : s.players) {
        if (p->carry.active) endCarryEpisode(s, *p);
        p->carried = false;
        p->retryAtNs = 0;
    }
    s.byId.clear();
    ApiRequest r;
    r.kind = ApiRequest::Kind::MoveSession;
    r.index = s.index;
    r.current = s.info.server;
    r.gameTokenId = s.info.gameTokenId;
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
    s.lastRxNs = now;
    if (len == 0) return;

    // The two commands that tell a session to move are always their own datagram.
    if (data[0] == wire::COMMAND_RECONNECT) {
        if (!verifyReconnect(data, len, s.token)) {
            inc(st.rxHmacFailures);
            return;
        }
        inc(st.reconnectCommands);
        requestMove(s, MoveTrigger::ReconnectCommand, now);
        return;
    }
    if (data[0] == COMMAND_SESSION_RELEASED) {
        const auto reason = verifySessionReleased(data, len, s.token);
        if (!reason) {
            inc(st.rxHmacFailures);
            return;
        }
        inc(st.sessionReleasedByReason[*reason]);
        requestMove(s, MoveTrigger::SessionReleased, now);
        return;
    }

    const bool signedBundle = data[0] == wire::MESSAGE_BUNDLE_SIGNED;
    if (signedBundle) {
        // One HMAC over the datagram, keyed with the fleet token; the members
        // carry none. forEachMessage strips the tail.
        inc(st.rxSignedBundles);
        if (!wire::verifySignedBundle(data, len, s.token)) {
            inc(st.rxHmacFailures);
            return;
        }
    } else if (data[0] == wire::MESSAGE_BUNDLE) {
        inc(st.rxPlainBundles);
    }
    const bool ok = wire::forEachMessage(data, len, [&](const wire::InboundView& m) {
        handleMessage(s, m, signedBundle, now);
    });
    if (!ok) inc(st.rxMalformed);
}

void FleetWorker::handleMessage(Session& s, const wire::InboundView& m, bool trusted,
                                int64_t now) {
    FleetStats& st = world_.stats;
    const uint8_t type = m.type();
    if (s.state != Session::State::Active) return; // a handler in this datagram moved the session
    switch (type) {
        case wire::GENERIC_ERROR_MESSAGE:
            inc(st.rxByType[type]);
            onError(s, m, now);
            return;
        case FLEET_CARRY_RESULT: {
            const auto r = parseCarryResult(m, world_.cfg.appId);
            if (!r) {
                inc(st.rxMalformed);
                return;
            }
            inc(st.rxByType[type]);
            if (!trusted) inc(st.fleetFramesUnsigned);
            if (looksLongForm(m, world_.cfg.appId)) inc(st.fleetFramesLongForm);
            onCarryResult(s, *r, now);
            return;
        }
        case FLEET_RELEASED: {
            const auto r = parseReleased(m, world_.cfg.appId);
            if (!r) {
                inc(st.rxMalformed);
                return;
            }
            inc(st.rxByType[type]);
            if (!trusted) inc(st.fleetFramesUnsigned);
            if (looksLongForm(m, world_.cfg.appId)) inc(st.fleetFramesLongForm);
            onReleased(s, *r, now);
            return;
        }
        case FLEET_DELIVERY: {
            const auto d = parseDelivery(m, world_.cfg.appId);
            if (!d) {
                inc(st.rxMalformed);
                return;
            }
            inc(st.rxByType[type]);
            if (!trusted) inc(st.fleetFramesUnsigned);
            inc(st.deliveries);
            inc(st.deliveryPlayers, d->count);
            inc(st.deliveryByType[d->notification.type()]);
            for (size_t i = 0; i < d->count; ++i) {
                // A communication must list only players this session carries.
                auto it = s.byId.find(d->player(i));
                if (it == s.byId.end() || !it->second->carried) inc(st.deliveryForUncarried);
            }
            return;
        }
        default:
            break;
    }
    if (type & wire::SPATIAL_TYPE_BIT) {
        if (!trusted) {
            // Outside a signed bundle a notification carries its own HMAC.
            if (m.len > wire::off::CONTAINS_AUTH && m.data[wire::off::CONTAINS_AUTH] == 0) {
                inc(st.rxUnsignedSpatial);
            } else if (!wire::verifyNotification(m, s.token)) {
                inc(st.rxHmacFailures);
                return;
            }
        }
        inc(st.rxByType[type]);
        s.errStreak = 0;
        if (type == wire::ACTOR_UPDATE_NOTIFICATION_2) onActorNotification(s, m, now);
        return;
    }
    inc(st.rxByType[type]);
}

void FleetWorker::onActorNotification(Session& s, const wire::InboundView& m, int64_t now) {
    FleetStats& st = world_.stats;
    const auto payload = notificationPayload(m);
    if (!payload) return;
    const auto marker = readMarker(payload->data, payload->len);
    if (!marker || marker->runTag != world_.runTag) return; // not this run's update
    inc(st.markersSeen);
    if (marker->session == s.index) {
        // State one of this session's own carried players sent. It must not come
        // back unless the session set FLEET_OWN_STATE.
        inc(st.ownStateReceived);
        return;
    }
    if (marker->sampled) {
        const int64_t us = now / 1000 - marker->sendUs;
        if (us >= 0) st.replicationLatency.record(us);
    }
}

void FleetWorker::onCarryResult(Session& s, const CarryResult& r, int64_t now) {
    FleetStats& st = world_.stats;
    const int64_t id = r.playerGameTokenId;
    auto it = s.byId.find(id);
    if (it == s.byId.end()) {
        // The player left this session before the answer came (or the id is not
        // one this session ever sent).
        inc(st.carryResultsLate);
        if (r.status == CARRY_CARRIED && s.departed.find(id) == s.departed.end()) {
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
    s.errStreak = 0;
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
        if (!p.carried) {
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
            // The token is not live. Carry with a newer one if the API threads
            // have stored it meanwhile; otherwise the player signs in again.
            const uint32_t usedVersion = p.carry.version;
            endCarryEpisode(s, p);
            const uint32_t v = world_.creds[static_cast<size_t>(p.index)].version();
            if (v != usedVersion && !(v & 1)) {
                startCarry(s, p, cause, now);
            } else {
                p.retryCause = cause;
                requestSignInAgain(p, now);
            }
            break;
        }
        case CARRY_OTHER_ORG_OR_APP:
            // Another organization's or another app's player: no retry can help.
            endCarryEpisode(s, p);
            p.carried = false;
            p.failed = true;
            inc(st.playersFailed);
            std::fprintf(stderr,
                         "[fleet] player %d refused for good: FLEET_CARRY_RESULT status 2 "
                         "(another organization's or another app's player)\n",
                         p.index);
            break;
        default:
            // 3 (the proof or its time), 4 (the session is full) and anything
            // unknown: the resend timer tries again with a fresh time until the
            // five seconds are up.
            break;
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
    s.departed.erase(it);
}

void FleetWorker::onReleased(Session& s, const Released& r, int64_t now) {
    FleetStats& st = world_.stats;
    inc(st.releasedByReason[r.reason]);
    const int64_t id = r.playerGameTokenId;

    if (r.reason == RELEASED_CARRIED_ELSEWHERE) {
        // The expected case: a player that crossed away from this session.
        auto d = s.departed.find(id);
        if (d != s.departed.end() && d->second.releasedNs == 0) {
            if (d->second.late) {
                inc(st.releasedAfterLateCarry);
                s.departed.erase(d);
                return;
            }
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
    p.carried = false;
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
        case RELEASED_IDLE:
            inc(st.released5Held);
            inc(st.recarriedAfterRelease);
            if (!p.carry.active) startCarry(s, p, CarryCause::Recarry, now);
            break;
        default:
            inc(st.releasedOtherHeld);
            inc(st.recarriedAfterRelease);
            if (!p.carry.active) startCarry(s, p, CarryCause::Recarry, now);
            break;
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
        if (++s.errStreak >= ERROR_STREAK_MOVE) {
            s.errStreak = 0;
            requestMove(s, MoveTrigger::ErrorStreak, now);
        }
        return;
    }
    if (code != wire::ERR_UNAUTHORIZED) return;

    if (!fresh) {
        inc(st.unauthUnexplained);
        return;
    }
    if (e.kind != SendKind::Update) {
        // One of the session's own messages (a carry, a release, its
        // capabilities or a heartbeat) was refused.
        inc(st.unauthSessionMessages);
        if (++s.errStreak >= ERROR_STREAK_MOVE) {
            s.errStreak = 0;
            requestMove(s, MoveTrigger::ErrorStreak, now);
        }
        return;
    }

    // A message sent for a player was refused.
    inc(st.playerMessagesRefused);
    auto it = s.byId.find(e.id);
    if (it == s.byId.end() || it->second->curId != e.id || !it->second->carried) {
        // The player has left this session since: its last update crossed the
        // new session's carry on the way.
        inc(st.unauthAfterHandoff);
        return;
    }
    Player& p = *it->second;
    const int64_t cx = world_.grid.chunkX(p.walk.x), cy = world_.grid.chunkY(p.walk.y);
    if (now - p.carriedSinceNs < static_cast<int64_t>(cfg.firstContactGraceMs) * MS) {
        // As for a player's own session: the first message makes the server load
        // the player's permissions, and the messages sent meanwhile are refused.
        inc(st.unauthFirstContact);
        return;
    }
    const bool sameEpisode =
        p.permReloadNs && now - p.permReloadNs < static_cast<int64_t>(cfg.permReloadGraceMs) * MS;
    const int64_t r = cfg.permWindowRadiusChunks;
    if (sameEpisode || std::llabs(cx - p.permCenterX) > r || std::llabs(cy - p.permCenterY) > r) {
        // The player walked out of the box of chunks its permissions were loaded
        // for; the server loads the next one (the existing harness's model).
        inc(st.unauthWindowReload);
        if (!sameEpisode) {
            p.permCenterX = cx;
            p.permCenterY = cy;
            p.permReloadNs = now;
        }
        return;
    }
    inc(st.unauthUnexplained);
    // A steady stream means the realtime servers no longer hold this carry (a
    // lost FLEET_RELEASED, a server that restarted): carry the player again.
    const int limit = std::max(5, static_cast<int>(5 * cfg.updateHz));
    if (++p.unauthStreak >= limit && !p.carry.active) {
        p.unauthStreak = 0;
        inc(st.recarriedAfterUnauthorized);
        startCarry(s, p, CarryCause::Recarry, now);
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

std::vector<FleetSessionInfo> FleetWorker::sessionInfos() const {
    std::vector<FleetSessionInfo> out;
    for (const auto& s : sessions_) out.push_back(s.info);
    return out;
}

} // namespace lt::fleet
