#include "fleet/DryRun.hpp"

#include "fleet/FleetRun.hpp"

namespace lt::fleet {

RunDeps fakeDeps(FakeRealtime& fake) {
    RunDeps deps;
    deps.mode = "dry-run";
    deps.makeTransport = [&fake] { return fake.transport(); };
    deps.connect = fake.connector();
    return deps;
}

FleetConfig configForFake(FleetConfig cfg, FakeRealtime& fake) {
    cfg.dryRun = true;
    cfg.orgToken = FakeRealtime::ORG_TOKEN;
    cfg.orgId = FakeRealtime::ORG_ID;
    cfg.appId = fake.appId();
    if (cfg.api.empty()) cfg.api = "dry-run";
    return cfg;
}

FakeRunResult runAgainstFake(FakeRealtime& fake, FleetConfig cfg, std::atomic<bool>& stop,
                             FILE* log, std::function<void(double, FakeRealtime&)> onTick) {
    cfg = configForFake(std::move(cfg), fake);
    RunDeps deps = fakeDeps(fake);
    if (onTick) deps.onTick = [&fake, onTick](double t) { onTick(t, fake); };

    // What the stand-in had counted before this run, so the result is this run's.
    const std::map<std::string, uint64_t> before = fake.snapshot();

    FakeRunResult out;
    {
        FleetRun run(cfg, deps, stop);
        run.setLog(log);
        out.exitCode = run.run();
        out.summary = run.summary();
    }
    for (const auto& [name, value] : fake.snapshot()) out.fake[name] = value - before.at(name);
    out.fake["largestDatagramOut"] = fake.counters().largestDatagramOut.load(); // a maximum
    out.apiOpen = out.fake["apiOpen"];
    out.apiMint = out.fake["apiMint"];
    out.apiClose = out.fake["apiClose"];
    out.apiRefreshSession = out.fake["apiRefreshSession"];
    out.apiRefreshSessionWithoutServer = out.fake["apiRefreshSessionWithoutServer"];
    out.apiRefreshAppToken = out.fake["apiRefreshAppToken"];
    out.apiCalls = out.apiOpen + out.apiMint + out.apiClose + out.apiRefreshSession +
                   out.apiRefreshAppToken + out.fake["apiDelete"] + out.fake["apiBlock"];
    out.carriesAccepted = out.fake["carriesAccepted"];
    out.carriesRefused = out.fake["carriesRefused"];
    out.releasedReason1 = out.fake["releasedReason1"];
    out.releaseRequests = out.fake["releaseRequests"];
    out.updatesAccepted = out.fake["updatesAccepted"];
    out.updatesUnauthorized = out.fake["updatesUnauthorized"];
    out.badHmac = out.fake["badHmac"];
    out.ownStateOut = out.fake["ownStateOut"];
    out.capabilities = out.fake["capabilities"];
    out.heartbeats = out.fake["heartbeats"];
    out.openSessionsAtEnd = fake.openSessions();
    return out;
}

FakeRunResult runAgainstFake(FleetConfig cfg, FakeRealtime::Options options,
                             std::atomic<bool>& stop, FILE* log,
                             std::function<void(double, FakeRealtime&)> onTick,
                             std::function<void(FakeRealtime&)> before) {
    FakeRealtime fake(cfg.appId > 0 ? cfg.appId : 4242, options);
    if (before) before(fake);
    fake.start();
    FakeRunResult out = runAgainstFake(fake, std::move(cfg), stop, log, std::move(onTick));
    fake.stop();
    return out;
}

} // namespace lt::fleet
