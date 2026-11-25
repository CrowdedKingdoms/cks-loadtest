// Unit tests for the world of the fleet load client: the region grid, the walk,
// crossings, and the controller that holds the crossing rate on its target
// (src/fleet/Grid.hpp, src/fleet/CrossRate.hpp).

#include "fleet/CrossRate.hpp"
#include "fleet/Grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace lt::fleet;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

void testGridShape() {
    Grid g = Grid::forServers(100, 16);
    check(g.cols == 10 && g.rows == 10 && g.regions() == 100, "grid: 100 servers are 10 x 10");
    check(g.width() == 160.0 && g.height() == 160.0, "grid: 10 regions of 16 chunks are 160 chunks");
    check(g.originChunkX == -80 && g.originChunkY == -80, "grid: the world is placed around chunk (0, 0)");

    g = Grid::forServers(12, 8);
    check(g.cols == 4 && g.rows == 3 && g.regions() == 12, "grid: 12 servers are 4 x 3");
    g = Grid::forServers(7, 8);
    check(g.cols == 7 && g.rows == 1 && g.regions() == 7, "grid: 7 servers are a row of 7");
    g = Grid::forServers(1, 8);
    check(g.cols == 1 && g.rows == 1, "grid: one server is one region");
    g = Grid::forServers(64, 4);
    check(g.cols == 8 && g.rows == 8, "grid: 64 servers are 8 x 8");
    bool everyRegionHasAServer = true;
    for (int s : {2, 3, 5, 6, 9, 10, 16, 50, 99, 101, 144}) {
        const Grid any = Grid::forServers(s, 16);
        everyRegionHasAServer = everyRegionHasAServer && any.regions() == s;
    }
    check(everyRegionHasAServer, "grid: every region has a server, whatever the count");
}

void testRegionAt() {
    const Grid g = Grid::forServers(100, 16);
    check(g.regionAt(0.0, 0.0) == 0, "regionAt: the low corner is region 0");
    check(g.regionAt(15.999, 15.999) == 0, "regionAt: just inside region 0");
    check(g.regionAt(16.0, 0.0) == 1, "regionAt: the border itself belongs to the next region");
    check(g.regionAt(0.0, 16.0) == 10, "regionAt: one row up is ten regions on");
    check(g.regionAt(159.9, 159.9) == 99, "regionAt: the high corner is region 99");
    check(g.regionAt(88.0, 40.0) == g.regionOf(5, 2) && g.regionOf(5, 2) == 25,
          "regionAt: column 5, row 2 is region 25");
    check(g.colOf(25) == 5 && g.rowOf(25) == 2, "regionAt: and back");
    check(g.regionAt(-3.0, 500.0) == 90 && g.regionAt(1e9, -1e9) == 9,
          "regionAt: a position outside the world counts as the nearest region");

    // Chunk coordinates in the app, around chunk (0, 0).
    check(g.chunkX(0.0) == -80 && g.chunkX(80.0) == 0 && g.chunkX(159.5) == 79,
          "chunks: the world's 160 chunks run from -80 to 79");
    check(g.chunkY(16.25) == -64, "chunks: a position's chunk is its floor plus the origin");
}

