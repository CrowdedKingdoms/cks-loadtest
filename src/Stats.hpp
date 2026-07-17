#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace lt {

/// Global counters, written by worker threads with relaxed atomics and read
/// by the reporter thread. Latency is a one-way estimate derived from the
/// server epoch-millis field in notification tails (subject to clock skew
/// between this host and the Buddy host).
struct Stats {
    std::atomic<uint64_t> txPackets{0};
    std::atomic<uint64_t> txBytes{0};
    std::atomic<uint64_t> txSendErrors{0};

    std::atomic<uint64_t> rxDatagrams{0};
    std::atomic<uint64_t> rxBytes{0};
    std::atomic<uint64_t> rxBundles{0};
    std::atomic<uint64_t> rxActorNotifications{0};
    std::atomic<uint64_t> rxOtherSpatial{0};
    std::atomic<uint64_t> rxErrorMessages{0};
    std::atomic<uint64_t> rxReconnectCommands{0};
    std::atomic<uint64_t> rxHmacFailures{0};
    std::atomic<uint64_t> rxMalformed{0};

    /// GENERIC_ERROR_MESSAGE counts keyed by wire error code.
    std::array<std::atomic<uint64_t>, 256> errorCodes{};

    // One-way latency (ms) from notification epoch tails.
    std::atomic<uint64_t> latencySamples{0};
    std::atomic<uint64_t> latencySumMs{0};
    std::atomic<uint64_t> latencyMaxMs{0};
    /// Buckets: <5, <10, <25, <50, <100, <250, <1000, >=1000 ms.
    static constexpr int LATENCY_BUCKETS = 8;
    std::array<std::atomic<uint64_t>, LATENCY_BUCKETS> latencyHist{};

    // Gauges maintained by workers / control thread.
    std::atomic<int> activeClients{0};
    std::atomic<int> suspendedClients{0};
    std::atomic<uint64_t> tokenRefreshes{0};
    std::atomic<uint64_t> reassignments{0};
    std::atomic<uint64_t> controlFailures{0};

    void recordLatencyMs(int64_t ms) {
        if (ms < 0) ms = 0; // clock skew can make the estimate negative
        latencySamples.fetch_add(1, std::memory_order_relaxed);
        latencySumMs.fetch_add(static_cast<uint64_t>(ms), std::memory_order_relaxed);
        uint64_t prev = latencyMaxMs.load(std::memory_order_relaxed);
        while (static_cast<uint64_t>(ms) > prev &&
               !latencyMaxMs.compare_exchange_weak(prev, static_cast<uint64_t>(ms),
                                                   std::memory_order_relaxed)) {
        }
        static constexpr int64_t bounds[LATENCY_BUCKETS - 1] = {5, 10, 25, 50,
                                                                100, 250, 1000};
        int bucket = LATENCY_BUCKETS - 1;
        for (int i = 0; i < LATENCY_BUCKETS - 1; ++i) {
            if (ms < bounds[i]) {
                bucket = i;
                break;
            }
        }
        latencyHist[bucket].fetch_add(1, std::memory_order_relaxed);
    }
};

/// Periodic console (and optional CSV) reporter. Runs on its own thread;
/// call stop() then join via destructor or stop() explicitly.
class StatsReporter {
public:
    StatsReporter(Stats& stats, int intervalSec, std::string csvPath);
    ~StatsReporter();

    void start();
    void stop();

    /// Print the end-of-run summary (totals, latency histogram, error codes).
    void printFinalSummary() const;

private:
    void run();

    Stats& stats_;
    int intervalSec_;
    std::string csvPath_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace lt
