#include "fleet/DryRun.hpp"

#include "fleet/FleetRun.hpp"

namespace lt::fleet {

FakeRunResult runAgainstFake(FleetConfig cfg, FakeRealtime::Options options,
                             std::atomic<bool>& stop, FILE* log,
                             std::function<void(double, FakeRealtime&)> onTick) {
    cfg.dryRun = true;
    cfg.orgToken = FakeRealtime::ORG_TOKEN;
    if (cfg.appId <= 0) cfg.appId = 4242;
    if (cfg.api.empty()) cfg.api = "dry-run";

    FakeRealtime fake(cfg.appId, options);
    fake.start();

    RunDeps deps;
    deps.mode = "dry-run";
    deps.makeTransport = [&fake] { return fake.transport(); };
    deps.connect = fake.connector();
    if (onTick) deps.onTick = [&fake, onTick](double t) { onTick(t, fake); };

    FakeRunResult out;
    {
        FleetRun run(cfg, deps, stop);
        run.setLog(log);
        out.exitCode = run.run();
        out.summary = run.summary();
    }
    const auto& c = fake.counters();
    out.apiCalls = fake.apiCalls();
    out.apiOpen = c.apiOpen;
    out.apiMint = c.apiMint;
    out.apiClose = c.apiClose;
    out.apiRefreshSession = c.apiRefreshSession;
    out.apiRefreshSessionWithoutServer = c.apiRefreshSessionWithoutServer;
    out.apiRefreshAppToken = c.apiRefreshAppToken;
    out.carriesAccepted = c.carriesAccepted;
    out.carriesRefused = c.carriesRefused;
    out.releasedReason1 = c.releasedReason1;
    out.releaseRequests = c.releaseRequests;
    out.updatesAccepted = c.updatesAccepted;
    out.updatesUnauthorized = c.updatesUnauthorized;
    out.badHmac = c.badHmac;
    out.ownStateOut = c.ownStateOut;
    out.capabilities = c.capabilities;
    out.heartbeats = c.heartbeats;
    out.openSessionsAtEnd = fake.openSessions();
    fake.stop();
    return out;
}

} // namespace lt::fleet
