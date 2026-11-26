#include "fleet/Modes.hpp"

#include "fleet/FleetApi.hpp"
#include "fleet/FleetStats.hpp"

#include <chrono>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace lt::fleet {

int runDeletePlayers(const FleetConfig& cfg, const RunDeps& deps, std::atomic<bool>& stop,
                     nlohmann::json& summary, FILE* log) {
    // Every player this client can have made: the load run's, by number, and
    // the rule check's, by name.
    std::vector<std::string> ids;
    ids.reserve(static_cast<size_t>(cfg.players) + rulesPlayerIds().size());
    for (int n = 0; n < cfg.players; ++n) ids.push_back(playerExternalId(n));
    for (const auto& id : rulesPlayerIds()) ids.push_back(id);

    ApiCounters counters;
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> deleted{0}, notFound{0}, failed{0};
    std::mutex mu; // the pacing clock, and the first error
    std::string firstError;
    const double gapNs = 1e9 / cfg.signinRate;
    double due = static_cast<double>(nowNs());

    if (log) {
        std::fprintf(log, "[fleet] deleting %zu players (fleet-load-0 to fleet-load-%d and the rule "
                          "check's %zu) at %.0f a second\n",
                     ids.size(), cfg.players - 1, rulesPlayerIds().size(), cfg.signinRate);
    }
    auto work = [&] {
        std::unique_ptr<ApiTransport> transport;
        try {
            transport = deps.makeTransport();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mu);
            if (firstError.empty()) firstError = e.what();
            return;
        }
        FleetApi api(std::move(transport), cfg.orgToken, cfg.appId, counters, &stop);
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= ids.size() || stop.load()) return;
            // One deletion every 1 / rate seconds over all threads. A deletion
            // is a write across the cluster, like a sign-in.
            double mine;
            {
                std::lock_guard<std::mutex> lock(mu);
                mine = due;
                due = std::max(due, static_cast<double>(nowNs())) + gapNs;
            }
            while (static_cast<double>(nowNs()) < mine && !stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            try {
                // PLATFORM_BUSY and RATE_LIMITED are waited out inside the call.
                if (api.deleteManagedPlayer(cfg.orgId, ids[i], ApiCause::Cleanup)) {
                    deleted.fetch_add(1);
                } else {
                    notFound.fetch_add(1);
                }
            } catch (const std::exception& e) {
                failed.fetch_add(1);
                std::lock_guard<std::mutex> lock(mu);
                if (firstError.empty()) firstError = e.what();
            }
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.apiThreads; ++t) threads.emplace_back(work);
    for (auto& t : threads) t.join();

    // The roster holds tokens of players that no longer exist.
    bool rosterRemoved = false;
    if (!cfg.rosterFile.empty() && failed.load() == 0 && !stop.load()) {
        rosterRemoved = ::unlink(cfg.rosterFile.c_str()) == 0;
    }

    const bool pass = failed.load() == 0 && !stop.load();
    const size_t k = static_cast<size_t>(ApiKind::DeleteManagedPlayer);
    summary = {
        {"tool", "ck-fleet-load"},
        {"mode", deps.mode == "dry-run" ? "delete-players dry-run" : "delete-players"},
        {"pass", pass},
        {"exit_code", pass ? 0 : (stop.load() ? 130 : 1)},
        {"interrupted", stop.load()},
        {"players_asked", ids.size()},
        {"deleted", deleted.load()},
        {"not_found", notFound.load()},
        {"failed", failed.load()},
        {"first_error", firstError},
        {"roster_file_removed", rosterRemoved},
        {"api",
         {{"calls", counters.total()},
          {"deleteManagedPlayer", counters.byKind(ApiKind::DeleteManagedPlayer)},
          {"platform_busy", counters.platformBusy[k].load()},
          {"rate_limited", counters.rateLimited[k].load()}}},
    };
    if (log) {
        std::fprintf(log, "[fleet] deleted %llu, not found %llu, failed %llu%s%s\n",
                     static_cast<unsigned long long>(deleted.load()),
                     static_cast<unsigned long long>(notFound.load()),
                     static_cast<unsigned long long>(failed.load()),
                     firstError.empty() ? "" : ": ", firstError.c_str());
    }
    return summary["exit_code"].get<int>();
}

} // namespace lt::fleet
