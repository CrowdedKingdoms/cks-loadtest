#include "Stats.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>

namespace lt {

namespace {

struct Snapshot {
    uint64_t txPackets, txBytes, txSendErrors;
    uint64_t rxDatagrams, rxBytes, rxBundles, rxActorNotifications;
    uint64_t rxErrorMessages, rxReconnectCommands, rxHmacFailures;
    uint64_t latencySamples, latencySumMs;
    uint64_t tokenRefreshes, reassignments, controlFailures;
    int activeClients, suspendedClients;
};

Snapshot snapshot(const Stats& s) {
    return Snapshot{
        s.txPackets.load(std::memory_order_relaxed),
        s.txBytes.load(std::memory_order_relaxed),
        s.txSendErrors.load(std::memory_order_relaxed),
        s.rxDatagrams.load(std::memory_order_relaxed),
        s.rxBytes.load(std::memory_order_relaxed),
        s.rxBundles.load(std::memory_order_relaxed),
        s.rxActorNotifications.load(std::memory_order_relaxed),
        s.rxErrorMessages.load(std::memory_order_relaxed),
        s.rxReconnectCommands.load(std::memory_order_relaxed),
        s.rxHmacFailures.load(std::memory_order_relaxed),
        s.latencySamples.load(std::memory_order_relaxed),
        s.latencySumMs.load(std::memory_order_relaxed),
        s.tokenRefreshes.load(std::memory_order_relaxed),
        s.reassignments.load(std::memory_order_relaxed),
        s.controlFailures.load(std::memory_order_relaxed),
        s.activeClients.load(std::memory_order_relaxed),
        s.suspendedClients.load(std::memory_order_relaxed),
    };
}

const char* LATENCY_LABELS[Stats::LATENCY_BUCKETS] = {
    "<5ms", "<10ms", "<25ms", "<50ms", "<100ms", "<250ms", "<1s", ">=1s"};

const char* errorCodeName(int code) {
    switch (code) {
        case 5: return "INVALID_TOKEN";
        case 6: return "APP_NOT_FOUND";
        case 7: return "UNAUTHORIZED";
        case 13: return "GAME_TOKEN_WRONG_SIZE";
        case 15: return "INVALID_REQUEST";
        case 18: return "INVALID_APP_ID";
        case 20: return "USER_NOT_AUTHENTICATED";
        case 32: return "TOKEN_EXPIRED";
        default: return "UNKNOWN";
    }
}

} // namespace

StatsReporter::StatsReporter(Stats& stats, int intervalSec, std::string csvPath)
    : stats_(stats), intervalSec_(intervalSec), csvPath_(std::move(csvPath)) {}

StatsReporter::~StatsReporter() { stop(); }

void StatsReporter::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void StatsReporter::stop() {
    bool was = running_.exchange(false);
    if (was && thread_.joinable()) thread_.join();
}

void StatsReporter::run() {
    using namespace std::chrono;

    std::ofstream csv;
    if (!csvPath_.empty()) {
        bool writeHeader = true;
        {
            std::ifstream probe(csvPath_);
            writeHeader = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
        }
        csv.open(csvPath_, std::ios::app);
        if (csv && writeHeader) {
            csv << "epoch_sec,active_clients,suspended_clients,tx_pps,tx_bps,"
                   "rx_dps,rx_bps,rx_notif_ps,rx_errors_total,reconnects_total,"
                   "hmac_failures_total,avg_latency_ms\n";
        }
    }

    Snapshot prev = snapshot(stats_);
    auto prevTime = steady_clock::now();

    while (running_.load()) {
        // Sleep in small slices so stop() is responsive.
        for (int i = 0; i < intervalSec_ * 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        if (!running_.load()) break;

        Snapshot cur = snapshot(stats_);
        auto now = steady_clock::now();
        double dt = duration_cast<duration<double>>(now - prevTime).count();
        if (dt <= 0) dt = 1;

        double txPps = static_cast<double>(cur.txPackets - prev.txPackets) / dt;
        double txBps = static_cast<double>(cur.txBytes - prev.txBytes) / dt;
        double rxDps = static_cast<double>(cur.rxDatagrams - prev.rxDatagrams) / dt;
        double rxBps = static_cast<double>(cur.rxBytes - prev.rxBytes) / dt;
        double rxNps =
            static_cast<double>(cur.rxActorNotifications - prev.rxActorNotifications) / dt;
        uint64_t dLatSamples = cur.latencySamples - prev.latencySamples;
        uint64_t dLatSum = cur.latencySumMs - prev.latencySumMs;
        double avgLat = dLatSamples ? static_cast<double>(dLatSum) / dLatSamples : 0.0;

        std::printf(
            "[stats] clients=%d/%d tx=%.0f pps %.1f KB/s | rx=%.0f dps %.1f KB/s "
            "notif=%.0f/s | lat~%.1fms | errs=%" PRIu64 " reconnects=%" PRIu64
            " hmac_fail=%" PRIu64 " send_fail=%" PRIu64 "\n",
            cur.activeClients, cur.activeClients + cur.suspendedClients, txPps,
            txBps / 1024.0, rxDps, rxBps / 1024.0, rxNps, avgLat,
            cur.rxErrorMessages, cur.rxReconnectCommands, cur.rxHmacFailures,
            cur.txSendErrors);
        std::fflush(stdout);

        if (csv) {
            csv << duration_cast<seconds>(system_clock::now().time_since_epoch()).count()
                << ',' << cur.activeClients << ',' << cur.suspendedClients << ','
                << static_cast<uint64_t>(txPps) << ',' << static_cast<uint64_t>(txBps)
                << ',' << static_cast<uint64_t>(rxDps) << ','
                << static_cast<uint64_t>(rxBps) << ',' << static_cast<uint64_t>(rxNps)
                << ',' << cur.rxErrorMessages << ',' << cur.rxReconnectCommands << ','
                << cur.rxHmacFailures << ',' << avgLat << '\n';
            csv.flush();
        }

        prev = cur;
        prevTime = now;
    }
}

void StatsReporter::printFinalSummary() const {
    Snapshot s = snapshot(stats_);
    std::printf("\n===== load test summary =====\n");
    std::printf("tx: %" PRIu64 " packets, %" PRIu64 " bytes (%" PRIu64 " send errors)\n",
                s.txPackets, s.txBytes, s.txSendErrors);
    std::printf("rx: %" PRIu64 " datagrams, %" PRIu64 " bytes, %" PRIu64
                " bundles, %" PRIu64 " actor notifications\n",
                s.rxDatagrams, s.rxBytes, s.rxBundles, s.rxActorNotifications);
    std::printf("control: %" PRIu64 " token refreshes, %" PRIu64
                " reassignments, %" PRIu64 " failures\n",
                s.tokenRefreshes, s.reassignments, s.controlFailures);
    if (s.rxHmacFailures) {
        std::printf("HMAC verification failures: %" PRIu64 "\n", s.rxHmacFailures);
    }

    if (s.latencySamples) {
        std::printf("one-way latency estimate (server epoch vs local clock; "
                    "includes clock skew):\n");
        std::printf("  samples=%" PRIu64 " avg=%.1fms max=%" PRIu64 "ms\n",
                    s.latencySamples,
                    static_cast<double>(s.latencySumMs) / s.latencySamples,
                    stats_.latencyMaxMs.load(std::memory_order_relaxed));
        for (int i = 0; i < Stats::LATENCY_BUCKETS; ++i) {
            uint64_t n = stats_.latencyHist[i].load(std::memory_order_relaxed);
            if (!n) continue;
            std::printf("  %-7s %" PRIu64 " (%.1f%%)\n", LATENCY_LABELS[i], n,
                        100.0 * static_cast<double>(n) /
                            static_cast<double>(s.latencySamples));
        }
    }

    bool anyErr = false;
    for (int c = 0; c < 256; ++c) {
        uint64_t n = stats_.errorCodes[c].load(std::memory_order_relaxed);
        if (!n) continue;
        if (!anyErr) {
            std::printf("server error messages by code:\n");
            anyErr = true;
        }
        std::printf("  code %d (%s): %" PRIu64 "\n", c, errorCodeName(c), n);
    }
    std::printf("=============================\n");
    std::fflush(stdout);
}

} // namespace lt
