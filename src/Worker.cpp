#include "Worker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace lt {

namespace {

/// How long after an assignment an UNAUTHORIZED is attributed to Buddy's lazy
/// permission-window load rather than to a real refusal.
///
/// Not tunable, on purpose: this is a statement about the server's behaviour,
/// not a preference. A knob here would let a run make its own error count look
/// clean by widening the window, which is the opposite of what the
/// classification is for. 2s is well clear of the observed load (a handful of
/// refusals inside the first few hundred milliseconds at 10 Hz) and well short
/// of anything a wedged session would stay quiet for.
constexpr int FIRST_CONTACT_GRACE_MS = 2000;

double steadySeconds() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t epochMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Worker::Worker(int index, const Config& config, Stats& stats,
               ControlQueue& control)
    : index_(index), config_(config), stats_(stats), control_(control) {
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
}

Worker::~Worker() {
    stop();
    for (auto& c : clients_) closeSocket(c);
    if (epollFd_ >= 0) close(epollFd_);
}

void Worker::addClient(ClientCredentials creds,
                       std::chrono::steady_clock::time_point activateAt) {
    SimClient c;
    c.creds = std::move(creds);
    c.activateAt = activateAt;
    clients_.push_back(std::move(c));
}

void Worker::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void Worker::stop() {
    bool was = running_.exchange(false);
    if (was && thread_.joinable()) thread_.join();
}

void Worker::postUpdate(ClientUpdate update) {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(update));
}

bool Worker::openSocket(SimClient& client) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(client.creds.serverPort);
    if (inet_pton(AF_INET, client.creds.serverIp4.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }
    // connect() pins the peer: send() is cheap and recv() only returns
    // datagrams from this Buddy.
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = static_cast<uint32_t>(&client - clients_.data());
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return false;
    }
    client.fd = fd;
    return true;
}

void Worker::closeSocket(SimClient& client) {
    if (client.fd >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, client.fd, nullptr);
        close(client.fd);
        client.fd = -1;
    }
}

void Worker::suspendClient(SimClient& client, ControlRequest::Kind kind) {
    if (client.state == SimClient::State::SUSPENDED) return;
    closeSocket(client);
    if (client.state == SimClient::State::ACTIVE) {
        stats_.activeClients.fetch_sub(1, std::memory_order_relaxed);
    }
    client.state = SimClient::State::SUSPENDED;
    stats_.suspendedClients.fetch_add(1, std::memory_order_relaxed);

    ControlRequest req;
    req.workerIndex = index_;
    req.clientIndex = static_cast<int>(&client - clients_.data());
    req.kind = kind;
    req.creds = client.creds;
    control_.push(std::move(req));
}

