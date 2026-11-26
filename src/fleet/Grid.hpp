#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

/// The world of the fleet load client: a grid of regions, one game server (one
/// fleet session) a region, and the walk of a player across it.
///
/// Positions are in CHUNKS, as doubles, measured from the world's low corner.
/// A region is a square of `regionChunks` chunks a side. The world is placed
/// around chunk (0, 0) of the app, so `originChunkX/Y` is the chunk of the low
/// corner.
namespace lt::fleet {

struct Grid {
    int cols = 1;
    int rows = 1;
    int regionChunks = 16;
    int64_t originChunkX = 0;
    int64_t originChunkY = 0;

    /// A grid for `servers` regions: square when `servers` is a square number
    /// (10 x 10 for 100), otherwise the fullest rectangle (3 x 4 for 12, 1 x 7
    /// for 7). Every region has a server.
    static Grid forServers(int servers, int regionChunks) {
        Grid g;
        if (servers < 1) servers = 1;
        g.regionChunks = std::max(1, regionChunks);
        int cols = 1;
        for (int d = 1; d * d <= servers; ++d) {
            if (servers % d == 0) cols = d;
        }
        g.rows = cols;           // the smaller side
        g.cols = servers / cols; // the larger side
        g.originChunkX = -static_cast<int64_t>(g.cols) * g.regionChunks / 2;
        g.originChunkY = -static_cast<int64_t>(g.rows) * g.regionChunks / 2;
        return g;
    }

    int regions() const { return cols * rows; }
    double width() const { return static_cast<double>(cols) * regionChunks; }
    double height() const { return static_cast<double>(rows) * regionChunks; }

    int regionOf(int col, int row) const { return row * cols + col; }
    int colOf(int region) const { return region % cols; }
    int rowOf(int region) const { return region / cols; }

    /// The region a position stands in. A position outside the world counts as
    /// the nearest region, so a caller never gets an index that has no server.
    int regionAt(double x, double y) const {
        int col = static_cast<int>(std::floor(x / regionChunks));
        int row = static_cast<int>(std::floor(y / regionChunks));
        col = std::clamp(col, 0, cols - 1);
        row = std::clamp(row, 0, rows - 1);
        return regionOf(col, row);
    }

    /// The app's chunk coordinates of a position.
    int64_t chunkX(double x) const { return originChunkX + static_cast<int64_t>(std::floor(x)); }
    int64_t chunkY(double y) const { return originChunkY + static_cast<int64_t>(std::floor(y)); }

    /// How far an app chunk is from a region, in chunks, on the axis where it is
    /// farthest. 0 inside the region. Every player of a region's server stands
    /// inside it, so a notification from farther than the replication distance
    /// is out of range of all of them.
    int64_t chunksFromRegion(int region, int64_t appChunkX, int64_t appChunkY) const {
        const int64_t x0 = originChunkX + static_cast<int64_t>(colOf(region)) * regionChunks;
        const int64_t y0 = originChunkY + static_cast<int64_t>(rowOf(region)) * regionChunks;
        const int64_t x1 = x0 + regionChunks - 1, y1 = y0 + regionChunks - 1;
        const int64_t dx = appChunkX < x0 ? x0 - appChunkX : appChunkX > x1 ? appChunkX - x1 : 0;
        const int64_t dy = appChunkY < y0 ? y0 - appChunkY : appChunkY > y1 ? appChunkY - y1 : 0;
        return std::max(dx, dy);
    }

    /// True when two different regions touch (a side or a corner).
    bool adjacent(int a, int b) const {
        if (a == b) return false;
        return std::abs(colOf(a) - colOf(b)) <= 1 && std::abs(rowOf(a) - rowOf(b)) <= 1;
    }

    /// The regions that touch `region`: up to 8, fewer at the world's edge.
    std::vector<int> neighbours(int region) const {
        std::vector<int> out;
        const int c = colOf(region), r = rowOf(region);
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                const int nc = c + dc, nr = r + dr;
                if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) continue;
                out.push_back(regionOf(nc, nr));
            }
        }
        return out;
    }

    /// How far a position is from the nearest border it shares with another
    /// region, in chunks. A player closer than the replication distance is in
    /// range of the neighbouring region.
    double distanceToBorder(double x, double y) const {
        const int col = colOf(regionAt(x, y)), row = rowOf(regionAt(x, y));
        double best = 1e300;
        const double lx = x - static_cast<double>(col) * regionChunks;
        const double ly = y - static_cast<double>(row) * regionChunks;
        if (col > 0) best = std::min(best, lx);
        if (col < cols - 1) best = std::min(best, regionChunks - lx);
        if (row > 0) best = std::min(best, ly);
        if (row < rows - 1) best = std::min(best, regionChunks - ly);
        return best;
    }
};

/// A player's walk: a straight line that turns to a new random heading every
/// few seconds and bounces off the world's edge.
struct Walker {
    double x = 0, y = 0;
    double dx = 1, dy = 0;       // unit heading
    double reaimInS = 0;         // seconds of walking until the next turn
};

inline void randomHeading(Walker& w, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> angle(0.0, 6.283185307179586);
    const double a = angle(rng);
    w.dx = std::cos(a);
    w.dy = std::sin(a);
    std::uniform_real_distribution<double> re(4.0, 12.0);
    w.reaimInS = re(rng);
}

inline void placeWalker(Walker& w, const Grid& g, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> ux(0.0, g.width());
    std::uniform_real_distribution<double> uy(0.0, g.height());
    w.x = ux(rng);
    w.y = uy(rng);
    randomHeading(w, rng);
}

namespace detail {
inline void bounce(double& v, double& d, double lo, double hi) {
    if (v < lo) { v = lo + (lo - v); d = std::fabs(d); }
    if (v > hi) { v = hi - (v - hi); d = -std::fabs(d); }
    if (v < lo) v = lo; // a step longer than the box
    if (v > hi) v = hi;
}
} // namespace detail

/// Move a walker for `dt` seconds at `speed` chunks a second.
/// With `confineRegion` >= 0 the walker stays inside that region (it bounces off
/// the region's borders), which is how a run with no crossings keeps its players
/// moving.
inline void stepWalker(Walker& w, const Grid& g, double speed, double dt,
                       std::mt19937_64& rng, int confineRegion = -1) {
    if (dt <= 0) return;
    w.x += w.dx * speed * dt;
    w.y += w.dy * speed * dt;
    // Keep a hair inside the upper edge so floor() never lands on a region or a
    // chunk that does not exist.
    const double eps = 1e-9;
    if (confineRegion >= 0) {
        const double lox = static_cast<double>(g.colOf(confineRegion)) * g.regionChunks;
        const double loy = static_cast<double>(g.rowOf(confineRegion)) * g.regionChunks;
        detail::bounce(w.x, w.dx, lox, lox + g.regionChunks - eps);
        detail::bounce(w.y, w.dy, loy, loy + g.regionChunks - eps);
    } else {
        detail::bounce(w.x, w.dx, 0.0, g.width() - eps);
        detail::bounce(w.y, w.dy, 0.0, g.height() - eps);
    }
    w.reaimInS -= dt;
    if (w.reaimInS <= 0) randomHeading(w, rng);
}

/// Follows one player's region and reports a crossing exactly once for each
/// change of region.
struct RegionTracker {
    int region = -1;
    /// Returns the region left when the position is in a new region, otherwise -1.
    int update(const Grid& g, double x, double y) {
        const int now = g.regionAt(x, y);
        if (region < 0) {
            region = now;
            return -1;
        }
        if (now == region) return -1;
        const int from = region;
        region = now;
        return from;
    }
};

} // namespace lt::fleet
