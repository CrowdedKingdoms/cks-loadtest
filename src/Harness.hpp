#pragma once

#include "Config.hpp"
#include "Provisioner.hpp"
#include "Stats.hpp"
#include "Worker.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lt {

/// Long-lived generator: workers, GraphQL control thread, stats, hot-add.
class Harness {
public:
    explicit Harness(Config config);
    ~Harness();
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// Provision the start slice (if LT_CLIENTS > 0) and start workers.
    /// Returns false on a fatal provision error.
    bool start(std::atomic<bool>& stop);

    nlohmann::json statusJson() const;
    nlohmann::json statsJson() const;

    /// Queue an async add. Empty string = accepted; otherwise an error.
    std::string requestAdd(int count);

    std::string openRung(std::string id);
    nlohmann::json closeRung();

    /// The app's replay logging as read at the end of the run: kept for `status` and the
    /// summary, and written into the last rung file, which was written before it was known.
    void recordReplayLoggingAtEnd(const std::string& atEnd);

    void requestShutdown();
    bool shuttingDown() const { return shutdown_.load(std::memory_order_relaxed); }

    /// Stop workers, reporter, control thread. Idempotent.
    void stop();

    const Config& config() const { return config_; }
    Stats& stats() { return stats_; }
    int used() const;

    /// True when the last add failed (see addError).
    bool busy() const;

private:
    void graphqlLoop();
    void runAdd(int count, int globalStart);
    void distribute(std::vector<ClientCredentials> creds);
    void syncSignIn();
    void ensureStatsDir() const;
    void writeRungFile(const std::string& id, const nlohmann::json& body) const;

    Config config_;
    Stats stats_;
    std::unique_ptr<Provisioner> provisioner_;
    ControlQueue controlQueue_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::unique_ptr<StatsReporter> reporter_;
    std::thread graphqlThread_;
    std::thread addThread_;

    mutable std::mutex mu_;
    /// The app's replay logging as READ from the API, not as requested: at the start of the
    /// run (after setting it, for on / off) and at its end. "unknown" when it could not be
    /// read; the end is empty until the run has stopped.
    std::string replayLoggingObserved_ = "unknown";
    std::string replayLoggingAtEnd_;
    /// The last rung file written, so the end reading can be added to it.
    std::string lastRungId_;
    nlohmann::json lastRungBody_;
    /// Read it now, report it, and warn when it records under `leave`. Returns the label.
    std::string observeReplayLogging(const char* when);
    int used_ = 0;
    bool busy_ = false;
    std::string addError_;
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool>* externalStop_ = nullptr;
};

} // namespace lt