void Worker::applyPendingUpdates() {
    std::vector<ClientUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        if (inbox_.empty()) return;
        updates.swap(inbox_);
    }
    for (auto& u : updates) {
        SimClient& client = clients_[static_cast<size_t>(u.clientIndex)];
        bool wasSuspended = client.state == SimClient::State::SUSPENDED;
        if (u.failed) {
            // Control thread gave up on this client; it stays suspended.
            continue;
        }
        bool tokenChanged = u.creds.gameTokenId != client.creds.gameTokenId;
        client.creds = std::move(u.creds);
        client.refreshRequested = false;
        if (tokenChanged) client.rebuildTemplate(config_);

        if (wasSuspended) {
            // Resume once the fresh session settles (udpReadyAt from assign).
            stats_.suspendedClients.fetch_sub(1, std::memory_order_relaxed);
            client.state = SimClient::State::WAITING;
            client.activateAt = client.creds.udpReadyAt;
        } else if (client.state == SimClient::State::ACTIVE) {
            // Proactive refresh while running: move to the (possibly new)
            // server. Reopen the socket and keep simulating.
            closeSocket(client);
            client.state = SimClient::State::WAITING;
            client.activateAt = client.creds.udpReadyAt;
            stats_.activeClients.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void Worker::activateDueClients(std::chrono::steady_clock::time_point now,
                                double nowSec) {
    for (auto& client : clients_) {
        if (client.state != SimClient::State::WAITING) continue;
        if (now < client.activateAt || now < client.creds.udpReadyAt) continue;
        bool firstActivation = client.uuid[0] == '\0';
        if (firstActivation) {
            client.initSimulation(config_, nowSec);
        } else {
            // Re-activation after reassign/refresh: keep the walk state and
            // UUID, just resume timing.
            client.lastMoveTime = nowSec;
            client.lastSendTime = nowSec;
        }
        // The first-contact grace window is per ASSIGNMENT, not per client: a
        // new gameTokenId on a new Buddy has its own permission window to load.
        // Re-arm so a reassignment's expected refusals are not misfiled.
        client.activatedAt = now;
        client.permWindowCenterX = client.chunkX;
        client.permWindowCenterY = client.chunkY;
        client.permWindowCenterZ = client.chunkZ;
        client.permWindowValid = true;
        if (!openSocket(client)) {
            std::fprintf(stderr,
                         "[worker %d] client %d: cannot open UDP socket to %s:%u "
                         "(%s)\n",
                         index_, client.creds.index,
                         client.creds.serverIp4.c_str(), client.creds.serverPort,
                         std::strerror(errno));
            suspendClient(client, ControlRequest::Kind::REASSIGN);
            continue;
        }
        client.state = SimClient::State::ACTIVE;
        stats_.activeClients.fetch_add(1, std::memory_order_relaxed);
    }
}

void Worker::handleDatagram(SimClient& client, const uint8_t* data, size_t len) {
    stats_.rxDatagrams.fetch_add(1, std::memory_order_relaxed);
    stats_.rxBytes.fetch_add(len, std::memory_order_relaxed);
    if (len == 0) return;

    if (data[0] == wire::MESSAGE_BUNDLE) {
        stats_.rxBundles.fetch_add(1, std::memory_order_relaxed);
    }

    const int64_t nowMs = epochMillis();
    bool ok = wire::forEachMessage(data, len, [&](const wire::InboundView& m) {
        const uint8_t type = m.type();
        if (type == wire::GENERIC_ERROR_MESSAGE) {
            stats_.rxErrorMessages.fetch_add(1, std::memory_order_relaxed);
            if (m.len >= 3) {
                uint8_t code = m.data[2];
                stats_.errorCodes[code].fetch_add(1, std::memory_order_relaxed);
                // UNAUTHORIZED shortly after an assignment is Buddy loading the
                // permission window, which this client's own first packet
                // triggered. Classify by a TIME WINDOW rather than by counting
                // the first one: the load takes longer than one send interval,
                // so a client at 10 Hz legitimately sees two or three, and
                // treating only the first as expected reports the rest as
                // faults. The raw errorCodes tally above still counts every
                // refusal, so nothing is hidden by this classification.
                bool expectedFirstContact = false;
                if (code == wire::ERR_UNAUTHORIZED &&
                    client.activatedAt.time_since_epoch().count() != 0 &&
                    std::chrono::steady_clock::now() - client.activatedAt <
                        std::chrono::milliseconds(FIRST_CONTACT_GRACE_MS)) {
                    expectedFirstContact = true;
                    stats_.unauthorizedFirstContact.fetch_add(
                        1, std::memory_order_relaxed);
                } else if (code == wire::ERR_UNAUTHORIZED && client.permWindowValid) {
                    // The same lazy load, later in the run. The server caches
                    // grid permissions as a BOX of radius R chunks centred
                    // where the last lookup ran, so a walking client leaves it
                    // and the packet that crosses the edge is refused while the
                    // re-query is in flight. Measured: onset scales as 1/walk
                    // speed (t+101s at 150 uu/s, t+21s at 600), and both a
                    // stationary walk and a spawn radius of 0 produce none at
                    // all -- so this is geometry, not a clock.
                    //
                    // R is the harness's ASSUMPTION about a server-side
                    // constant it cannot read, so a refusal that arrives while
                    // the client is still inside the modelled box stays
                    // unexplained rather than being absorbed here. That is the
                    // case worth alarming about and it must remain reachable.
                    const int64_t r = config_.permWindowRadiusChunks;
                    const auto nowTp = std::chrono::steady_clock::now();
                    const bool sameEpisode =
                        client.permReloadAt.time_since_epoch().count() != 0 &&
                        nowTp - client.permReloadAt <
                            std::chrono::milliseconds(config_.permReloadGraceMs);
                    if (sameEpisode ||
                        std::llabs(client.chunkX - client.permWindowCenterX) > r ||
                        std::llabs(client.chunkY - client.permWindowCenterY) > r ||
                        std::llabs(client.chunkZ - client.permWindowCenterZ) > r) {
                        expectedFirstContact = true;
                        stats_.unauthorizedWindowReload.fetch_add(
                            1, std::memory_order_relaxed);
                        if (!sameEpisode) {
                            // Re-centre: the refusal triggered a fresh lookup
                            // centred here, so this is the box from now on.
                            client.permWindowCenterX = client.chunkX;
                            client.permWindowCenterY = client.chunkY;
                            client.permWindowCenterZ = client.chunkZ;
                            client.permReloadAt = nowTp;
                        }
                    }
                }
                if (!client.errorLogged && !expectedFirstContact) {
                    client.errorLogged = true;
                    std::fprintf(stderr,
                                 "[worker %d] client %d (%s) got error code %u "
                                 "from %s:%u (gameTokenId=%lld)\n",
                                 index_, client.creds.index,
                                 client.creds.email.c_str(), code,
                                 client.creds.serverIp4.c_str(),
                                 client.creds.serverPort,
                                 static_cast<long long>(client.creds.gameTokenId));
                }
                // Token-level failures: rotate the token and reinstall the
                // session. One request per client at a time.
                if ((code == wire::ERR_INVALID_TOKEN ||
                     code == wire::ERR_TOKEN_EXPIRED ||
                     code == wire::ERR_USER_NOT_AUTHENTICATED) &&
                    !client.refreshRequested) {
                    client.refreshRequested = true;
                    suspendClient(client, ControlRequest::Kind::REFRESH);
                } else if (code == wire::ERR_UNAUTHORIZED) {
                    // A handful of these are normal right after (re)assignment
                    // (the server lazily loads permission windows). A steady
                    // stream means the session is wedged; re-register on a
                    // fresh assignment, like a real client would.
                    if (++client.unauthorizedCount >= 5 * config_.updateHz) {
                        client.unauthorizedCount = 0;
                        suspendClient(client, ControlRequest::Kind::REASSIGN);
                    }
                }
            }
            return;
        }
        if (type == wire::COMMAND_RECONNECT) {
            stats_.rxReconnectCommands.fetch_add(1, std::memory_order_relaxed);
            suspendClient(client, ControlRequest::Kind::REASSIGN);
            return;
        }
        if (type & wire::SPATIAL_TYPE_BIT) {
            if (config_.verifyServerHmac &&
                !wire::verifyNotification(
                    m, reinterpret_cast<const uint8_t*>(client.creds.appToken.data()))) {
                stats_.rxHmacFailures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (type == wire::ACTOR_UPDATE_NOTIFICATION_2) {
                stats_.rxActorNotifications.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.rxOtherSpatial.fetch_add(1, std::memory_order_relaxed);
            }
            if (auto epoch = wire::notificationEpochMs(m); epoch && *epoch > 0) {
                stats_.recordLatencyMs(nowMs - *epoch);
            }
        }
    });
    if (!ok) stats_.rxMalformed.fetch_add(1, std::memory_order_relaxed);
}

void Worker::drainSocket(int clientIndex) {
    SimClient& client = clients_[static_cast<size_t>(clientIndex)];
    if (client.fd < 0) return;
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recv(client.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            // EAGAIN: drained. ECONNREFUSED: ICMP port-unreachable from a
            // Buddy restart; the datagram was lost, keep going.
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == ECONNREFUSED) continue;
            break;
        }
        if (client.state != SimClient::State::ACTIVE) continue;
        handleDatagram(client, buf, static_cast<size_t>(n));
        // handleDatagram may suspend the client (closing the fd).
        if (client.fd < 0) break;
    }
}

void Worker::sendDueUpdates(double nowSec) {
    const double interval = 1.0 / config_.updateHz;
    const auto sysNow = std::chrono::system_clock::now();

    for (auto& client : clients_) {
        if (client.state != SimClient::State::ACTIVE) continue;
        if (nowSec - client.lastSendTime < interval) continue;
        client.lastSendTime += interval;
        // If we fell far behind (scheduler stall), don't burst-catch-up.
        if (nowSec - client.lastSendTime > interval) client.lastSendTime = nowSec;

        // Proactive token refresh: request once when inside the lead window.
        if (!client.refreshRequested &&
            sysNow + std::chrono::seconds(config_.tokenRefreshLeadSec) >=
                client.creds.tokenExpiresAt) {
            client.refreshRequested = true;
            ControlRequest req;
            req.workerIndex = index_;
            req.clientIndex = static_cast<int>(&client - clients_.data());
            req.kind = ControlRequest::Kind::REFRESH;
            req.creds = client.creds;
            control_.push(std::move(req));
            // Keep sending with the current token until the update arrives.
        }

        client.updateWalk(config_, nowSec);
        if (!client.buildUpdate(config_)) {
            stats_.txSendErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ssize_t n = send(client.fd, client.message, wire::ACTOR_UPDATE_SIZE, 0);
        if (n == static_cast<ssize_t>(wire::ACTOR_UPDATE_SIZE)) {
            stats_.txPackets.fetch_add(1, std::memory_order_relaxed);
            stats_.txBytes.fetch_add(wire::ACTOR_UPDATE_SIZE,
                                     std::memory_order_relaxed);
            client.consecutiveSendErrors = 0;
        } else {
            stats_.txSendErrors.fetch_add(1, std::memory_order_relaxed);
            // A dead Buddy shows up as persistent ECONNREFUSED; move the
            // client to another server after ~3 seconds of failures.
            if (++client.consecutiveSendErrors >= 3 * config_.updateHz) {
                client.consecutiveSendErrors = 0;
                suspendClient(client, ControlRequest::Kind::REASSIGN);
            }
        }
    }
}

void Worker::run() {
    constexpr int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_acquire)) {
        applyPendingUpdates();

        auto now = std::chrono::steady_clock::now();
        double nowSec = steadySeconds();
        activateDueClients(now, nowSec);
        sendDueUpdates(nowSec);

        // Block on RX until roughly the next tick edge. 5ms keeps per-client
        // send jitter well under one update interval at typical rates.
        int timeoutMs = 5;
        int n = epoll_wait(epollFd_, events, MAX_EVENTS, timeoutMs);
        for (int i = 0; i < n; ++i) {
            drainSocket(static_cast<int>(events[i].data.u32));
        }
    }
}

} // namespace lt