void testNeighbours() {
    const Grid g = Grid::forServers(100, 16);
    auto sorted = [&](int r) {
        auto n = g.neighbours(r);
        std::sort(n.begin(), n.end());
        return n;
    };
    check(sorted(0) == std::vector<int>({1, 10, 11}), "neighbours: a corner has 3");
    check(sorted(5) == std::vector<int>({4, 6, 14, 15, 16}), "neighbours: an edge has 5");
    check(sorted(55) == std::vector<int>({44, 45, 46, 54, 56, 64, 65, 66}),
          "neighbours: an inner region has 8");
    check(sorted(99) == std::vector<int>({88, 89, 98}), "neighbours: the far corner has 3");
    bool symmetric = true;
    for (int a = 0; a < g.regions(); ++a) {
        for (int b : g.neighbours(a)) {
            const auto back = g.neighbours(b);
            symmetric = symmetric && std::find(back.begin(), back.end(), a) != back.end() &&
                        g.adjacent(a, b) && g.adjacent(b, a);
        }
    }
    check(symmetric, "neighbours: if a touches b then b touches a");
    check(!g.adjacent(0, 0) && !g.adjacent(0, 2) && !g.adjacent(9, 10) && g.adjacent(0, 11),
          "neighbours: a region is not its own; row ends do not wrap; corners touch");
    const Grid row = Grid::forServers(7, 8);
    check(row.neighbours(0).size() == 1 && row.neighbours(3).size() == 2,
          "neighbours: in a single row there are 1 or 2");

    // A player near a border is within replication range of the next region.
    check(std::fabs(g.distanceToBorder(15.0, 8.0) - 1.0) < 1e-9,
          "border: one chunk from the border on the right");
    check(std::fabs(g.distanceToBorder(8.0, 8.0) - 8.0) < 1e-9,
          "border: the middle of a corner region is 8 chunks from its borders");
    check(std::fabs(g.distanceToBorder(1.0, 1.0) - 15.0) < 1e-9,
          "border: the world's edge is not a border");
    bool allWithinRange = true;
    for (double x = 0.5; x < 160; x += 1.0) {
        for (double y = 0.5; y < 160; y += 7.0) {
            const int r = g.regionAt(x, y);
            const bool inner = g.colOf(r) > 0 && g.colOf(r) < 9 && g.rowOf(r) > 0 && g.rowOf(r) < 9;
            if (inner && g.distanceToBorder(x, y) > 8.0) allWithinRange = false;
        }
    }
    check(allWithinRange, "border: with 16-chunk regions every inner player is within 8 chunks of one");
}

void testCrossingDetectedOnce() {
    const Grid g = Grid::forServers(100, 16);
    RegionTracker t;
    check(t.update(g, 10.0, 8.0) == -1 && t.region == 0, "crossing: the first position is not a crossing");

    // Walk east in small steps over the border at x = 16.
    int crossings = 0, from = -2;
    for (double x = 10.0; x <= 20.0; x += 0.06) {
        const int left = t.update(g, x, 8.0);
        if (left >= 0) {
            ++crossings;
            from = left;
        }
    }
    check(crossings == 1 && from == 0 && t.region == 1, "crossing: a walk over a border is detected exactly once");

    // Standing still, or walking inside the region, is not a crossing.
    crossings = 0;
    for (int i = 0; i < 100; ++i) {
        if (t.update(g, 17.0 + 0.1 * i, 8.0) >= 0) ++crossings;
    }
    check(crossings == 0, "crossing: walking inside a region is none");

    // Walking back is a second crossing, in the other direction.
    from = -2;
    crossings = 0;
    for (double x = 20.0; x >= 12.0; x -= 0.06) {
        const int left = t.update(g, x, 8.0);
        if (left >= 0) {
            ++crossings;
            from = left;
        }
    }
    check(crossings == 1 && from == 1 && t.region == 0, "crossing: walking back is one more");

    // A step over a corner is one crossing, to the region diagonally across.
    RegionTracker corner;
    corner.update(g, 15.9, 15.9);
    check(corner.update(g, 16.1, 16.1) == 0 && corner.region == 11,
          "crossing: a step over a corner is one crossing to the diagonal region");

    // A long step that passes a whole region is one crossing to where it ends.
    RegionTracker far;
    far.update(g, 8.0, 8.0);
    check(far.update(g, 40.0, 8.0) == 0 && far.region == 2,
          "crossing: a long step is one crossing, to the region it ends in");

    // Many walkers: each change of region is counted once, and the region
    // tracked always equals the region the walker stands in.
    std::mt19937_64 rng(42);
    std::vector<Walker> walkers(500);
    std::vector<RegionTracker> trackers(500);
    for (size_t i = 0; i < walkers.size(); ++i) {
        placeWalker(walkers[i], g, rng);
        trackers[i].update(g, walkers[i].x, walkers[i].y);
    }
    int counted = 0, changes = 0;
    bool consistent = true, inside = true;
    for (int step = 0; step < 600; ++step) {
        for (size_t i = 0; i < walkers.size(); ++i) {
            const int before = g.regionAt(walkers[i].x, walkers[i].y);
            stepWalker(walkers[i], g, 2.0, 0.2, rng);
            const int after = g.regionAt(walkers[i].x, walkers[i].y);
            if (before != after) ++changes;
            if (trackers[i].update(g, walkers[i].x, walkers[i].y) >= 0) ++counted;
            consistent = consistent && trackers[i].region == after;
            inside = inside && walkers[i].x >= 0 && walkers[i].x < g.width() && walkers[i].y >= 0 &&
                     walkers[i].y < g.height();
        }
    }
    check(counted == changes && changes > 1000, "crossing: 500 walkers, every change of region counted once");
    check(consistent, "crossing: the tracked region is always the region stood in");
    check(inside, "walk: nobody leaves the world");
}

