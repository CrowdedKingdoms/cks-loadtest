#include "Harness.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace lt {

namespace {

void controlLoop(Provisioner& provisioner, ControlQueue& queue,
                 std::vector<std::unique_ptr<Worker>>& workers, Stats& stats) {
    GraphQLClient mgmt(provisioner.config().managementApiUrl,
                       provisioner.config().tlsInsecure);
    ControlRequest req;
    while (queue.pop(req)) {
        ClientUpdate update;
        update.clientIndex = req.clientIndex;
        update.creds = std::move(req.creds);
        try {
            if (req.kind == ControlRequest::Kind::REFRESH) {
                // REFRESH WHERE THE CLIENT PLAYS. `mintAppToken` handed this client
                // its app's own Game API URL -- the instance in the datacenter that
                // holds the app's shards -- and a real client refreshes against
                // that. Through the management alias the request lands on either
                // datacenter, and every routed statement on the far one is a
                // cross-datacenter hop: measured ~60-66 ms each, four of them, on
                // 2026-09-05 (refreshAppToken 390-440 ms against 55 ms for `me`).
                if (!update.creds.gameApiUrl.empty() &&
                    update.creds.gameApiUrl != provisioner.config().managementApiUrl) {
                    GraphQLClient game(update.creds.gameApiUrl,
                                       provisioner.config().tlsInsecure);
                    provisioner.refreshToken(game, update.creds);
                } else {
                    provisioner.refreshToken(mgmt, update.creds);
                }
                stats.tokenRefreshes.fetch_add(1, std::memory_order_relaxed);
                // A REFRESHED TOKEN HAS NO SESSION ANYWHERE YET, and only
                // `serverWithLeastClients` installs one: it picks a server and sends
                // that Buddy the token authorization. A Buddy that was never told
                // about a token drops its packets SILENTLY -- no refusal, no
                // notification -- so "keep the old Buddy and let first contact
                // install it" (tried 2026-09-05) left every refreshed client mute for
                // exactly `LT_RX_SILENT_REASSIGN_SEC` and then reassigned anyway.
                // Until the API can authorize a refreshed token on the client's
                // CURRENT server (open item), a refresh must re-ask for placement.
                // The client stays put whenever placement hands back the server it
                // already had, and only a real move counts as a reassignment.
            }
            const std::string previousIp4 = update.creds.serverIp4;
            const uint16_t previousPort = update.creds.serverPort;
            for (int attempt = 1;; ++attempt) {
                try {
                    provisioner.assignServer(update.creds);
                    break;
                } catch (const std::exception& e) {
                    if (attempt >= 15 || queue.isShutdown()) throw;
                    if (attempt == 1) {
                        std::fprintf(stderr,
                                     "[control] client %d assign failed (%s); "
                                     "retrying with backoff\n",
                                     update.creds.index, e.what());
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
            const bool moved = update.creds.serverIp4 != previousIp4 ||
                               update.creds.serverPort != previousPort;
            if (req.kind != ControlRequest::Kind::REFRESH || moved) {
                stats.reassignments.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            stats.controlFailures.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[control] client %d %s failed: %s\n",
                         update.creds.index,
                         req.kind == ControlRequest::Kind::REFRESH ? "refresh"
                                                                   : "reassign",
                         e.what());
            update.failed = true;
        }
        workers[static_cast<size_t>(req.workerIndex)]->postUpdate(std::move(update));
    }
}

} // namespace

Harness::Harness(Config config) : config_(std::move(config)) {}

Harness::~Harness() { stop(); }

int Harness::used() const {
    std::lock_guard<std::mutex> lock(mu_);
    return used_;
}

bool Harness::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return busy_;
}

void Harness::syncSignIn() {
    if (!provisioner_) return;
    stats_.setSignIn(provisioner_->signIns().reused.load(),
                     provisioner_->signIns().mintedProvisioning.load(),
                     provisioner_->signIns().mintedMidRun.load());
}

void Harness::distribute(std::vector<ClientCredentials> creds) {
    const auto rampStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < creds.size(); ++i) {
        int batch = static_cast<int>(i) / config_.rampBatchSize;
        auto activateAt =
            rampStart + std::chrono::milliseconds(
                            static_cast<int64_t>(batch) * config_.rampIntervalMs);
        int workerIdx = creds[i].index % config_.threads;
        if (workerIdx < 0) workerIdx = 0;
        workers_[static_cast<size_t>(workerIdx)]->addClient(std::move(creds[i]),
                                                            activateAt);
    }
}

bool Harness::start(std::atomic<bool>& stop) {
    externalStop_ = &stop;
    ensureStatsDir();
    try {
        provisioner_ = std::make_unique<Provisioner>(config_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return false;
    }

    workers_.reserve(static_cast<size_t>(config_.threads));
    for (int w = 0; w < config_.threads; ++w) {
        workers_.push_back(
            std::make_unique<Worker>(w, config_, stats_, controlQueue_));
    }

    if (config_.clients > 0) {
        auto creds = provisioner_->provisionAll(stop);
        if (creds.empty()) return false;
        std::printf("[provision] all %zu clients have a session, an app token, "
                    "and a Buddy assignment\n",
                    creds.size());
        std::printf("[provision] sign-in: %d minted, %d reused from the roster\n",
                    provisioner_->signIns().mintedProvisioning.load(),
                    provisioner_->signIns().reused.load());
        {
            std::lock_guard<std::mutex> lock(mu_);
            used_ = static_cast<int>(creds.size());
        }
        provisioner_->markRunStarted();
        distribute(std::move(creds));
    } else {
        provisioner_->markRunStarted();
    }
    syncSignIn();
    stats_.markWindow("");

    std::string jsonl;
    if (!config_.statsDir.empty()) {
        jsonl = config_.statsDir + "/intervals.jsonl";
    }
    reporter_ = std::make_unique<StatsReporter>(
        stats_, config_.statsIntervalSec, config_.csvOut, jsonl, config_.instanceId);
    reporter_->start();

    graphqlThread_ = std::thread([this] { graphqlLoop(); });
    for (auto& w : workers_) w->start();
    started_.store(true, std::memory_order_release);
    return true;
}

void Harness::graphqlLoop() { controlLoop(*provisioner_, controlQueue_, workers_, stats_); }

std::string Harness::requestAdd(int count) {
    if (count < 1) return "count must be >= 1";
    int globalStart = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (busy_) return "busy: an add is already running";
        if (used_ + count > config_.indexLimit) {
            return "add would exceed LT_INDEX_LIMIT (" +
                   std::to_string(config_.indexLimit) + "); used=" +
                   std::to_string(used_);
        }
        busy_ = true;
        addError_.clear();
        globalStart = config_.indexBase + used_;
    }
    if (addThread_.joinable()) addThread_.join();
    addThread_ = std::thread([this, count, globalStart] { runAdd(count, globalStart); });
    return "";
}

void Harness::runAdd(int count, int globalStart) {
    std::atomic<bool> localStop{false};
    std::atomic<bool>* stop = externalStop_ ? externalStop_ : &localStop;
    std::printf("[add] provisioning %d clients at global index %d\n", count,
                globalStart);
    auto creds = provisioner_->provisionRange(globalStart, count, *stop);
    if (creds.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        busy_ = false;
        addError_ = stop->load() ? "add aborted" : "provisioning failed (see logs)";
        syncSignIn();
        return;
    }
    distribute(std::move(creds));
    {
        std::lock_guard<std::mutex> lock(mu_);
        used_ += count;
        busy_ = false;
        addError_.clear();
    }
    syncSignIn();
    std::printf("[add] %d clients queued onto workers (used=%d / %d)\n", count,
                used(), config_.indexLimit);
}

std::string Harness::openRung(std::string id) {
    if (id.empty()) return "rung id is required";
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (busy_) return "busy: finish the in-flight add before opening a rung";
    }
    stats_.markWindow(std::move(id));
    return "";
}

