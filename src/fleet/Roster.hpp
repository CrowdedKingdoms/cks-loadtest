#pragma once

#include "fleet/FleetApi.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

/// The roster file of ck-fleet-load: the player tokens one run minted, kept so
/// the next run signs nobody in again.
///
/// The file holds live app tokens. It is written with mode 0600, replaced
/// atomically, and its contents are never printed.
namespace lt::fleet {

struct Roster {
    std::string api;     // the API origin the tokens were minted against
    int64_t appId = 0;
    std::unordered_map<int, PlayerToken> players; // by player number
};

enum class RosterLoad {
    Missing,   // no file yet: the run mints and then writes one
    Loaded,
    Refused,   // unreadable, not JSON, or minted for another API or app
};

/// Read `path`. A roster minted against another API origin or app is refused: a
/// token is valid only where it was issued, and using it elsewhere fails in ways
/// that look like a broken tier.
RosterLoad loadRoster(const std::string& path, const std::string& api, int64_t appId,
                      Roster& out, std::string& error);

/// Write `roster` to `path` with mode 0600, through a temporary file in the same
/// directory and a rename. Returns false and sets `error` on failure.
bool saveRoster(const std::string& path, const Roster& roster, std::string& error);

} // namespace lt::fleet
