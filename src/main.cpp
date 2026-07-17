#include "Config.hpp"
#include "GraphQLClient.hpp"
#include "Provisioner.hpp"
#include "Stats.hpp"
#include "Worker.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

/// Runs GraphQL work (token refresh, server reassignment) on behalf of the
/// workers so their tick loops never block on HTTP.
void controlLoop(lt::Provisioner& provisioner, lt::ControlQueue& queue,
                 std::vector<std::unique_ptr<lt::Worker>>& workers,
                 lt::Stats& stats) {
    lt::GraphQLClient mgmt(provisioner.config().managementApiUrl,
                           provisioner.config().tlsInsecure);
    lt::ControlRequest req;
    while (queue.pop(req)) {
        lt::ClientUpdate update;
        update.clientIndex = req.clientIndex;
        update.creds = std::move(req.creds);
        try {
            if (req.kind == lt::ControlRequest::Kind::REFRESH) {
                provisioner.refreshToken(mgmt, update.creds);
                stats.tokenRefreshes.fetch_add(1, std::memory_order_relaxed);
            }
            // Assignment can legitimately fail for a while (every Buddy
            // reporting Full during an overload shed); retry with backoff
            // like a real reconnecting client instead of abandoning the
            // simulated client. The refreshed token above stays valid.
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
            stats.reassignments.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            stats.controlFailures.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[control] client %d %s failed: %s\n",
                         update.creds.index,
                         req.kind == lt::ControlRequest::Kind::REFRESH
                             ? "refresh"
                             : "reassign",
                         e.what());
            update.failed = true;
        }
        workers[static_cast<size_t>(req.workerIndex)]->postUpdate(std::move(update));
    }
}

} // namespace

int main(int argc, char** argv) {
    lt::Config config = lt::Config::load(argc, argv);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    lt::GraphQLClient::globalInit();

    std::printf("cks-loadtest: %d clients x %d Hz on %d threads, app %lld\n",
                config.clients, config.updateHz, config.threads,
                static_cast<long long>(config.appId));
    std::printf("management api: %s\n", config.managementApiUrl.c_str());

    // ---- Phase 1: provision (GraphQL) -----------------------------------
    lt::Provisioner provisioner(config);
    std::vector<lt::ClientCredentials> creds = provisioner.provisionAll(g_stop);
    if (creds.empty()) {
        lt::GraphQLClient::globalCleanup();
        return g_stop.load() ? 130 : 1;
    }
    std::printf("[provision] all %zu clients signed in, minted, and assigned\n",
                creds.size());

    // ---- Phase 2: simulate (UDP) -----------------------------------------
    lt::Stats stats;
    lt::ControlQueue controlQueue;

    std::vector<std::unique_ptr<lt::Worker>> workers;
    workers.reserve(static_cast<size_t>(config.threads));
    for (int w = 0; w < config.threads; ++w) {
        workers.push_back(
            std::make_unique<lt::Worker>(w, config, stats, controlQueue));
    }

    // Ramp schedule: batches of rampBatchSize activate every rampIntervalMs,
    // ordered by global client index; clients also respect their session
    // settle time (udpReadyAt).
    const auto rampStart = std::chrono::steady_clock::now();
    for (auto& c : creds) {
        int batch = c.index / config.rampBatchSize;
        auto activateAt =
            rampStart + std::chrono::milliseconds(
                            static_cast<int64_t>(batch) * config.rampIntervalMs);
        int workerIdx = c.index % config.threads;
        workers[static_cast<size_t>(workerIdx)]->addClient(std::move(c),
                                                           activateAt);
    }
    creds.clear();

    lt::StatsReporter reporter(stats, config.statsIntervalSec, config.csvOut);
    reporter.start();

    std::thread controlThread([&] {
        controlLoop(provisioner, controlQueue, workers, stats);
    });

    for (auto& w : workers) w->start();

    // ---- Main loop: duration, signals, RX health -------------------------
    const auto testStart = std::chrono::steady_clock::now();
    bool rxHealthFailed = false;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto elapsed = std::chrono::steady_clock::now() - testStart;
        if (config.durationSec > 0 &&
            elapsed >= std::chrono::seconds(config.durationSec)) {
            std::printf("[main] duration reached, shutting down\n");
            break;
        }
        // Fail fast when we are clearly sending into a black hole: traffic
        // out, nothing back after the health deadline. Requires >1 client
        // to guarantee fan-out (a lone client receives nothing by design).
        if (config.rxHealthTimeoutSec > 0 && config.clients > 1 &&
            !rxHealthFailed &&
            elapsed >= std::chrono::seconds(config.rxHealthTimeoutSec)) {
            uint64_t tx = stats.txPackets.load(std::memory_order_relaxed);
            uint64_t rx = stats.rxDatagrams.load(std::memory_order_relaxed);
            if (tx > 0 && rx == 0) {
                std::fprintf(
                    stderr,
                    "[main] RX HEALTH FAILURE: sent %llu packets but received "
                    "nothing after %ds. Check the Buddy address is reachable "
                    "(UDP egress), the session settle wait, and your token.\n",
                    static_cast<unsigned long long>(tx),
                    config.rxHealthTimeoutSec);
                rxHealthFailed = true;
                break;
            }
        }
    }

    // ---- Shutdown ---------------------------------------------------------
    std::printf("[main] stopping workers...\n");
    for (auto& w : workers) w->stop();
    controlQueue.shutdown();
    controlThread.join();
    reporter.stop();
    reporter.printFinalSummary();

    lt::GraphQLClient::globalCleanup();
    if (rxHealthFailed) return 3;
    return 0;
}
