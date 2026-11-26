#pragma once

#include "fleet/CrossRate.hpp"
#include "fleet/FleetApi.hpp"
#include "fleet/FleetConfig.hpp"
#include "fleet/FleetStats.hpp"
#include "fleet/FleetWorker.hpp"
#include "fleet/Roster.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lt::fleet {

/// What a run reaches the outside world through. A live run gives libcurl and
/// UDP; a dry run and the tests give the in-process stand-in.
struct RunDeps {
    std::function<std::unique_ptr<ApiTransport>()> makeTransport;
    Connector connect;
    /// Called about every 20 ms of the measured window with the seconds since it
    /// opened. Tests use it to inject faults at a known time.
    std::function<void(double)> onTick;
    /// "live" or "dry-run", for the summary.
    std::string mode = "live";
};

/// One run of the fleet load client, from opening the sessions to closing them.
class FleetRun {
public:
    FleetRun(FleetConfig cfg, RunDeps deps, std::atomic<bool>& stop);
    ~FleetRun();
    FleetRun(const FleetRun&) = delete;
    FleetRun& operator=(const FleetRun&) = delete;

    /// Runs to the end and returns the exit code: 0 passed, 1 a pass condition
    /// failed, 2 the setup failed, 130 interrupted.
    int run();

    /// The summary of the last run().
    const nlohmann::json& summary() const { return summary_; }
    const FleetStats& stats() const { return stats_; }
    /// Progress lines go here (stderr by default; tests silence them).
    void setLog(FILE* log) { log_ = log; }

private:
    struct Job {
        enum class Kind : uint8_t {
            Open, Mint, RefreshToken, SignInAgain, MoveSession, KeepSession, Close
        } kind = Kind::Open;
        int index = 0;
        ApiCause cause = ApiCause::Setup;
        ServerAddr current;
        int64_t gameTokenId = 0;
    };

    void apiThread();
    void push(Job j);
    bool pop(Job& out);
    void runJob(FleetApi& api, const Job& j);
    void onApiRequest(const ApiRequest& r);
    void setFatal(const std::string& why);
    void admit(int player);
    void storeToken(int player, const PlayerToken& t);
    void housekeeping();
    void progress(double windowS);
    void buildSummary(int exitCode, const std::vector<std::string>& failures);
    nlohmann::json carriesByStatus() const;
    nlohmann::json releasesByReason() const;
    int sumPending() const;
    int sumCarried() const;
    int sumAwaiting() const;

    FleetConfig cfg_;
    RunDeps deps_;
    std::atomic<bool>& stop_;
    FleetStats stats_;
    FILE* log_ = stderr;

    std::unique_ptr<World> world_;
    std::vector<std::unique_ptr<FleetWorker>> workers_;

    std::vector<std::thread> apiThreads_;
    std::mutex jobMu_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    bool jobsClosed_ = false;
    std::atomic<int> jobsInFlight_{0};

    std::mutex mu_;                              // sessions_, roster_, fatal_
    std::vector<FleetSessionInfo> sessions_;
    std::vector<uint8_t> sessionOpen_;
    Roster roster_;
    std::string fatal_;
    std::atomic<bool> hasFatal_{false};
    std::atomic<int> opened_{0};
    std::atomic<int> closeDone_{0};
    std::atomic<int> tokensReady_{0};

    std::vector<int64_t> refreshNotBeforeMs_;    // by player; the main thread's only
    std::vector<uint32_t> refreshTriedVersion_;  // the token version the last attempt started from
    std::vector<int64_t> keepNotBeforeMs_;       // by session
    std::unique_ptr<CrossRateController> controller_;
    double startSpeed_ = 0;

    // Marks for the summary.
    int64_t startNs_ = 0;
    int64_t windowStartNs_ = 0;
    int64_t windowEndNs_ = 0;
    uint64_t crossingsAtWindowStart_ = 0;
    uint64_t crossingsAtWindowEnd_ = 0;
    uint64_t updatesAtWindowStart_ = 0;
    uint64_t updatesAtWindowEnd_ = 0;
    uint64_t notificationsAtWindowStart_ = 0;
    uint64_t notificationsAtWindowEnd_ = 0;
    uint64_t lastCrossings_ = 0;
    uint64_t lastUpdates_ = 0;
    uint64_t lastNotifications_ = 0;
    int64_t lastProgressNs_ = 0;
    int64_t lastHousekeepingNs_ = 0;
    bool interrupted_ = false;
    FleetWorker::End end_;
    std::vector<FleetWorker::MinuteUse> usage_;
    int peakCarried_ = 0;
    nlohmann::json summary_;
};

} // namespace lt::fleet
