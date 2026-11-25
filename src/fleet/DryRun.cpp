#include "fleet/DryRun.hpp"

#include "fleet/FleetRun.hpp"

namespace lt::fleet {

FakeRunResult runAgainstFake(FakeRealtime& fake, FleetConfig cfg, std::atomic<bool>& stop,
                             FILE* log, std::function<void(double, FakeRealtime&)> onTick) {
    cfg.dryRun = true;
    cfg.orgToken = FakeRealtime::ORG_TOKEN;
    cfg.appId = fake.appId();
    if (cfg.api.empty()) cfg.api = "dry-run";

    RunDeps deps;
    deps.mode = "dry-run";
    deps.makeTransport = [&fake] { return fake.transport(); };
    deps.connect = fake.connector();
    if (onTick) deps.onTick = [&fake, onTick](double t) { onTick(t, fake); };

    // What the stand-in had counted before this run, so the result is this run's.
    const auto& c = fake.counters();
    const uint64_t open0 = c.apiOpen, mint0 = c.apiMint, close0 = c.apiClose,
                   refreshSession0 = c.apiRefreshSession,
                   withoutServer0 = c.apiRefreshSessionWithoutServer,
                   refreshToken0 = c.apiRefreshAppToken, carries0 = c.carriesAccepted,
                   refused0 = c.carriesRefused, released0 = c.releasedReason1,
                   releaseRequests0 = c.releaseRequests, updates0 = c.updatesAccepted,
                   unauthorized0 = c.updatesUnauthorized, badHmac0 = c.badHmac,
                   ownState0 = c.ownStateOut, caps0 = c.capabilities, heartbeats0 = c.heartbeats;

    FakeRunResult out;
    {
        FleetRun run(cfg, deps, stop);
        run.setLog(log);
        out.exitCode = run.run();
        out.summary = run.summary();
    }
    out.apiOpen = c.apiOpen - open0;
    out.apiMint = c.apiMint - mint0;
    out.apiClose = c.apiClose - close0;
    out.apiRefreshSession = c.apiRefreshSession - refreshSession0;
    out.apiRefreshSessionWithoutServer = c.apiRefreshSessionWithoutServer - withoutServer0;
    out.apiRefreshAppToken = c.apiRefreshAppToken - refreshToken0;
    out.apiCalls = out.apiOpen + out.apiMint + out.apiClose + out.apiRefreshSession +
                   out.apiRefreshAppToken;
    out.carriesAccepted = c.carriesAccepted - carries0;
    out.carriesRefused = c.carriesRefused - refused0;
    out.releasedReason1 = c.releasedReason1 - released0;
    out.releaseRequests = c.releaseRequests - releaseRequests0;
    out.updatesAccepted = c.updatesAccepted - updates0;
    out.updatesUnauthorized = c.updatesUnauthorized - unauthorized0;
    out.badHmac = c.badHmac - badHmac0;
    out.ownStateOut = c.ownStateOut - ownState0;
    out.capabilities = c.capabilities - caps0;
    out.heartbeats = c.heartbeats - heartbeats0;
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
