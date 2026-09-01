#include "Stats.hpp"

#include <cstdio>
#include <cmath>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

} // namespace

int main() {
    lt::CounterSnap a;
    a.txPackets = 100;
    a.rxActorNotifications = 50;
    a.latencySamples = 10;
    a.latencySumMs = 100;
    a.latencyMaxMs = 20;
    a.latencyHist[4] = 10; // <5ms bucket index: bounds 1,2,3,4,5 so index 4 is <5
    a.signInReused = 10;

    lt::CounterSnap b;
    b.txPackets = 40;
    b.rxActorNotifications = 20;
    b.latencySamples = 4;
    b.latencySumMs = 40;
    b.latencyHist[4] = 4;
    b.signInReused = 10;

    auto d = lt::CounterSnap::minus(a, b);
    check(d.txPackets == 60, "window tx diff");
    check(d.rxActorNotifications == 30, "window notif diff");
    check(d.latencySamples == 6, "window latency samples");
    check(d.signInReused == 0, "sign-in reused does not grow in window");

    lt::CounterSnap x;
    x.latencySamples = 100;
    x.latencyHist[0] = 50; // <1ms
    x.latencyHist[5] = 50; // <7ms  (bounds: 1,2,3,4,5,7 -> index 5)
    double p50 = lt::percentileMs(x, 0.50);
    check(p50 >= 0.5 && p50 <= 7.0, "p50 in hist range");
    double p99 = lt::percentileMs(x, 0.99);
    check(p99 >= p50, "p99 >= p50");

    lt::Stats stats;
    for (int i = 0; i < 100; ++i) stats.recordLatencyMs(3);
    stats.txPackets.store(1000);
    stats.markWindow("r1");
    for (int i = 0; i < 50; ++i) stats.recordLatencyMs(20);
    stats.txPackets.store(1300);
    auto win = stats.loadWindow();
    check(win.txPackets == 300, "markWindow then tx diff");
    check(win.latencySamples == 50, "window only counts post-open samples");
    check(stats.rungId() == "r1", "rung id");

    lt::SnapshotMeta m1;
    m1.instanceId = "a";
    m1.used = 100;
    m1.activeClients = 100;
    m1.rungId = "r1";
    m1.windowOpenEpochSec = 1756770000;
    m1.windowDurationSec = 180;
    lt::IntervalRates iv;
    iv.txPps = 1000;
    iv.rxNotifPs = 800;
    auto ja = lt::buildStatsJson(m1, a, d, iv);
    lt::SnapshotMeta m2 = m1;
    m2.instanceId = "b";
    // Later open, shorter window: the fanned-out rung-open reaches hosts milliseconds
    // apart, and the merged blob must bound both rather than describe one of them.
    m2.windowOpenEpochSec = 1756770002;
    m2.windowDurationSec = 178;
    auto jb = lt::buildStatsJson(m2, a, d, iv);
    auto fleet = lt::mergeFleetStats({ja, jb});
    check(fleet["used"] == 200, "fleet used sums");
    check(fleet["gauges"]["active"] == 200, "fleet active sums");
    check(std::abs(fleet["interval"]["tx_pps"].get<double>() - 2000.0) < 0.01,
          "fleet tx_pps sums not averaged");
    check(fleet["window"]["counters"]["tx_packets"] == 120, "fleet window tx sums");
    // The fleet window must carry an ABSOLUTE start, earliest of the hosts'. This
    // reported 0 until 2026-09-01: nothing in the merge loop copied it, so a rung
    // summary could not be aligned to a monitoring window without opening per_host.
    check(fleet["window"]["open_epoch_sec"] == 1756770000,
          "fleet window open is the EARLIEST host open, not zero");
    check(std::abs(fleet["window"]["duration_sec"].get<double>() - 180.0) < 0.01,
          "fleet window duration is the LONGEST host window");

    // A host that never opened the window reports 0, and must not drag the fleet's
    // start back to the epoch -- which is what a plain std::min would do.
    lt::SnapshotMeta m3 = m1;
    m3.instanceId = "c";
    m3.windowOpenEpochSec = 0;
    m3.windowDurationSec = 0;
    auto fleet3 = lt::mergeFleetStats({ja, lt::buildStatsJson(m3, a, d, iv)});
    check(fleet3["window"]["open_epoch_sec"] == 1756770000,
          "a host with no window does not zero the fleet's window start");

    // round-trip JSON
    auto back = lt::CounterSnap::fromJson(a.toJson());
    check(back.txPackets == 100, "fromJson tx");
    check(back.latencyHist[4] == 10, "fromJson hist");

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all ok\n");
    return 0;
}
