#include "fleet/FleetRun.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

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

void FleetRun::storeToken(int player, const PlayerToken& t) {
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
            bool done = false;
            try {
                const PlayerToken t = api.refreshAppToken(creds.snapshot().token, j.cause);
                storeToken(j.index, t);
                stats_.tokenRefreshes.fetch_add(1, std::memory_order_relaxed);
                done = true;
            } catch (const std::exception&) {
                stats_.tokenRefreshFailures.fetch_add(1, std::memory_order_relaxed);
            }
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
    }

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
            failures.push_back(std::to_string(get(stats_.ownStateReceived)) +
                               " update(s) of a session's own carried players came back to it "
                               "with FLEET_OWN_STATE off");
        }
    }
    // The assertion the proof rests on: no API call was made because of a crossing.
    const uint64_t crossingCalls =
        stats_.api.byCause(ApiCause::Crossing) + get(stats_.api.requestsFromCrossingPath);
    if (crossingCalls != 0) {
        failures.push_back(std::to_string(crossingCalls) +
                           " Application API call(s) were made because of a crossing");
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
    uint64_t refusedOther = 0;
    for (size_t s = 5; s < 256; ++s) refusedOther += get(st.carryRefused[s]);
    j["carries"] = {
        {"attempts", sum(st.carryAttempts)},
        {"attempts_by_cause", attempts},
        {"sent", get(st.carryDatagrams)},
        {"carried", sum(st.carryCarried)},
        {"carried_by_cause", carriedBy},
        {"refused",
         {{"1_no_live_token", get(st.carryRefused[1])},
          {"2_other_organization_or_app", get(st.carryRefused[2])},
          {"3_proof_or_time", get(st.carryRefused[3])},
          {"4_session_full", get(st.carryRefused[4])},
          {"other", refusedOther}}},
        {"gave_up_after_5s", get(st.carryTimedOut)},
        {"retried", get(st.carryRetries)},
        {"abandoned_player_moved_on", get(st.carryAbandoned)},
        {"results_duplicate", get(st.carryResultsDuplicate)},
        {"results_late", get(st.carryResultsLate)},
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
        {"by_reason", nonZero(st.releasedByReason)},
        {"old_session",
         {{"expected", get(st.releaseExpected)},
          {"received", get(st.releaseReceived)},
          {"delay_after_new_result", st.releaseDelay.toJson()},
          {"before_new_result", get(st.releaseBeforeResult)},
          {"never_arrived_within_5s", get(st.releaseMissing)},
          {"no_new_result_to_pair", get(st.releaseUnpaired)},
          {"pending_at_end", end_.releasesPending}}},
        {"reason_1_for_a_held_player", get(st.releasedUnexpected)},
        {"reason_1_after_a_late_carry", get(st.releasedAfterLateCarry)},
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
        {"unauthorized_first_contact", get(st.unauthFirstContact)},
        {"unauthorized_window_reload", get(st.unauthWindowReload)},
        {"unauthorized_after_handoff", get(st.unauthAfterHandoff)},
        {"unauthorized_unexplained", get(st.unauthUnexplained)},
        {"unauthorized_session_messages", get(st.unauthSessionMessages)},
        {"carried_again_after_unauthorized", get(st.recarriedAfterUnauthorized)},
    };

    // Traffic.
    nlohmann::json byType = nlohmann::json::array();
    for (int t = 0; t < 256; ++t) {
        const uint64_t n = get(st.rxByType[static_cast<size_t>(t)]);
        if (n) byType.push_back({{"type", t}, {"name", messageTypeName(t)}, {"n", n}});
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
        {"hmac_failures", get(st.rxHmacFailures)},
        {"malformed", get(st.rxMalformed)},
        {"unsigned_notifications", get(st.rxUnsignedSpatial)},
        {"fleet_frames_outside_signed_bundle", get(st.fleetFramesUnsigned)},
        {"fleet_frames_long_form", get(st.fleetFramesLongForm)},
    };

    j["replication"] = {
        {"sample_players", std::min(cfg_.latencySample, cfg_.players)},
        {"marked_updates_received", get(st.markersSeen)},
        {"latency", st.replicationLatency.toJson()},
    };
    j["own_state"] = {
        {"capability_set", cfg_.ownState},
        {"received", get(st.ownStateReceived)},
    };

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
    for (int c = 0; c < ApiCounters::C; ++c)
        causes[apiCauseName(static_cast<ApiCause>(c))] = st.api.byCause(static_cast<ApiCause>(c));
    j["api"] = {
        {"calls", st.api.total()},
        {"calls_by_kind", kinds},
        {"calls_by_cause", causes},
        {"calls_in_measured_window_by_kind", window},
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
    summary_ = std::move(j);
}

} // namespace lt::fleet
