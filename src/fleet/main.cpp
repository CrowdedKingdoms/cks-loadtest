// ck-fleet-load: the fleet session load client.
//
// S simulated game servers, each with one fleet session, carry P players who
// wander a grid of regions and cross between servers. See docs/FLEET-LOAD.md.

#include "GraphQLClient.hpp"
#include "fleet/DryRun.hpp"
#include "fleet/FleetConfig.hpp"
#include "fleet/FleetRun.hpp"
#include "fleet/Modes.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

void installSignals() {
    // The first SIGINT or SIGTERM ends the run cleanly: the sessions are closed
    // and the summary is printed. The handler then resets, so a second one ends
    // the process at once.
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}

void printDryRunRates(const nlohmann::json& s) {
    std::fprintf(stderr,
                 "[dry-run] no network was used. Rates over %.1f s:\n"
                 "[dry-run]   actor updates sent      %.0f/s (expected %.0f/s)\n"
                 "[dry-run]   notifications received  %.0f/s\n"
                 "[dry-run]   crossings completed     %.1f/s (target %.1f/s)\n"
                 "[dry-run]   carries                 %llu sent, %llu carried\n"
                 "[dry-run]   generator send lag      p50 %.2f ms, p99 %.2f ms, max %.2f ms\n"
                 "[dry-run]   API calls for crossings %llu\n",
                 s["timing"]["measured_s"].get<double>(),
                 s["traffic"]["updates_per_s"].get<double>(),
                 s["traffic"]["updates_per_s_expected"].get<double>(),
                 s["traffic"]["notifications_per_s"].get<double>(),
                 s["crossings"]["achieved_per_s"].get<double>(),
                 s["crossings"]["target_per_s"].get<double>(),
                 static_cast<unsigned long long>(s["carries"]["sent"].get<uint64_t>()),
                 static_cast<unsigned long long>(s["carries"]["carried"].get<uint64_t>()),
                 s["generator"]["send_lag"]["p50_ms"].get<double>(),
                 s["generator"]["send_lag"]["p99_ms"].get<double>(),
                 s["generator"]["send_lag"]["max_ms"].get<double>(),
                 static_cast<unsigned long long>(
                     s["api"]["calls_caused_by_crossing"].get<uint64_t>()));
}

} // namespace

int main(int argc, char** argv) {
    lt::fleet::FleetConfig cfg = lt::fleet::FleetConfig::load(argc, argv);
    if (cfg.selfTest) return lt::fleet::runSelfTest();

    const std::string problem = cfg.validate();
    if (!problem.empty()) {
        std::fprintf(stderr, "error: %s\n", problem.c_str());
        return 2;
    }
    installSignals();

    int code = 0;
    nlohmann::json summary;
    if (cfg.dryRun) {
        // Nothing to wait for in a dry run: no API to pace, no authorization to settle.
        if (!cfg.signinRateGiven) cfg.signinRate = 5000;
        if (!cfg.sessionSettleGiven) cfg.sessionSettleMs = 0;
        if (!cfg.rulesIdleGiven) cfg.rulesIdleTimeoutS = 3;
        if (!cfg.rulesStepGiven) cfg.rulesStepS = 0.5;
        lt::fleet::FakeRealtime::Options options;
        if (cfg.rulesCheck)
            options.idleTimeoutMs = static_cast<int64_t>(cfg.rulesIdleTimeoutS * 1000);
        lt::fleet::FakeRealtime fake(cfg.appId > 0 ? cfg.appId : 4242, options);
        fake.start();
        if (cfg.rulesCheck || cfg.deletePlayers) {
            const lt::fleet::FleetConfig fakeCfg = lt::fleet::configForFake(cfg, fake);
            const lt::fleet::RunDeps deps = lt::fleet::fakeDeps(fake);
            code = cfg.rulesCheck
                       ? lt::fleet::runRulesCheck(fakeCfg, deps, g_stop, summary, stderr)
                       : lt::fleet::runDeletePlayers(fakeCfg, deps, g_stop, summary, stderr);
        } else {
            const lt::fleet::FakeRunResult r =
                lt::fleet::runAgainstFake(fake, cfg, g_stop, stderr);
            code = r.exitCode;
            summary = r.summary;
            printDryRunRates(summary);
        }
        fake.stop();
    } else {
        lt::GraphQLClient::globalInit();
        {
            lt::fleet::RunDeps deps;
            deps.mode = "live";
            deps.makeTransport = [&cfg] {
                return lt::fleet::makeCurlTransport(cfg.api, cfg.tlsInsecure);
            };
            deps.connect = [&cfg](const lt::fleet::FleetSessionInfo& info, std::string& error) {
                return lt::fleet::connectUdp(info, cfg.socketRcvbufBytes, error);
            };
            if (cfg.rulesCheck) {
                code = lt::fleet::runRulesCheck(cfg, deps, g_stop, summary, stderr);
            } else if (cfg.deletePlayers) {
                code = lt::fleet::runDeletePlayers(cfg, deps, g_stop, summary, stderr);
            } else {
                lt::fleet::FleetRun run(cfg, deps, g_stop);
                code = run.run();
                summary = run.summary();
            }
        }
        lt::GraphQLClient::globalCleanup();
    }

    const std::string text = summary.dump(2);
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
    if (!cfg.summaryFile.empty()) {
        std::ofstream f(cfg.summaryFile);
        if (f) {
            f << text << '\n';
        } else {
            std::fprintf(stderr, "warning: cannot write %s\n", cfg.summaryFile.c_str());
        }
    }
    if (code != 0) {
        if (summary.contains("fail_reasons")) {
            for (const auto& why : summary["fail_reasons"]) {
                std::fprintf(stderr, "[fleet] FAILED: %s\n", why.get<std::string>().c_str());
            }
        } else {
            std::fprintf(stderr, "[fleet] FAILED\n");
        }
    } else {
        std::fprintf(stderr, "[fleet] passed\n");
    }
    return code;
}
