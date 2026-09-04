#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lt {

/// Mergeable counter snapshot (lifetime or a rung window).
struct CounterSnap {
    uint64_t txPackets = 0;
    uint64_t txBytes = 0;
    uint64_t txSendErrors = 0;
    uint64_t rxDatagrams = 0;
    uint64_t rxBytes = 0;
    uint64_t rxBundles = 0;
    uint64_t rxActorNotifications = 0;
    uint64_t rxOtherSpatial = 0;
    uint64_t rxErrorMessages = 0;
    uint64_t rxReconnectCommands = 0;
    uint64_t rxSilentReassigns = 0;
    uint64_t rxHmacFailures = 0;
    uint64_t rxMalformed = 0;
    uint64_t tokenRefreshes = 0;
    uint64_t reassignments = 0;
    uint64_t controlFailures = 0;
    uint64_t unauthorizedFirstContact = 0;
    uint64_t unauthorizedWindowReload = 0;
    uint64_t latencySamples = 0;
    uint64_t latencySumMs = 0;
    uint64_t latencyMaxMs = 0;
    static constexpr int LATENCY_BUCKETS = 23;
    std::array<uint64_t, LATENCY_BUCKETS> latencyHist{};
    std::array<uint64_t, 256> errorCodes{};
    int signInReused = 0;
    int signInMintedBefore = 0;
    int signInMintedDuring = 0;

    static CounterSnap minus(const CounterSnap& now, const CounterSnap& origin);
    static CounterSnap plus(const CounterSnap& a, const CounterSnap& b);
    nlohmann::json toJson() const;
    static CounterSnap fromJson(const nlohmann::json& j);
};

/// Upper bounds (exclusive) of latency histogram buckets, last bucket is +inf.
/// 22 finite bounds + overflow = 23 buckets.
inline constexpr int64_t kLatencyBounds[CounterSnap::LATENCY_BUCKETS - 1] = {
    1,    2,    3,    4,    5,    7,    10,   15,   20,   25,  30,
    40,   50,   75,   100,  150,  200,  250,  400,  600,  1000, 2000};

/// Interpolated percentile in milliseconds from a histogram. `p` in [0, 1].
double percentileMs(const CounterSnap& snap, double p);

/// Last completed reporter interval (rates, not totals).
struct IntervalRates {
    double dtSec = 0;
    uint64_t epochSec = 0;
    double txPps = 0;
    double txBps = 0;
    double rxDps = 0;
    double rxBps = 0;
    double rxNotifPs = 0;
    double avgLatencyMs = 0;
};

/// Extra fields the HTTP snapshot stamps that live on the harness, not Stats.
struct SnapshotMeta {
    std::string instanceId;
    int indexBase = 0;
    int used = 0;
    int limit = 0;
    bool busy = false;
    std::string addError;
    std::string rungId;
    uint64_t windowOpenEpochSec = 0;
    double windowDurationSec = 0;
    int activeClients = 0;
    int suspendedClients = 0;
};

nlohmann::json buildStatsJson(const SnapshotMeta& meta, const CounterSnap& lifetime,
                              const CounterSnap& window, const IntervalRates& interval);

/// Merge N host snapshots (each is buildStatsJson output). Sums counts and
/// rates, max of max latency, merged histograms then percentiles.
nlohmann::json mergeFleetStats(const std::vector<nlohmann::json>& hosts);

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
    std::atomic<uint64_t> rxSilentReassigns{0};
    std::atomic<uint64_t> rxHmacFailures{0};
    std::atomic<uint64_t> rxMalformed{0};

    std::array<std::atomic<uint64_t>, 256> errorCodes{};

    std::atomic<uint64_t> unauthorizedFirstContact{0};
    std::atomic<uint64_t> unauthorizedWindowReload{0};

    std::atomic<uint64_t> latencySamples{0};
    std::atomic<uint64_t> latencySumMs{0};
    std::atomic<uint64_t> latencyMaxMs{0};
    std::array<std::atomic<uint64_t>, CounterSnap::LATENCY_BUCKETS> latencyHist{};
    /// Max latency observed since the open window; reset on markWindow().
    std::atomic<uint64_t> windowLatencyMaxMs{0};

    std::atomic<int> activeClients{0};
    std::atomic<int> suspendedClients{0};
    std::atomic<uint64_t> tokenRefreshes{0};
    std::atomic<uint64_t> reassignments{0};
    std::atomic<uint64_t> controlFailures{0};

    void recordLatencyMs(int64_t ms);

    CounterSnap loadLifetime() const;
    void markWindow(std::string id);
    CounterSnap loadWindow() const; // lifetime minus origin; window max

    std::string rungId() const;
    uint64_t windowOpenEpochSec() const;
    double windowDurationSec() const;

    void setInterval(const IntervalRates& r);
    IntervalRates interval() const;

    void setSignIn(int reused, int mintedBefore, int mintedDuring);
    void loadSignIn(int& reused, int& mintedBefore, int& mintedDuring) const;

private:
    mutable std::mutex mu_;
    CounterSnap windowOrigin_{};
    std::string rungId_;
    uint64_t windowOpenEpochSec_ = 0;
    std::chrono::steady_clock::time_point windowOpenSteady_{};
    IntervalRates interval_{};
    int signInReused_ = 0;
    int signInMintedBefore_ = 0;
    int signInMintedDuring_ = 0;
};

/// Periodic console (and optional CSV / JSONL) reporter.
class StatsReporter {
public:
    StatsReporter(Stats& stats, int intervalSec, std::string csvPath,
                  std::string jsonlPath, std::string instanceId);
    ~StatsReporter();

    void start();
    void stop();

    /// Print the end-of-run summary (totals, latency histogram, error codes).
    void printFinalSummary(int permWindowRadiusChunks, int reused = -1,
                           int mintedBefore = -1, int mintedDuring = -1) const;

private:
    void run();

    Stats& stats_;
    int intervalSec_;
    std::string csvPath_;
    std::string jsonlPath_;
    std::string instanceId_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace lt