void testConfinedWalk() {
    const Grid g = Grid::forServers(9, 16);
    std::mt19937_64 rng(7);
    Walker w;
    w.x = 24.0; // region 4, the middle one
    w.y = 24.0;
    randomHeading(w, rng);
    bool stayed = true;
    double moved = 0;
    for (int i = 0; i < 5000; ++i) {
        const double x0 = w.x, y0 = w.y;
        stepWalker(w, g, 3.0, 0.2, rng, 4);
        moved += std::hypot(w.x - x0, w.y - y0);
        stayed = stayed && g.regionAt(w.x, w.y) == 4;
    }
    check(stayed, "walk: a confined walker never leaves its region");
    check(moved > 1000.0, "walk: a confined walker keeps moving");
}

/// Crossings a second measured over `seconds` of a simulated world.
struct Sim {
    Grid g;
    std::mt19937_64 rng;
    std::vector<Walker> walkers;
    std::vector<RegionTracker> trackers;
    Sim(int servers, int regionChunks, int players, uint64_t seed)
        : g(Grid::forServers(servers, regionChunks)), rng(seed), walkers(static_cast<size_t>(players)),
          trackers(static_cast<size_t>(players)) {
        for (size_t i = 0; i < walkers.size(); ++i) {
            placeWalker(walkers[i], g, rng);
            trackers[i].update(g, walkers[i].x, walkers[i].y);
        }
    }
    uint64_t run(double speed, double seconds, double dt = 0.2) {
        uint64_t crossings = 0;
        for (double t = 0; t < seconds; t += dt) {
            for (size_t i = 0; i < walkers.size(); ++i) {
                stepWalker(walkers[i], g, speed, dt, rng);
                if (trackers[i].update(g, walkers[i].x, walkers[i].y) >= 0) ++crossings;
            }
        }
        return crossings;
    }
};

void testCrossRateFormula() {
    const Grid g = Grid::forServers(100, 16);
    const double v = speedForCrossRate(g, 3000, 75.0);
    check(std::fabs(expectedCrossRate(g, 3000, v) - 75.0) < 1e-9,
          "cross rate: speedForCrossRate inverts expectedCrossRate");
    // 3,000 players, 75 a second: each player crosses about every 40 seconds.
    check(v > 0.3 && v < 0.4, "cross rate: 75 a second among 3,000 players is about a third of a chunk a second");
    check(std::fabs(expectedCrossRate(g, 3000, 2 * v) - 150.0) < 1e-9, "cross rate: twice the speed, twice the rate");
    check(std::fabs(expectedCrossRate(g, 6000, v) - 150.0) < 1e-9, "cross rate: twice the players, twice the rate");
    check(speedForCrossRate(Grid::forServers(1, 16), 3000, 75.0) == 0.0,
          "cross rate: one region has nothing to cross");
    check(speedForCrossRate(g, 3000, 0.0) == 0.0, "cross rate: a target of 0 asks for no speed");
    const double wider = speedForCrossRate(Grid::forServers(100, 32), 3000, 75.0);
    check(std::fabs(wider - 2 * v) < 1e-9, "cross rate: regions twice as wide need twice the speed");

    // The formula against a simulated world: close enough for a start value.
    Sim sim(100, 16, 3000, 11);
    sim.run(v, 20.0); // settle
    const double measured = static_cast<double>(sim.run(v, 120.0)) / 120.0;
    std::printf("   formula speed %.4f chunks/s gives %.1f crossings/s in simulation (target 75)\n", v, measured);
    check(measured > 0.75 * 75.0 && measured < 1.25 * 75.0,
          "cross rate: the formula's speed gives the target within a quarter in simulation");
}