nlohmann::json Harness::closeRung() {
    auto body = statsJson();
    std::string id = body.value("rung_id", std::string());
    if (!id.empty()) writeRungFile(id, body);
    return body;
}

nlohmann::json Harness::statusJson() const {
    std::lock_guard<std::mutex> lock(mu_);
    int active = stats_.activeClients.load(std::memory_order_relaxed);
    int suspended = stats_.suspendedClients.load(std::memory_order_relaxed);
    int waiting = used_ - active - suspended;
    if (waiting < 0) waiting = 0;
    return {
        {"instance_id", config_.instanceId},
        {"index_base", config_.indexBase},
        {"used", used_},
        {"limit", config_.indexLimit},
        {"active", active},
        {"suspended", suspended},
        {"waiting", waiting},
        {"busy", busy_},
        {"add_error", addError_},
        {"rung_id", stats_.rungId()},
        {"threads", config_.threads},
        {"update_hz", config_.updateHz},
    };
}

nlohmann::json Harness::statsJson() const {
    SnapshotMeta m;
    {
        std::lock_guard<std::mutex> lock(mu_);
        m.instanceId = config_.instanceId;
        m.indexBase = config_.indexBase;
        m.used = used_;
        m.limit = config_.indexLimit;
        m.busy = busy_;
        m.addError = addError_;
    }
    m.rungId = stats_.rungId();
    m.windowOpenEpochSec = stats_.windowOpenEpochSec();
    m.windowDurationSec = stats_.windowDurationSec();
    m.activeClients = stats_.activeClients.load(std::memory_order_relaxed);
    m.suspendedClients = stats_.suspendedClients.load(std::memory_order_relaxed);
    return buildStatsJson(m, stats_.loadLifetime(), stats_.loadWindow(),
                          stats_.interval());
}

void Harness::requestShutdown() {
    shutdown_.store(true, std::memory_order_release);
    if (externalStop_) externalStop_->store(true);
}

void Harness::stop() {
    if (!started_.exchange(false)) {
        // still join add thread if any
        if (addThread_.joinable()) addThread_.join();
        return;
    }
    std::printf("[main] stopping workers...\n");
    for (auto& w : workers_) w->stop();
    controlQueue_.shutdown();
    if (graphqlThread_.joinable()) graphqlThread_.join();
    if (addThread_.joinable()) addThread_.join();
    if (reporter_) {
        reporter_->stop();
        int reused = 0, before = 0, during = 0;
        stats_.loadSignIn(reused, before, during);
        reporter_->printFinalSummary(config_.permWindowRadiusChunks, reused, before,
                                     during);
    }
}

void Harness::ensureStatsDir() const {
    if (config_.statsDir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(config_.statsDir, ec);
    if (ec) {
        std::fprintf(stderr, "warning: cannot create LT_STATS_DIR '%s': %s\n",
                     config_.statsDir.c_str(), ec.message().c_str());
    }
}

void Harness::writeRungFile(const std::string& id, const nlohmann::json& body) const {
    if (config_.statsDir.empty()) return;
    std::string path = config_.statsDir + "/rung-" + id + ".json";
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "warning: cannot write %s\n", path.c_str());
        return;
    }
    f << body.dump(2) << '\n';
    std::printf("[rung] wrote %s\n", path.c_str());
}

} // namespace lt
