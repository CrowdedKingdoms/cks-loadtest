#pragma once

#include "fleet/FleetConfig.hpp"
#include "fleet/FleetRun.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

/// The two small modes of ck-fleet-load beside the load run.
namespace lt::fleet {

/// The players `--rules-check` signs in. Stable ids, so a rerun reuses them and
/// `--delete-players` can remove them.
///   a      blocks b; carried by the first session
///   b      blocked by a; carried by the second session
///   c      a witness beside a, on the first session
///   d      a witness beside b, on the second session
///   minor  16 years old, on the second session
///   idle   sends one update and then only heartbeats, on the first session
inline const std::vector<std::string>& rulesPlayerIds() {
    static const std::vector<std::string> ids = {"fleet-rules-a", "fleet-rules-b",
                                                 "fleet-rules-c", "fleet-rules-d",
                                                 "fleet-rules-minor", "fleet-rules-idle"};
    return ids;
}

/// `--delete-players`: delete fleet-load-0 to fleet-load-<players - 1> and the
/// rule check's players with deleteManagedPlayer, at `--signin-rate` a second.
/// Returns 0 when no deletion failed. `summary` is one JSON object.
int runDeletePlayers(const FleetConfig& cfg, const RunDeps& deps, std::atomic<bool>& stop,
                     nlohmann::json& summary, FILE* log);

/// `--rules-check`: two fleet sessions carry six players and check, one rule at
/// a time, that each per-player rule holds for a carried player. Prints PASS or
/// FAIL with what it observed for each check to `log`. Returns 0 when no check
/// failed. `summary` is one JSON object.
int runRulesCheck(const FleetConfig& cfg, const RunDeps& deps, std::atomic<bool>& stop,
                  nlohmann::json& summary, FILE* log);

} // namespace lt::fleet
