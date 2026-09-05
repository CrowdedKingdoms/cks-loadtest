#include "Config.hpp"
#include "ControlServer.hpp"
#include "GraphQLClient.hpp"
#include "Harness.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

void installSignals() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
}

} // namespace

int main(int argc, char** argv) {
    lt::Config config = lt::Config::load(argc, argv);
    installSignals();
    lt::GraphQLClient::globalInit();

    std::printf("cks-loadtest: start=%d capacity=%d base=%d width=%d x %d Hz on %d "
                "threads, app %lld instance=%s\n",
                config.clients, config.indexLimit, config.indexBase, config.indexWidth,
                config.updateHz, config.threads, static_cast<long long>(config.appId),
                config.instanceId.c_str());
    std::printf("management api: %s\n", config.managementApiUrl.c_str());

    lt::Harness harness(config);
    if (!harness.start(g_stop)) {
        lt::GraphQLClient::globalCleanup();
        return g_stop.load() ? 130 : 1;
    }

    lt::ControlServer http(harness, config.parsedBind(), config.controlToken);
    http.start();

    const auto testStart = std::chrono::steady_clock::now();
    bool rxHealthFailed = false;
    std::chrono::steady_clock::time_point healthFrom{};
    bool healthArmed = false;
    while (!g_stop.load() && !harness.shuttingDown()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - testStart;
        if (config.durationSec > 0 &&
            elapsed >= std::chrono::seconds(config.durationSec)) {
            std::printf("[main] duration reached, shutting down\n");
            break;
        }
        int live = harness.stats().activeClients.load(std::memory_order_relaxed) +
                   harness.stats().suspendedClients.load(std::memory_order_relaxed);
        if (live > 1 && !healthArmed) {
            healthArmed = true;
            healthFrom = now;
        }
        if (config.rxHealthTimeoutSec > 0 && healthArmed && !rxHealthFailed &&
            now - healthFrom >= std::chrono::seconds(config.rxHealthTimeoutSec)) {
            uint64_t tx = harness.stats().txPackets.load(std::memory_order_relaxed);
            uint64_t rx = harness.stats().rxDatagrams.load(std::memory_order_relaxed);
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

    http.stop();
    harness.stop();
    lt::GraphQLClient::globalCleanup();
    if (rxHealthFailed) return 3;
    return 0;
}
