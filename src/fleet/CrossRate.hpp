#pragma once

#include "fleet/Grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

/// How fast players must walk for the world to see a target number of crossings
/// a second, and the controller that holds the achieved rate on the target.
namespace lt::fleet {

/// Crossings a second expected from `players` spread evenly over the grid, each
/// walking at `speed` chunks a second on a random heading.
///
/// A player with a random heading moves at 2/pi of its speed along either axis
/// on average. A world `cols` regions wide has `cols - 1` borders across its
/// width, so along x a player meets a border every `cols * R / (cols - 1)`
/// chunks, and the same along y with `rows`.
inline double expectedCrossRate(const Grid& g, int players, double speed) {
    const double r = g.regionChunks;
    const double perChunk = (g.cols - 1.0) / (g.cols * r) + (g.rows - 1.0) / (g.rows * r);
    return players * (2.0 / 3.14159265358979323846) * speed * perChunk;
}

/// The speed, in chunks a second, at which expectedCrossRate gives `target`.
/// 0 when the grid has one region (nothing to cross) or nobody walks.
inline double speedForCrossRate(const Grid& g, int players, double target) {
    const double perUnitSpeed = expectedCrossRate(g, players, 1.0);
    if (perUnitSpeed <= 0 || target <= 0) return 0.0;
    return target / perUnitSpeed;
}

/// Holds the achieved crossing rate on the target by scaling the walking speed.
///
/// Each interval it is told how many crossings happened. It smooths the measured
/// rate, compares it with the target, and moves the speed part of the way toward
/// the speed that would have produced the target, by at most a quarter a step.
/// The start speed comes from speedForCrossRate, so the controller only trims.
class CrossRateController {
public:
    CrossRateController(double targetPerS, double startSpeed, double minSpeed, double maxSpeed)
        : target_(targetPerS), speed_(std::clamp(startSpeed, minSpeed, maxSpeed)),
          min_(minSpeed), max_(maxSpeed) {}

    /// Feed one interval. Returns the speed to use from now on.
    double update(uint64_t crossings, double intervalS) {
        if (intervalS <= 0 || target_ <= 0) return speed_;
        const double measured = static_cast<double>(crossings) / intervalS;
        smoothed_ = primed_ ? 0.5 * smoothed_ + 0.5 * measured : measured;
        primed_ = true;
        lastMeasured_ = measured;
        if (smoothed_ <= 0) {
            // Nobody crossed: walk faster, by the largest step.
            speed_ = std::clamp(speed_ > 0 ? speed_ * kMaxStep : min_, min_, max_);
            return speed_;
        }
        // The rate is proportional to the speed, so target / achieved is the
        // whole correction. Take its square root: half the way in log terms.
        const double step = std::clamp(std::sqrt(target_ / smoothed_), 1.0 / kMaxStep, kMaxStep);
        speed_ = std::clamp(speed_ * step, min_, max_);
        return speed_;
    }

    double speed() const { return speed_; }
    double target() const { return target_; }
    double smoothedRate() const { return smoothed_; }
    double lastMeasuredRate() const { return lastMeasured_; }

private:
    static constexpr double kMaxStep = 1.25;
    double target_;
    double speed_;
    double min_;
    double max_;
    double smoothed_ = 0;
    double lastMeasured_ = 0;
    bool primed_ = false;
};

} // namespace lt::fleet
