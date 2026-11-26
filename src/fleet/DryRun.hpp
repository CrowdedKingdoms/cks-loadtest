#pragma once

#include "fleet/FakeRealtime.hpp"
#include "fleet/FleetConfig.hpp"
#include "fleet/FleetRun.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

namespace lt::fleet {

/// What a run against the in-process stand-in ended with: the client's own
/// summary, and what the stand-in counted on its side of the wire.
struct FakeRunResult {
    int exitCode = 0;
    nlohmann::json summary;
    // The API's own count of the calls it answered.
    uint64_t apiCalls = 0;
    uint64_t apiOpen = 0;
    uint64_t apiMint = 0;
    uint64_t apiClose = 0;
    uint64_t apiRefreshSession = 0;
    uint64_t apiRefreshSessionWithoutServer = 0;
    uint64_t apiRefreshAppToken = 0;
    // The realtime side.
    uint64_t carriesAccepted = 0;
    uint64_t carriesRefused = 0;
    uint64_t releasedReason1 = 0;
    uint64_t releaseRequests = 0;
    uint64_t updatesAccepted = 0;
    uint64_t updatesUnauthorized = 0;
    uint64_t badHmac = 0;
    uint64_t ownStateOut = 0;
    uint64_t capabilities = 0;
    uint64_t heartbeats = 0;
    int openSessionsAtEnd = 0;
    /// Every counter of the stand-in for this run, by its name in
    /// FakeRealtime::Counters.
    std::map<std::string, uint64_t> fake;
};

/// Run `cfg` with no network: the stand-in answers the Application API and the
/// realtime wire. `onTick` is called during the measured window with the seconds
/// since it opened, to inject faults; `before` is called once before the run.
FakeRunResult runAgainstFake(FleetConfig cfg, FakeRealtime::Options options,
                             std::atomic<bool>& stop, FILE* log,
                             std::function<void(double, FakeRealtime&)> onTick = nullptr,
                             std::function<void(FakeRealtime&)> before = nullptr);

/// What a run reaches a stand-in through, and the options a run against it needs
/// (its org token, organization and app).
RunDeps fakeDeps(FakeRealtime& fake);
FleetConfig configForFake(FleetConfig cfg, FakeRealtime& fake);

/// The same against a stand-in the caller keeps, so two runs can share one.
FakeRunResult runAgainstFake(FakeRealtime& fake, FleetConfig cfg, std::atomic<bool>& stop,
                             FILE* log,
                             std::function<void(double, FakeRealtime&)> onTick = nullptr);

/// The built-in checks behind `--self-test`. Prints one line a check to stdout
/// and returns 0 when all pass.
int runSelfTest();

} // namespace lt::fleet
