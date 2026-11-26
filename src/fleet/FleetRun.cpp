#include "fleet/FleetRun.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace lt::fleet {

namespace {

constexpr int64_t MS = 1000000;
constexpr int64_t SEC = 1000 * MS;

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t get(const Counter& c) { return c.load(std::memory_order_relaxed); }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// Only the entries that are not zero, keyed by the index as a string.
template <size_t N> nlohmann::json nonZero(const std::array<Counter, N>& a) {
    nlohmann::json j = nlohmann::json::object();
    for (size_t i = 0; i < N; ++i) {
        if (get(a[i])) j[std::to_string(i)] = get(a[i]);
    }
    return j;
}

constexpr uint64_t kWireOverhead = 42; // Ethernet 14 + IPv4 20 + UDP 8

std::string minuteIso(int64_t minute) {
    const time_t secs = static_cast<time_t>(minute * 60);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:00Z", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

uint64_t notificationsReceived(const FleetStats& st) {
    uint64_t n = 0;
    for (size_t t = 128; t < 256; ++t) n += get(st.rxByType[t]);
    return n;
}

} // namespace

FleetRun::FleetRun(FleetConfig cfg, RunDeps deps, std::atomic<bool>& stop)
    : cfg_(std::move(cfg)), deps_(std::move(deps)), stop_(stop), stats_(cfg_.servers) {}

FleetRun::~FleetRun() {
    {
        std::lock_guard<std::mutex> lock(jobMu_);
        jobsClosed_ = true;
    }
    jobCv_.notify_all();
    for (auto& t : apiThreads_) {
        if (t.joinable()) t.join();
    }
    for (auto& w : workers_) w->stop();
}

// ---------------------------------------------------------------------------
// The API threads
// ---------------------------------------------------------------------------

void FleetRun::push(Job j) {
    {
        std::lock_guard<std::mutex> lock(jobMu_);
        if (jobsClosed_) return;
        jobs_.push_back(std::move(j));
    }
    jobCv_.notify_one();
}

bool FleetRun::pop(Job& out) {
    std::unique_lock<std::mutex> lock(jobMu_);
    jobCv_.wait(lock, [this] { return !jobs_.empty() || jobsClosed_; });
    if (jobs_.empty()) return false;
    out = std::move(jobs_.front());
    jobs_.pop_front();
    jobsInFlight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void FleetRun::apiThread() {
    // One transport a thread: a libcurl handle is not shared.
    std::unique_ptr<ApiTransport> transport;
    try {
        transport = deps_.makeTransport();
    } catch (const std::exception& e) {
        setFatal(std::string("cannot reach the Application API: ") + e.what());
        return;
    }
    FleetApi api(std::move(transport), cfg_.orgToken, cfg_.appId, stats_.api, &stop_);
    Job j;
    while (pop(j)) {
        runJob(api, j);
        jobsInFlight_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void FleetRun::setFatal(const std::string& why) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fatal_.empty()) fatal_ = why;
    hasFatal_.store(true);
}

void FleetRun::storeToken(int player, const PlayerToken& t, bool fromRefresh) {
    // The time is written first, so a worker that sees the new token also sees
    // when it came.
    PlayerShared& shared = world_->shared[static_cast<size_t>(player)];
    shared.tokenRefreshed.store(fromRefresh, std::memory_order_relaxed);
    shared.tokenStoredAtNs.store(nowNs(), std::memory_order_relaxed);
    world_->creds[static_cast<size_t>(player)].store(t);
    if (!cfg_.rosterFile.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        roster_.players[player] = t;
    }
}

void FleetRun::admit(int player) {
    // Where the player stands depends on the seed and the player alone, so a run
    // can be repeated.
    std::mt19937_64 rng(mix64(cfg_.seed * 0x2545F4914F6CDD1DULL + static_cast<uint64_t>(player)));
    Walker w;
    placeWalker(w, world_->grid, rng);
    const int session = world_->grid.regionAt(w.x, w.y);
    workers_[static_cast<size_t>(world_->workerOf(session))]->admitPlayer(player, w);
}

void FleetRun::runJob(FleetApi& api, const Job& j) {
    const size_t i = static_cast<size_t>(j.index);
    switch (j.kind) {
        case Job::Kind::Open:
            try {
                const FleetSessionInfo info = api.openFleetSession(j.cause);
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    sessions_[i] = info;
                    sessionOpen_[i] = 1;
                }
                stats_.sessionsOpened.fetch_add(1, std::memory_order_relaxed);
                int prev = stats_.maxPlayersPerSession.load();
                while (info.maxPlayers > prev &&
                       !stats_.maxPlayersPerSession.compare_exchange_weak(prev, info.maxPlayers)) {
                }
                workers_[static_cast<size_t>(world_->workerOf(j.index))]->openSession(j.index, info);
            } catch (const std::exception& e) {
                setFatal(std::string("openFleetSession failed: ") + e.what());
            }
            opened_.fetch_add(1, std::memory_order_relaxed);
            break;

        case Job::Kind::Mint:
            try {
                const PlayerToken t = api.mintManagedPlayerToken(j.index, j.cause);
                storeToken(j.index, t);
                stats_.playersSignedIn.fetch_add(1, std::memory_order_relaxed);
                admit(j.index);
            } catch (const std::exception& e) {
                stats_.signInFailures.fetch_add(1, std::memory_order_relaxed);
                setFatal(std::string("mintManagedPlayerToken failed for player ") +
                         std::to_string(j.index) + ": " + e.what());
            }
            tokensReady_.fetch_add(1, std::memory_order_relaxed);
            break;

        case Job::Kind::RefreshToken: {
            // The player's own app token is the bearer. If the API will not
            // refresh it any more, the player signs in again.
            SharedCreds& creds = world_->creds[i];
            PlayerShared& shared = world_->shared[i];
            bool done = false;
            shared.refreshStartedAtNs.store(nowNs(), std::memory_order_relaxed);
            try {
                const PlayerToken t = api.refreshAppToken(creds.snapshot().token, j.cause);
                storeToken(j.index, t, /*fromRefresh*/ true);
                stats_.tokenRefreshes.fetch_add(1, std::memory_order_relaxed);
                done = true;
            } catch (const std::exception&) {
                stats_.tokenRefreshFailures.fetch_add(1, std::memory_order_relaxed);
            }
            shared.refreshStartedAtNs.store(0, std::memory_order_relaxed);
            if (!done && !stop_.load()) {
                try {
                    const PlayerToken t =
                        api.mintManagedPlayerToken(j.index, ApiCause::SignInAgain);
                    storeToken(j.index, t);
                    stats_.signedInAgain.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::exception&) {
                    stats_.signInAgainFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            creds.busy.store(false);
            break;
        }

        case Job::Kind::SignInAgain: {
            SharedCreds& creds = world_->creds[i];
            try {
                const PlayerToken t = api.mintManagedPlayerToken(j.index, j.cause);
                storeToken(j.index, t);
                stats_.signedInAgain.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception&) {
                stats_.signInAgainFailures.fetch_add(1, std::memory_order_relaxed);
            }
            creds.busy.store(false);
            break;
        }

        case Job::Kind::MoveSession:
        case Job::Kind::KeepSession: {
            FleetWorker& worker = *workers_[static_cast<size_t>(world_->workerOf(j.index))];
            try {
                FleetSessionInfo info = api.refreshFleetSession(j.gameTokenId, j.current, j.cause);
                bool moved = true;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    info.maxPlayers = sessions_[i].maxPlayers;
                    moved = !(sessions_[i].server == info.server);
                    sessions_[i] = info;
                }
                if (j.kind == Job::Kind::KeepSession) {
                    stats_.sessionKeepRefreshes.fetch_add(1, std::memory_order_relaxed);
                    // Kept where it is: nothing for the worker to do.
                    if (moved) worker.sessionMoved(j.index, info, true);
                } else {
                    worker.sessionMoved(j.index, info, true);
                }
            } catch (const std::exception& e) {
                if (log_) {
                    std::fprintf(log_, "[fleet] session %d: refreshFleetSession failed: %s\n",
                                 j.index, e.what());
                }
                if (j.kind == Job::Kind::MoveSession) worker.sessionMoved(j.index, {}, false);
            }
            break;
        }

        case Job::Kind::Close:
            try {
                if (api.closeFleetSession(j.gameTokenId, j.cause)) {
                    stats_.sessionsClosed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats_.sessionCloseFailures.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception& e) {
                stats_.sessionCloseFailures.fetch_add(1, std::memory_order_relaxed);
                if (log_) {
                    std::fprintf(log_, "[fleet] session %d: closeFleetSession failed: %s\n",
                                 j.index, e.what());
                }
            }
            closeDone_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void FleetRun::onApiRequest(const ApiRequest& r) {
    // Called on a realtime worker thread. The worker never calls the API: it
    // hands the request to the API threads.
    Job j;
    j.index = r.index;
    if (r.kind == ApiRequest::Kind::MoveSession) {
        j.kind = Job::Kind::MoveSession;
        j.current = r.current;
        j.gameTokenId = r.gameTokenId;
        j.cause = ApiCause::SessionMove;
    } else {
        j.kind = Job::Kind::SignInAgain;
        j.cause = ApiCause::SignInAgain;
    }
    // A request raised while handling a crossing is counted as one, so it shows.
    if (r.fromCrossingPath) j.cause = ApiCause::Crossing;
    push(std::move(j));
}

// ---------------------------------------------------------------------------
// Housekeeping and progress
// ---------------------------------------------------------------------------

void FleetRun::housekeeping() {
    const int64_t now = nowNs();
    if (now - lastHousekeepingNs_ < SEC) return;
    lastHousekeepingNs_ = now;
    const int64_t nowMs = epochMs();

    // A player's app token lasts 30 minutes: refresh it before it expires. The
    // worker that carries the player sees the new token and carries again.
    const int64_t lead = static_cast<int64_t>(cfg_.tokenRefreshLeadS) * 1000;
    for (int n = 0; n < cfg_.players; ++n) {
        SharedCreds& creds = world_->creds[static_cast<size_t>(n)];
        const uint32_t version = creds.version();
        if (version == 0) continue;
        if (creds.expiresAtMs() - nowMs > lead) continue;
        // After an attempt that did not bring a new token, wait before the next.
        if (version == refreshTriedVersion_[static_cast<size_t>(n)] &&
            nowMs < refreshNotBeforeMs_[static_cast<size_t>(n)])
            continue;
        if (creds.busy.exchange(true)) continue;
        refreshTriedVersion_[static_cast<size_t>(n)] = version;
        refreshNotBeforeMs_[static_cast<size_t>(n)] = nowMs + 10000;
        Job j;
        j.kind = Job::Kind::RefreshToken;
        j.index = n;
        j.cause = ApiCause::TokenRefresh;
        push(std::move(j));
    }

    // A fleet session lasts 24 hours: keep it, on the server it is on.
    std::lock_guard<std::mutex> lock(mu_);
    for (int s = 0; s < cfg_.servers; ++s) {
        const size_t i = static_cast<size_t>(s);
        if (!sessionOpen_[i] || sessions_[i].expiresAtMs == 0) continue;
        if (sessions_[i].expiresAtMs - nowMs > 10 * 60 * 1000) continue;
        if (nowMs < keepNotBeforeMs_[i]) continue;
        keepNotBeforeMs_[i] = nowMs + 60000;
        Job j;
        j.kind = Job::Kind::KeepSession;
        j.index = s;
        j.cause = ApiCause::SessionKeep;
        j.current = sessions_[i].server;
        j.gameTokenId = sessions_[i].gameTokenId;
        {
            std::lock_guard<std::mutex> jl(jobMu_);
            if (!jobsClosed_) jobs_.push_back(std::move(j));
        }
        jobCv_.notify_one();
    }
}

int FleetRun::sumPending() const {
    int n = 0;
    for (const auto& w : workers_) n += w->pending();
    return n;
}
int FleetRun::sumCarried() const {
    int n = 0;
    for (const auto& w : workers_) n += w->carried();
    return n;
}
int FleetRun::sumAwaiting() const {
    int n = 0;
    for (const auto& w : workers_) n += w->awaitingRelease();
    return n;
}

void FleetRun::progress(double windowS) {
    if (!log_) return;
    const int64_t now = nowNs();
    const double dt = lastProgressNs_ ? static_cast<double>(now - lastProgressNs_) * 1e-9 : 0.0;
    lastProgressNs_ = now;
    const uint64_t crossings = get(stats_.crossingsCompleted);
    const uint64_t updates = get(stats_.updatesSent);
    const uint64_t notifications = notificationsReceived(stats_);
    const double crossPs = dt > 0 ? static_cast<double>(crossings - lastCrossings_) / dt : 0.0;
    const double updPs = dt > 0 ? static_cast<double>(updates - lastUpdates_) / dt : 0.0;
    const double notifPs = dt > 0 ? static_cast<double>(notifications - lastNotifications_) / dt : 0.0;
    lastCrossings_ = crossings;
    lastUpdates_ = updates;
    lastNotifications_ = notifications;

    const uint64_t busy = sum(stats_.api.platformBusy);
    const uint64_t limited = sum(stats_.api.rateLimited);
    if (windowS < 0) {
        std::fprintf(log_,
                     "[fleet] setup: sessions=%" PRIu64 "/%d signed_in=%" PRIu64 " roster=%" PRIu64
                     " of %d carried=%d | busy=%" PRIu64 " rate_limited=%" PRIu64 "\n",
                     get(stats_.sessionsOpened), cfg_.servers, get(stats_.playersSignedIn),
                     get(stats_.playersFromRoster), cfg_.players, sumCarried(), busy, limited);
    } else {
        std::fprintf(
            log_,
            "[fleet] t=%.0fs sessions=%d players=%d/%d | upd=%.0f/s notif=%.0f/s | "
            "cross=%.1f/s (target %.1f) speed=%.3f | carry p50=%.1fms p99=%.1fms | "
            "released ok=%" PRIu64 " missing=%" PRIu64 " r2=%" PRIu64 " r5=%" PRIu64
            " | refused=%" PRIu64 " own_state=%" PRIu64
            " | api busy=%" PRIu64 " limited=%" PRIu64 " crossing=%" PRIu64 " | lag p99=%.2fms\n",
            // A player in transit between two workers can be in both gauges for an instant.
            windowS, cfg_.servers, std::min(sumCarried(), cfg_.players), cfg_.players, updPs,
            notifPs, crossPs,
            cfg_.crossRate, world_->speed.load(), stats_.carryLatency.percentileMs(0.50),
            stats_.carryLatency.percentileMs(0.99), get(stats_.releaseReceived),
            get(stats_.releaseMissing), get(stats_.releasedByReason[2]),
            get(stats_.releasedByReason[5]), get(stats_.playerMessagesRefused),
            get(stats_.ownStateReceived), busy, limited,
            stats_.api.byCause(ApiCause::Crossing) + get(stats_.api.requestsFromCrossingPath),
            stats_.sendLag.percentileMs(0.99));
    }
    std::fflush(log_);
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

int FleetRun::run() {
    startNs_ = nowNs();
    world_ = std::make_unique<World>(cfg_, stats_, cfg_.threads);
    const Grid& grid = world_->grid;
    sessions_.assign(static_cast<size_t>(cfg_.servers), FleetSessionInfo{});
    sessionOpen_.assign(static_cast<size_t>(cfg_.servers), 0);
    refreshNotBeforeMs_.assign(static_cast<size_t>(cfg_.players), 0);
    refreshTriedVersion_.assign(static_cast<size_t>(cfg_.players), 0);
    keepNotBeforeMs_.assign(static_cast<size_t>(cfg_.servers), 0);

    // The speed that should give the target crossing rate; the controller trims it.
    // A player never moves more than half a region between two updates.
    const double wanted = speedForCrossRate(grid, cfg_.players, cfg_.crossRate);
    const double ceiling = grid.regionChunks * cfg_.updateHz * 0.5;
    startSpeed_ = std::min(wanted, ceiling);
    world_->speed.store(startSpeed_);
    controller_ = std::make_unique<CrossRateController>(
        cfg_.crossRate, startSpeed_, startSpeed_ / 8.0, std::min(startSpeed_ * 8.0, ceiling));
    if (log_) {
        std::fprintf(log_,
                     "[fleet] %d servers on a %d x %d grid of %d-chunk regions, %d players, "
                     "%.1f updates/s each, target %.1f crossings/s (start speed %.3f chunks/s)%s\n",
                     cfg_.servers, grid.cols, grid.rows, grid.regionChunks, cfg_.players,
                     cfg_.updateHz, cfg_.crossRate, startSpeed_,
                     wanted > ceiling ? ", capped: the target needs a faster walk than half a "
                                        "region an update"
                                      : "");
    }

    // The roster first: a refused roster should stop the run before anything is opened.
    bool setupOk = true;
    if (!cfg_.rosterFile.empty()) {
        std::string error;
        const RosterLoad r =
            loadRoster(cfg_.rosterFile, cfg_.apiOrigin(), cfg_.appId, roster_, error);
        if (r == RosterLoad::Refused) {
            setFatal(error);
            setupOk = false;
        } else if (log_) {
            std::fprintf(log_, "[fleet] roster: %zu player token(s) in %s\n",
                         roster_.players.size(), cfg_.rosterFile.c_str());
        }
    }

    for (int i = 0; i < world_->workers; ++i) {
        workers_.push_back(std::make_unique<FleetWorker>(
            i, *world_, deps_.connect, [this](const ApiRequest& r) { onApiRequest(r); }));
    }
    for (auto& w : workers_) w->setPeers(&workers_);
    for (auto& w : workers_) w->start();
    for (int i = 0; i < cfg_.apiThreads; ++i) {
        apiThreads_.emplace_back([this] { apiThread(); });
    }

    auto alive = [this] { return !stop_.load() && !hasFatal_.load(); };
    auto setupProgress = [this] {
        if (nowNs() - lastProgressNs_ >= static_cast<int64_t>(cfg_.statsIntervalS) * SEC)
            progress(-1.0);
    };
    lastProgressNs_ = nowNs();

    // 1. One fleet session for each simulated game server.
    if (setupOk) {
        for (int s = 0; s < cfg_.servers; ++s) {
            Job j;
            j.kind = Job::Kind::Open;
            j.index = s;
            j.cause = ApiCause::Setup;
            push(std::move(j));
        }
        while (opened_.load() < cfg_.servers && !stop_.load()) {
            setupProgress();
            sleepMs(5);
        }
        setupOk = alive() && opened_.load() == cfg_.servers;
        const int perSession = stats_.maxPlayersPerSession.load();
        if (setupOk && log_ && perSession > 0 &&
            static_cast<double>(cfg_.players) / cfg_.servers > 0.6 * perSession) {
            std::fprintf(log_,
                         "[fleet] warning: about %d players a server against %d a session may "
                         "carry. Players are not spread evenly, so some carries will be refused "
                         "(status 4)\n",
                         cfg_.players / cfg_.servers, perSession);
        }
    }

    // 2. Every player signs in once (or is taken from the roster) and joins.
    if (setupOk) {
        const int64_t nowMs = epochMs();
        const int64_t margin = (static_cast<int64_t>(cfg_.tokenRefreshLeadS) + 60) * 1000;
        std::vector<int> toMint;
        for (int n = 0; n < cfg_.players; ++n) {
            auto it = roster_.players.find(n);
            if (it != roster_.players.end() && it->second.expiresAtMs - nowMs > margin) {
                world_->shared[static_cast<size_t>(n)].tokenStoredAtNs.store(
                    nowNs(), std::memory_order_relaxed);
                world_->creds[static_cast<size_t>(n)].store(it->second);
                stats_.playersFromRoster.fetch_add(1, std::memory_order_relaxed);
                tokensReady_.fetch_add(1, std::memory_order_relaxed);
                admit(n);
            } else {
                toMint.push_back(n);
            }
        }
        const double gapNs = 1e9 / cfg_.signinRate;
        double due = static_cast<double>(nowNs());
        size_t next = 0;
        while (next < toMint.size() && alive()) {
            const double now = static_cast<double>(nowNs());
            while (next < toMint.size() && now >= due) {
                Job j;
                j.kind = Job::Kind::Mint;
                j.index = toMint[next++];
                j.cause = ApiCause::Setup;
                push(std::move(j));
                due += gapNs;
            }
            housekeeping();
            setupProgress();
            sleepMs(1);
        }
        while (tokensReady_.load() < cfg_.players && alive()) {
            housekeeping();
            setupProgress();
            sleepMs(5);
        }
        setupOk = alive() && tokensReady_.load() == cfg_.players;

        // The measured window opens when everyone is carried. Give stragglers
        // the time a carry may take, then open it and let the summary say so.
        const int64_t waitFrom = nowNs();
        const int64_t patience =
            static_cast<int64_t>(cfg_.sessionSettleMs + 2 * cfg_.carryGiveUpMs + 2000) * MS;
        while (setupOk && alive() &&
               sumCarried() + static_cast<int>(get(stats_.playersFailed)) < cfg_.players &&
               nowNs() - waitFrom < patience) {
            housekeeping();
            sleepMs(10);
        }
        if (setupOk && !cfg_.rosterFile.empty()) {
            std::string error;
            Roster copy;
            {
                std::lock_guard<std::mutex> lock(mu_);
                copy = roster_;
            }
            copy.api = cfg_.apiOrigin();
            copy.appId = cfg_.appId;
            if (!saveRoster(cfg_.rosterFile, copy, error) && log_)
                std::fprintf(log_, "[fleet] warning: %s\n", error.c_str());
        }
    }

    // 3. The measured window.
    bool ranWindow = false;
    if (setupOk) {
        ranWindow = true;
        windowStartNs_ = nowNs();
        world_->windowStartNs.store(windowStartNs_, std::memory_order_relaxed);
        stats_.api.windowOpen.store(true);
        crossingsAtWindowStart_ = get(stats_.crossingsCompleted);
        updatesAtWindowStart_ = get(stats_.updatesSent);
        notificationsAtWindowStart_ = notificationsReceived(stats_);
        lastProgressNs_ = windowStartNs_;
        lastCrossings_ = crossingsAtWindowStart_;
        lastUpdates_ = updatesAtWindowStart_;
        lastNotifications_ = notificationsAtWindowStart_;
        const double measureS = cfg_.dryRun ? cfg_.dryRunS : static_cast<double>(cfg_.durationS);
        // How often the controller looks: long enough to count a few hundred crossings.
        const double controlS = measureS > 0 ? std::clamp(measureS / 10.0, 1.0, 5.0) : 5.0;
        int64_t lastControlNs = windowStartNs_;
        uint64_t lastControlCrossings = crossingsAtWindowStart_;
        if (log_) {
            std::fprintf(log_, "[fleet] every player has joined (%d carried of %d): measuring%s\n",
                         sumCarried(), cfg_.players,
                         measureS > 0 ? "" : " until interrupted");
        }
        while (alive()) {
            const int64_t now = nowNs();
            const double elapsed = static_cast<double>(now - windowStartNs_) * 1e-9;
            if (measureS > 0 && elapsed >= measureS) break;
            if (deps_.onTick) deps_.onTick(elapsed);
            housekeeping();
            peakCarried_ = std::max(peakCarried_, std::min(sumCarried(), cfg_.players));
            if (static_cast<double>(now - lastControlNs) * 1e-9 >= controlS) {
                const uint64_t c = get(stats_.crossingsCompleted);
                const double dt = static_cast<double>(now - lastControlNs) * 1e-9;
                world_->speed.store(controller_->update(c - lastControlCrossings, dt));
                lastControlNs = now;
                lastControlCrossings = c;
            }
            if (now - lastProgressNs_ >= static_cast<int64_t>(cfg_.statsIntervalS) * SEC)
                progress(elapsed);
            sleepMs(20);
        }
        windowEndNs_ = nowNs();
        stats_.api.windowOpen.store(false);
        crossingsAtWindowEnd_ = get(stats_.crossingsCompleted);
        updatesAtWindowEnd_ = get(stats_.updatesSent);
        notificationsAtWindowEnd_ = notificationsReceived(stats_);
    }
    interrupted_ = stop_.load();

    // 4. Stop walking, and wait for the carries and releases still on their way.
    world_->moving.store(false);
    if (ranWindow) {
        const int64_t drainFrom = nowNs();
        sleepMs(250); // the workers refresh their gauges every 100 ms
        while (nowNs() - drainFrom < static_cast<int64_t>(cfg_.drainS) * SEC &&
               (sumPending() > 0 || sumAwaiting() > 0)) {
            housekeeping(); // a token that falls due now is still refreshed
            sleepMs(20);
        }
    }

    // 5. The players leave the game: FLEET_RELEASE for each one carried.
    for (auto& w : workers_) w->releaseAll();
    sleepMs(ranWindow ? 200 : 20);
    for (auto& w : workers_) w->stop();
    end_ = FleetWorker::End{};
    for (auto& w : workers_) {
        const FleetWorker::End e = w->end();
        end_.players += e.players;
        end_.carried += e.carried;
        end_.notCarried += e.notCarried;
        end_.failed += e.failed;
        end_.crossingsOpen += e.crossingsOpen;
        end_.releasesPending += e.releasesPending;
        const auto use = w->usage();
        usage_.insert(usage_.end(), use.begin(), use.end());
    }
    peakCarried_ = std::max(peakCarried_, end_.carried);

    // 6. Close every session, also after an interrupt. First let go of the setup
    // work that has not started and wait for what is in flight, so a session
    // that is being opened right now is closed too.
    {
        std::lock_guard<std::mutex> lock(jobMu_);
        jobs_.clear();
    }
    const int64_t settleFrom = nowNs();
    while (jobsInFlight_.load() > 0 && nowNs() - settleFrom < 90 * SEC) sleepMs(5);
    int toClose = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (int s = 0; s < cfg_.servers; ++s) {
            if (!sessionOpen_[static_cast<size_t>(s)]) continue;
            Job j;
            j.kind = Job::Kind::Close;
            j.index = s;
            j.cause = ApiCause::Teardown;
            j.gameTokenId = sessions_[static_cast<size_t>(s)].gameTokenId;
            {
                std::lock_guard<std::mutex> jl(jobMu_);
                jobs_.push_back(std::move(j));
            }
            ++toClose;
        }
    }
    jobCv_.notify_all();
    const int64_t closeFrom = nowNs();
    while (closeDone_.load() < toClose && nowNs() - closeFrom < 180 * SEC) sleepMs(5);
    {
        std::lock_guard<std::mutex> lock(jobMu_);
        jobsClosed_ = true;
        jobs_.clear();
    }
    jobCv_.notify_all();
    for (auto& t : apiThreads_) {
        if (t.joinable()) t.join();
    }

    if (!cfg_.rosterFile.empty() && setupOk) {
        // Tokens were refreshed during the run: keep the newest for the next one.
        std::string error;
        roster_.api = cfg_.apiOrigin();
        roster_.appId = cfg_.appId;
        if (!saveRoster(cfg_.rosterFile, roster_, error) && log_)
            std::fprintf(log_, "[fleet] warning: %s\n", error.c_str());
    }

    // 7. The pass conditions.
    std::vector<std::string> failures;
    int exitCode = 0;
    if (hasFatal_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        failures.push_back("setup failed: " + fatal_);
        exitCode = 2;
    } else if (!setupOk) {
        failures.push_back("the run was interrupted before every player had joined");
    }
    if (ranWindow) {
        if (end_.notCarried > 0) {
            failures.push_back(std::to_string(end_.notCarried) +
                               " player(s) were not carried at the end: a carry did not end "
                               "carried and was not retried to carried");
        }
        if (end_.failed > 0) {
            failures.push_back(std::to_string(end_.failed) + " player(s) were refused for good");
        }
        if (end_.crossingsOpen > 0) {
            failures.push_back(std::to_string(end_.crossingsOpen) +
                               " crossing(s) lost a player: the new server never carried them");
        }
        const int64_t unaccounted =
            static_cast<int64_t>(get(stats_.crossingsAttempted)) -
            static_cast<int64_t>(get(stats_.crossingsCompleted)) -
            static_cast<int64_t>(get(stats_.crossingsSuperseded)) - end_.crossingsOpen;
        if (unaccounted != 0) {
            failures.push_back(std::to_string(unaccounted) +
                               " crossing(s) are unaccounted for (attempted is not completed "
                               "plus superseded plus open)");
        }
        if (!cfg_.ownState && get(stats_.ownStateReceived) > 0) {
            // Each update names the session that sent it: these came back to the
            // session that sent them. An update another session sent for a player
            // the receiver carries now is counted apart and does not fail a run.
            failures.push_back(
                std::to_string(get(stats_.ownStateReceived)) +
                " update(s) came back to the session that sent them with FLEET_OWN_STATE off (" +
                std::to_string(get(stats_.ownSentWhileCarrying)) +
                " while it still carried the player, " +
                std::to_string(get(stats_.ownSentAfterPlayerLeft)) +
                " after the player had left it)");
        }
        // Refusals. Each class is described in the summary; two of them say the
        // client and the realtime servers disagree about a carry, and fail a run.
        if (get(stats_.unauthUnexplained) > 0) {
            failures.push_back(std::to_string(get(stats_.unauthUnexplained)) +
                               " message(s) were refused UNAUTHORIZED with no explanation: the "
                               "session carried the player under that token id when it sent them");
        }
        if (get(stats_.unauthSessionMessages) > 0) {
            failures.push_back(std::to_string(get(stats_.unauthSessionMessages)) +
                               " of a session's own messages (29, 49, 50 or 26) were refused "
                               "UNAUTHORIZED");
        }
    }
    // The assertion the proof rests on: no API call was made because of a crossing.
    const uint64_t crossingCalls =
        stats_.api.byCause(ApiCause::Crossing) + get(stats_.api.requestsFromCrossingPath);
    if (crossingCalls != 0) {
        failures.push_back(std::to_string(crossingCalls) +
                           " Application API call(s) were made because of a crossing");
    }
    // And its wider form: while the players crossed, the API was called for
    // nothing but the refreshes that fell due (a player's app token, a session's
    // 24 hours). Anything else is named, so the reader can see what it was.
    {
        std::string what;
        uint64_t other = 0;
        for (int c = 0; c < ApiCounters::C; ++c) {
            const ApiCause cause = static_cast<ApiCause>(c);
            if (cause == ApiCause::TokenRefresh || cause == ApiCause::SessionKeep) continue;
            const uint64_t n = get(stats_.api.duringWindowByCause[static_cast<size_t>(c)]);
            if (!n) continue;
            other += n;
            what += (what.empty() ? "" : ", ") + std::to_string(n) + " for " + apiCauseName(cause);
        }
        if (other != 0) {
            failures.push_back(std::to_string(other) +
                               " Application API call(s) in the measured phase were not "
                               "refreshes that fell due: " + what);
        }
    }
    if (log_ && ranWindow) {
        const uint64_t inPhase = sum(stats_.api.duringWindow);
        const uint64_t due =
            get(stats_.api.duringWindowByCause[static_cast<size_t>(ApiCause::TokenRefresh)]) +
            get(stats_.api.duringWindowByCause[static_cast<size_t>(ApiCause::SessionKeep)]);
        std::fprintf(log_,
                     "[fleet] Application API calls in the measured phase: %" PRIu64
                     " (refreshes that fell due: %" PRIu64 ", anything else: %" PRIu64
                     ", because of a crossing: %" PRIu64 ") over %" PRIu64 " crossings\n",
                     inPhase, due, inPhase - due, crossingCalls,
                     crossingsAtWindowEnd_ - crossingsAtWindowStart_);
    }
    if (get(stats_.plainCommunications) > 0) {
        failures.push_back(std::to_string(get(stats_.plainCommunications)) +
                           " communication(s) arrived outside a FLEET_DELIVERY: a server defect");
    }
    if (exitCode == 0 && !failures.empty()) exitCode = 1;
    // An interrupt ends a run that was asked to go on until interrupted, and that
    // run is judged like any other. Any other interrupted run did not finish.
    const bool untilInterrupted = !cfg_.dryRun && cfg_.durationS == 0 && ranWindow;
    if (interrupted_ && exitCode != 2 && !untilInterrupted) exitCode = 130;

    buildSummary(exitCode, failures);
    return exitCode;
}

// ---------------------------------------------------------------------------
// The summary
// ---------------------------------------------------------------------------

nlohmann::json FleetRun::carriesByStatus() const {
    // FLEET_CARRY_RESULT by status, under the names the contract gives them.
    uint64_t other = 0;
    for (size_t s = 5; s < 256; ++s) other += get(stats_.carryRefused[s]);
    return {{carryStatusName(0), sum(stats_.carryCarried)},
            {carryStatusName(1), get(stats_.carryRefused[1])},
            {carryStatusName(2), get(stats_.carryRefused[2])},
            {carryStatusName(3), get(stats_.carryRefused[3])},
            {carryStatusName(4), get(stats_.carryRefused[4])},
            {"other", other}};
}

nlohmann::json FleetRun::releasesByReason() const {
    // FLEET_RELEASED by reason, under the names the contract gives them.
    uint64_t other = 0;
    for (size_t r = 0; r < 256; ++r) {
        if (r != 1 && r != 2 && r != 5) other += get(stats_.releasedByReason[r]);
    }
    return {{releaseReasonName(1), get(stats_.releasedByReason[1])},
            {releaseReasonName(2), get(stats_.releasedByReason[2])},
            {releaseReasonName(5), get(stats_.releasedByReason[5])},
            {"other", other}};
}

void FleetRun::buildSummary(int exitCode, const std::vector<std::string>& failures) {
    const FleetStats& st = stats_;
    const Grid& grid = world_->grid;
    const double measuredS =
        windowEndNs_ > windowStartNs_ ? static_cast<double>(windowEndNs_ - windowStartNs_) * 1e-9 : 0.0;
    const double totalS = static_cast<double>(nowNs() - startNs_) * 1e-9;
    const double setupS =
        windowStartNs_ ? static_cast<double>(windowStartNs_ - startNs_) * 1e-9 : totalS;
    auto rate = [measuredS](uint64_t a, uint64_t b) {
        return measuredS > 0 ? static_cast<double>(a - b) / measuredS : 0.0;
    };

    nlohmann::json j;
    j["tool"] = "ck-fleet-load";
    j["mode"] = deps_.mode;
    j["pass"] = exitCode == 0;
    j["exit_code"] = exitCode;
    j["interrupted"] = interrupted_;
    j["fail_reasons"] = failures;

    j["config"] = {
        {"api", cfg_.dryRun ? std::string("dry-run") : cfg_.apiOrigin()},
        {"app_id", std::to_string(cfg_.appId)},
        {"servers", cfg_.servers},
        {"players", cfg_.players},
        {"duration_s", cfg_.dryRun ? cfg_.dryRunS : static_cast<double>(cfg_.durationS)},
        {"update_hz", cfg_.updateHz},
        {"cross_rate", cfg_.crossRate},
        {"signin_rate", cfg_.signinRate},
        {"region_chunks", grid.regionChunks},
        {"grid_cols", grid.cols},
        {"grid_rows", grid.rows},
        {"distance", cfg_.distance},
        {"decay", cfg_.decay},
        {"seed", cfg_.seed},
        {"own_state", cfg_.ownState},
        {"threads", world_->workers},
        {"api_threads", cfg_.apiThreads},
        {"latency_sample", cfg_.latencySample},
    };
    j["timing"] = {{"setup_s", setupS}, {"measured_s", measuredS}, {"total_s", totalS}};

    // Sessions.
    nlohmann::json moves = nlohmann::json::object();
    for (int t = 0; t < static_cast<int>(MoveTrigger::COUNT); ++t)
        moves[moveTriggerName(static_cast<MoveTrigger>(t))] = get(st.sessionMoves[static_cast<size_t>(t)]);
    j["sessions"] = {
        {"opened", get(st.sessionsOpened)},
        {"closed", get(st.sessionsClosed)},
        {"close_failures", get(st.sessionCloseFailures)},
        {"max_players", st.maxPlayersPerSession.load()},
        {"moves", sum(st.sessionMoves)},
        {"moves_by_trigger", moves},
        {"moves_completed", get(st.sessionMovesCompleted)},
        {"came_back_to_the_same_server", get(st.sessionReturnsSameServer)},
        {"came_back_to_another_server", get(st.sessionReturnsOtherServer)},
        {"bound_control_acknowledged", get(st.boundAcks)},
        {"legacy_move_commands_dropped", get(st.legacyControlDropped)},
        {"move_failures", get(st.sessionMoveFailures)},
        {"keep_refreshes", get(st.sessionKeepRefreshes)},
        {"reconnect_commands", get(st.reconnectCommands)},
        {"session_released_by_reason", nonZero(st.sessionReleasedByReason)},
    };

    // Players.
    const size_t mintK = static_cast<size_t>(ApiKind::MintManagedPlayerToken);
    j["players"] = {
        {"total", cfg_.players},
        {"signed_in", get(st.playersSignedIn)},
        {"from_roster", get(st.playersFromRoster)},
        {"joined", get(st.playersAdmitted)},
        {"platform_busy", get(st.api.platformBusy[mintK])},
        {"rate_limited", get(st.api.rateLimited[mintK])},
        {"sign_in_failures", get(st.signInFailures)},
        {"signed_in_again", get(st.signedInAgain)},
        {"sign_in_again_failures", get(st.signInAgainFailures)},
        {"token_refreshes", get(st.tokenRefreshes)},
        {"token_refresh_failures", get(st.tokenRefreshFailures)},
        {"carried_at_end", end_.carried},
        {"not_carried_at_end", end_.notCarried},
        {"refused_for_good", end_.failed},
    };

    // Carries.
    nlohmann::json attempts = nlohmann::json::object(), carriedBy = nlohmann::json::object();
    for (int c = 0; c < static_cast<int>(CarryCause::COUNT); ++c) {
        attempts[carryCauseName(static_cast<CarryCause>(c))] = get(st.carryAttempts[static_cast<size_t>(c)]);
        carriedBy[carryCauseName(static_cast<CarryCause>(c))] = get(st.carryCarried[static_cast<size_t>(c)]);
    }
    j["carries"] = {
        {"attempts", sum(st.carryAttempts)},
        {"attempts_by_cause", attempts},
        {"sent", get(st.carryDatagrams)},
        {"carried", sum(st.carryCarried)},
        {"carried_by_cause", carriedBy},
        {"by_status", carriesByStatus()},
        {"resent", get(st.carryResends)},
        {"waited_for_the_carry_rate", get(st.carryPacedWaits)},
        {"gave_up_after_5s", get(st.carryTimedOut)},
        {"started_again_after_giving_up", get(st.carryRetries)},
        {"abandoned_player_moved_on", get(st.carryAbandoned)},
        {"results_duplicate", get(st.carryResultsDuplicate)},
        {"results_late", get(st.carryResultsLate)},
        {"took_a_newer_token_before_sending", get(st.carryTookNewerToken)},
        {"retried_after_refresh", get(st.carryRetriedAfterRefresh)},
        {"retried_after_refresh_at_once_with_the_newer_token", get(st.carryRetriedWithNewerToken)},
        {"retried_after_refresh_after_a_wait", get(st.carryRetriedAfterWait)},
        {"status_1_not_explained_by_a_refresh", get(st.carryStatus1Refusals)},
        {"token_carry_sent_after_the_refresh_answer", st.tokenCarryDelay.toJson()},
        {"unresolved_at_end", end_.notCarried + end_.failed},
        {"latency", st.carryLatency.toJson()},
    };

    // Crossings.
    const int64_t unaccounted = static_cast<int64_t>(get(st.crossingsAttempted)) -
                                static_cast<int64_t>(get(st.crossingsCompleted)) -
                                static_cast<int64_t>(get(st.crossingsSuperseded)) -
                                end_.crossingsOpen;
    j["crossings"] = {
        {"attempted", get(st.crossingsAttempted)},
        {"completed", get(st.crossingsCompleted)},
        {"superseded", get(st.crossingsSuperseded)},
        {"lost", end_.crossingsOpen},
        {"unaccounted", unaccounted},
        {"latency", st.crossingLatency.toJson()},
        {"target_per_s", cfg_.crossRate},
        {"achieved_per_s", rate(crossingsAtWindowEnd_, crossingsAtWindowStart_)},
        {"speed_chunks_per_s", world_->speed.load()},
        {"start_speed_chunks_per_s", startSpeed_},
        {"api_calls", st.api.byCause(ApiCause::Crossing) + get(st.api.requestsFromCrossingPath)},
    };

    // FLEET_RELEASED.
    j["released"] = {
        {"by_reason", releasesByReason()},
        {"old_session",
         {{"expected", get(st.releaseExpected)},
          {"received", get(st.releaseReceived)},
          {"delay_after_new_result", st.releaseDelay.toJson()},
          {"before_new_result", get(st.releaseBeforeResult)},
          {"named_the_token_id_of_a_carry_left_unanswered", get(st.releaseNamedNewerToken)},
          {"never_arrived_within_5s", get(st.releaseMissing)},
          {"no_new_result_to_pair", get(st.releaseUnpaired)},
          {"pending_at_end", end_.releasesPending}}},
        {"reason_1_for_a_held_player", get(st.releasedUnexpected)},
        {"reason_1_after_a_late_carry", get(st.releasedAfterLateCarry)},
        {"reason_1_after_the_sessions_own_release", get(st.releasedAfterOwnRelease)},
        {"reason_2_for_a_held_player", get(st.released2Held)},
        {"reason_5_for_a_held_player", get(st.released5Held)},
        {"other_reason_for_a_held_player", get(st.releasedOtherHeld)},
        {"carried_again", get(st.recarriedAfterRelease)},
        {"sign_in_again_requests", get(st.resignRequests)},
        {"signed_in_again", get(st.signedInAgain)},
        {"for_a_player_not_held", get(st.releasedUnknownPlayer)},
    };

    // Refusals.
    nlohmann::json codes = nlohmann::json::array();
    for (int c = 0; c < 256; ++c) {
        const uint64_t n = get(st.errorCodes[static_cast<size_t>(c)]);
        if (n) codes.push_back({{"code", c}, {"name", errorCodeName(c)}, {"n", n}});
    }
    j["refused"] = {
        {"errors_by_code", codes},
        {"unauthorized", get(st.errorCodes[7])},
        {"player_messages_refused", get(st.playerMessagesRefused)},
        {"unauthorized_during_token_refresh", get(st.unauthTokenRefresh)},
        {"unauthorized_during_token_refresh_before_the_new_carry_result",
         get(st.unauthTokenRefreshBeforeResult)},
        {"unauthorized_during_token_refresh_after_the_new_carry_result",
         get(st.unauthTokenRefreshAfterResult)},
        {"unauthorized_after_handoff", get(st.unauthAfterHandoff)},
        {"unauthorized_after_the_carry_ended", get(st.unauthAfterCarryEnded)},
        {"unauthorized_first_contact", get(st.unauthFirstContact)},
        {"unauthorized_window_reload", get(st.unauthWindowReload)},
        {"unauthorized_unexplained", get(st.unauthUnexplained)},
        {"unauthorized_session_messages", get(st.unauthSessionMessages)},
        {"classes_that_fail_a_run",
         {"unauthorized_unexplained", "unauthorized_session_messages"}},
        {"carried_again_after_unauthorized", get(st.recarriedAfterUnauthorized)},
    };

    // Traffic.
    nlohmann::json byType = nlohmann::json::array();
    for (int t = 0; t < 256; ++t) {
        const uint64_t n = get(st.rxByType[static_cast<size_t>(t)]);
        if (n) byType.push_back({{"type", t}, {"name", messageTypeName(t)}, {"n", n}});
    }
    // Where each message type arrived, and how what arrived outside a signed
    // bundle was authenticated.
    nlohmann::json arrival = nlohmann::json::array();
    nlohmann::json plainMembers = nlohmann::json::array(), unsignedByType = nlohmann::json::array();
    for (int t = 0; t < 256; ++t) {
        const size_t i = static_cast<size_t>(t);
        const uint64_t inSigned = get(st.rxInSignedBundle[i]), inPlain = get(st.rxInPlainBundle[i]),
                       alone = get(st.rxStandalone[i]);
        if (inSigned + inPlain + alone == 0) continue;
        arrival.push_back({{"type", t},
                           {"name", messageTypeName(t)},
                           {"in_signed_bundles", inSigned},
                           {"in_plain_bundles", inPlain},
                           {"alone_in_a_datagram", alone},
                           {"outside_signed_bundles_own_hmac_verified", get(st.rxOutsideOwnHmacOk[i])},
                           {"outside_signed_bundles_own_hmac_failed", get(st.rxOutsideOwnHmacFailed[i])},
                           {"outside_signed_bundles_no_hmac", get(st.rxOutsideNoHmac[i])}});
        if (inPlain) plainMembers.push_back({{"type", t}, {"name", messageTypeName(t)}, {"n", inPlain}});
        if ((t & wire::SPATIAL_TYPE_BIT) && get(st.rxOutsideNoHmac[i])) {
            unsignedByType.push_back(
                {{"type", t}, {"name", messageTypeName(t)}, {"n", get(st.rxOutsideNoHmac[i])}});
        }
    }
    nlohmann::json leftByReason = nlohmann::json::object();
    for (int r = 0; r < 256; ++r) {
        const uint64_t n = get(st.actorLeftByReason[static_cast<size_t>(r)]);
        if (n) leftByReason[std::to_string(r)] = n;
    }
    j["traffic"] = {
        {"updates_sent", get(st.updatesSent)},
        {"updates_per_s", rate(updatesAtWindowEnd_, updatesAtWindowStart_)},
        {"updates_per_s_expected", cfg_.players * cfg_.updateHz},
        {"capabilities_sent", get(st.capabilitiesSent)},
        {"heartbeats_sent", get(st.heartbeatsSent)},
        {"releases_sent", get(st.releasesSent)},
        {"send_errors", get(st.txSendErrors)},
        {"received_by_type", byType},
        {"notifications_received", notificationsReceived(st)},
        {"notifications_per_s", rate(notificationsAtWindowEnd_, notificationsAtWindowStart_)},
        {"deliveries",
         {{"frames", get(st.deliveries)},
          {"players_listed", get(st.deliveryPlayers)},
          {"by_type", nonZero(st.deliveryByType)},
          {"listing_a_player_not_carried", get(st.deliveryForUncarried)}}},
        {"signed_bundles", get(st.rxSignedBundles)},
        {"plain_bundles", get(st.rxPlainBundles)},
        {"plain_bundle_members", get(st.plainBundleMembers)},
        {"plain_bundle_members_by_type", plainMembers},
        {"hmac_failures", get(st.rxHmacFailures)},
        {"malformed", get(st.rxMalformed)},
        {"unsigned_notifications", get(st.rxUnsignedSpatial)},
        {"unsigned_notifications_by_type", unsignedByType},
        {"unsigned_notifications_were",
         {{"an_update_the_receiving_session_sent", get(st.unsignedSentByReceiver)},
          {"an_update_another_session_of_this_run_sent", get(st.unsignedSentByAnother)},
          {"of_those_the_senders_first_update_after_a_carry_result", get(st.unsignedFirstUpdate)},
          {"an_update_with_no_marker_of_this_run", get(st.unsignedNotThisRun)},
          {"not_an_actor_update", get(st.unsignedOtherType)},
          {"from_beyond_the_range_of_the_receivers_region", get(st.unsignedOutOfRange)}}},
        {"updates_from_beyond_the_range_of_the_receivers_region", get(st.updatesOutOfRange)},
        {"first_updates_after_a_carry_result",
         {{"sent", get(st.firstUpdatesSent)},
          {"notifications_received", get(st.firstUpdatesReceived)},
          {"of_those_with_no_hmac_outside_a_signed_bundle", get(st.firstUpdatesReceivedUnsigned)}}},
        {"by_type_and_arrival", arrival},
        {"actor_left",
         {{"by_reason", leftByReason},
          {"its_player_was_carried_then", get(st.actorLeftForCarriedPlayer)},
          {"its_player_was_not_carried_then", get(st.actorLeftForUncarriedPlayer)},
          {"from_beyond_the_range_of_the_receivers_region", get(st.actorLeftOutOfRange)},
          {"not_an_actor_of_this_run", get(st.actorLeftUnknownActor)}}},
        {"actor_updates_not_of_this_run",
         {{"another_payload", get(st.actorUpdatesOtherPayload)},
          {"another_run_of_this_tool", get(st.actorUpdatesOtherRun)}}},
        {"fleet_frames_in_signed_bundles", get(st.fleetFramesInSignedBundle)},
        {"fleet_frames_with_own_hmac", get(st.fleetFramesWithOwnHmac)},
        {"fleet_frame_hmac_failures", get(st.fleetFrameHmacFailures)},
        {"plain_state_notifications", get(st.plainState)},
        {"plain_communications", get(st.plainCommunications)},
    };

    j["replication"] = {
        {"sample_players", std::min(cfg_.latencySample, cfg_.players)},
        {"marked_updates_received", get(st.markersSeen)},
        {"latency", st.replicationLatency.toJson()},
    };
    // Every update names the session that sent it and the player it is for.
    j["own_state"] = {
        {"capability_set", cfg_.ownState},
        {"received", get(st.ownStateReceived)},
        {"sent_by_the_receiving_session",
         {{"total", get(st.ownStateReceived)},
          {"while_it_still_carried_the_player", get(st.ownSentWhileCarrying)},
          {"after_the_player_had_left_it", get(st.ownSentAfterPlayerLeft)},
          {"its_first_update_after_a_carry_result", get(st.ownSentFirstUpdate)},
          {"with_no_hmac_outside_a_signed_bundle", get(st.ownSentUnsigned)},
          {"age_when_received", st.ownSentAge.toJson()},
          {"fails_a_run", !cfg_.ownState}}},
        {"sent_by_another_session_before_the_handoff",
         {{"total", get(st.otherSentBeforeHandoff)},
          {"sent_before_the_receivers_carry_result", st.handoffLead.toJson()},
          {"fails_a_run", false}}},
        {"sent_by_another_session_after_the_receivers_carry_result", get(st.otherSentAfterCarry)},
    };
    j["events"] = st.events.toJson();

    // Bytes.
    nlohmann::json perSession = nlohmann::json::array();
    for (int s = 0; s < cfg_.servers; ++s) {
        const SessionIo& io = *st.sessions[static_cast<size_t>(s)];
        const FleetSessionInfo& info = sessions_[static_cast<size_t>(s)];
        perSession.push_back({
            {"session", s},
            {"server", info.server.ip4.empty()
                           ? std::string()
                           : info.server.ip4 + ":" + std::to_string(info.server.port)},
            {"tx_bytes", get(io.txBytes)},
            {"rx_bytes", get(io.rxBytes)},
            {"tx_datagrams", get(io.txDatagrams)},
            {"rx_datagrams", get(io.rxDatagrams)},
            {"plain_state_notifications", get(io.plainState)},
            {"deliveries", get(io.deliveries)},
            {"delivery_players_listed", get(io.deliveryPlayers)},
            {"plain_communications", get(io.plainCommunications)},
            {"game_token_id", std::to_string(info.gameTokenId)},
            {"players_at_end", io.players.load()},
        });
    }
    j["bytes"] = {
        {"tx", get(st.txBytes)},
        {"rx", get(st.rxBytes)},
        {"tx_datagrams", get(st.txDatagrams)},
        {"rx_datagrams", get(st.rxDatagrams)},
        {"per_session", perSession},
    };

    // The Application API.
    nlohmann::json kinds = nlohmann::json::object(), causes = nlohmann::json::object(),
                   window = nlohmann::json::object(), busy = nlohmann::json::object(),
                   limited = nlohmann::json::object(), failed = nlohmann::json::object();
    for (int k = 0; k < ApiCounters::K; ++k) {
        const char* name = apiKindName(static_cast<ApiKind>(k));
        kinds[name] = st.api.byKind(static_cast<ApiKind>(k));
        window[name] = get(st.api.duringWindow[static_cast<size_t>(k)]);
        busy[name] = get(st.api.platformBusy[static_cast<size_t>(k)]);
        limited[name] = get(st.api.rateLimited[static_cast<size_t>(k)]);
        failed[name] = get(st.api.failures[static_cast<size_t>(k)]);
    }
    nlohmann::json windowCauses = nlohmann::json::object();
    for (int c = 0; c < ApiCounters::C; ++c) {
        causes[apiCauseName(static_cast<ApiCause>(c))] = st.api.byCause(static_cast<ApiCause>(c));
        windowCauses[apiCauseName(static_cast<ApiCause>(c))] =
            get(st.api.duringWindowByCause[static_cast<size_t>(c)]);
    }
    const uint64_t refreshesDue =
        get(st.api.duringWindowByCause[static_cast<size_t>(ApiCause::TokenRefresh)]) +
        get(st.api.duringWindowByCause[static_cast<size_t>(ApiCause::SessionKeep)]);
    j["api"] = {
        {"calls", st.api.total()},
        {"calls_by_kind", kinds},
        {"calls_by_cause", causes},
        {"calls_in_measured_phase", sum(st.api.duringWindow)},
        {"calls_in_measured_phase_by_kind", window},
        {"calls_in_measured_phase_by_cause", windowCauses},
        {"calls_in_measured_phase_refreshes_due", refreshesDue},
        {"calls_in_measured_phase_other", sum(st.api.duringWindow) - refreshesDue},
        {"platform_busy_by_kind", busy},
        {"rate_limited_by_kind", limited},
        {"failures_by_kind", failed},
        {"calls_caused_by_crossing",
         st.api.byCause(ApiCause::Crossing) + get(st.api.requestsFromCrossingPath)},
    };

    j["generator"] = {
        {"send_lag", st.sendLag.toJson()},
        {"updates_skipped", get(st.updatesSkipped)},
    };

    // What each session sent and received in each minute of the wall clock (UTC),
    // to lay beside the tier's usage rows, which are by session game token id and
    // minute. `received_*` is what the server sent the session: the data out the
    // bill prices. Bytes are UDP payload; `*_wire_bytes` adds 42 a datagram (the
    // Ethernet, IPv4 and UDP headers).
    std::sort(usage_.begin(), usage_.end(), [](const auto& a, const auto& b) {
        return a.minute != b.minute ? a.minute < b.minute : a.session < b.session;
    });
    nlohmann::json minutes = nlohmann::json::array();
    for (const auto& u : usage_) {
        minutes.push_back({
            {"session", u.session},
            {"game_token_id", std::to_string(u.gameTokenId)},
            {"minute", minuteIso(u.minute)},
            {"sent_datagrams", u.txDatagrams},
            {"sent_bytes", u.txBytes},
            {"sent_wire_bytes", u.txBytes + kWireOverhead * u.txDatagrams},
            {"received_datagrams", u.rxDatagrams},
            {"received_bytes", u.rxBytes},
            {"received_wire_bytes", u.rxBytes + kWireOverhead * u.rxDatagrams},
            {"carried_player_seconds", std::round(u.carriedPlayerSeconds * 10.0) / 10.0},
        });
    }
    j["usage"] = {
        {"wire_overhead_bytes_per_datagram", kWireOverhead},
        {"per_session_minute", minutes},
    };

    // The headline numbers of the proof, in one place.
    const uint64_t hmacFailures = get(st.rxHmacFailures) + get(st.fleetFrameHmacFailures);
    uint64_t otherErrors = 0;
    for (int c = 0; c < 256; ++c) {
        if (c != 7) otherErrors += get(st.errorCodes[static_cast<size_t>(c)]);
    }
    uint64_t carriesRefused = 0;
    for (size_t s = 1; s < 256; ++s) carriesRefused += get(st.carryRefused[s]);
    j["proof"] = {
        {"sessions_opened", get(st.sessionsOpened)},
        {"players_carried", peakCarried_},
        {"players_carried_at_end", end_.carried},
        {"carries_by_status", carriesByStatus()},
        {"releases_by_reason", releasesByReason()},
        {"crossings_attempted", get(st.crossingsAttempted)},
        {"crossings_completed", get(st.crossingsCompleted)},
        {"crossings_lost", end_.crossingsOpen},
        {"crossings_per_s", rate(crossingsAtWindowEnd_, crossingsAtWindowStart_)},
        {"crossing_plan", cfg_.releaseFirst ? "release-first" : "carry"},
        {"carry_retries", get(st.carryResends) + get(st.carryRetries)},
        {"api_calls_in_measured_phase", sum(st.api.duringWindow)},
        {"api_calls_in_measured_phase_refreshes_due", refreshesDue},
        {"api_calls_in_measured_phase_other", sum(st.api.duringWindow) - refreshesDue},
        {"api_calls_caused_by_crossing",
         st.api.byCause(ApiCause::Crossing) + get(st.api.requestsFromCrossingPath)},
        {"hmac_failures", hmacFailures},
        {"unauthorized_answers", get(st.errorCodes[7])},
        {"refused",
         {{"carries_refused", carriesRefused},
          {"unauthorized", get(st.errorCodes[7])},
          {"other_errors", otherErrors},
          {"total", carriesRefused + get(st.errorCodes[7]) + otherErrors}}},
        {"nothing_refused", carriesRefused + get(st.errorCodes[7]) + otherErrors == 0},
        {"own_state_received", get(st.ownStateReceived)},
        {"own_state_sent_by_the_receiving_session", get(st.ownStateReceived)},
        {"own_state_sent_by_another_session_before_the_handoff", get(st.otherSentBeforeHandoff)},
        {"plain_communications", get(st.plainCommunications)},
        {"datagrams_sent", get(st.txDatagrams)},
        {"datagrams_received", get(st.rxDatagrams)},
        {"bytes_sent", get(st.txBytes)},
        {"bytes_received", get(st.rxBytes)},
    };
    summary_ = std::move(j);
}

} // namespace lt::fleet