double controlledRate(double startFactor, double target, int servers, int players, uint64_t seed,
                      double* finalSpeed = nullptr) {
    Sim sim(servers, 16, players, seed);
    const double formula = speedForCrossRate(sim.g, players, target);
    CrossRateController c(target, formula * startFactor, formula / 8.0, formula * 8.0);
    double speed = c.speed();
    const double interval = 5.0;
    // 40 intervals: the first 20 to settle, the last 20 measured.
    uint64_t late = 0;
    for (int k = 0; k < 40; ++k) {
        const uint64_t n = sim.run(speed, interval);
        if (k >= 20) late += n;
        speed = c.update(n, interval);
    }
    if (finalSpeed) *finalSpeed = speed;
    return static_cast<double>(late) / (20 * interval);
}

void testController() {
    const double target = 75.0;
    for (double start : {1.0, 2.0, 0.5, 4.0, 0.25}) {
        double speed = 0;
        const double achieved = controlledRate(start, target, 100, 3000, 5, &speed);
        std::printf("   start at %.2fx the formula speed: achieved %.1f crossings/s, speed %.4f\n", start,
                    achieved, speed);
        check(std::fabs(achieved - target) < 0.07 * target,
              "controller: holds the achieved rate within 7% of the target");
    }
    {
        const double achieved = controlledRate(3.0, 20.0, 16, 400, 9);
        check(std::fabs(achieved - 20.0) < 0.12 * 20.0,
              "controller: a small world with a low target is held as well");
    }

    // It only trims: one step changes the speed by at most a quarter.
    CrossRateController c(75.0, 1.0, 0.125, 8.0);
    const double up = c.update(1, 5.0); // far too few crossings
    check(up > 1.0 && up <= 1.25 + 1e-12, "controller: one step raises the speed by at most a quarter");
    CrossRateController d(75.0, 1.0, 0.125, 8.0);
    const double down = d.update(100000, 5.0); // far too many
    check(down < 1.0 && down >= 0.8 - 1e-12, "controller: one step lowers it by at most a fifth");
    CrossRateController quiet(75.0, 1.0, 0.125, 8.0);
    check(quiet.update(0, 5.0) == 1.25, "controller: with no crossings at all it walks faster");

    // It stays inside its bounds.
    CrossRateController bounded(75.0, 1.0, 0.5, 2.0);
    double s = 1.0;
    for (int i = 0; i < 50; ++i) s = bounded.update(0, 5.0);
    check(s == 2.0, "controller: never above its ceiling");
    for (int i = 0; i < 50; ++i) s = bounded.update(1000000, 5.0);
    check(s == 0.5, "controller: never below its floor");

    // On target it holds still.
    CrossRateController steady(75.0, 1.0, 0.125, 8.0);
    for (int i = 0; i < 10; ++i) s = steady.update(375, 5.0);
    check(std::fabs(s - 1.0) < 1e-12 && std::fabs(steady.smoothedRate() - 75.0) < 1e-9,
          "controller: on target the speed does not move");

    // A target of 0 and an empty interval change nothing.
    CrossRateController off(0.0, 1.0, 0.125, 8.0);
    check(off.update(500, 5.0) == 1.0, "controller: a target of 0 leaves the speed alone");
    check(steady.update(10, 0.0) == s, "controller: an interval of no length is ignored");
}

} // namespace

int main() {
    testGridShape();
    testRegionAt();
    testNeighbours();
    testCrossingDetectedOnce();
    testConfinedWalk();
    testCrossRateFormula();
    testController();

    if (g_failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all fleet world tests passed\n");
    return 0;
}
