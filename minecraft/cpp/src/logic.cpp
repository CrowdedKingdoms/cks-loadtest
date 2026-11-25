#include "logic.hpp"

#include <algorithm>
#include <cmath>

namespace ckmc {

uint64_t seedFromName(std::string_view name) {
  uint64_t h = 14695981039346656037ULL;
  for (char c : name) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

uint64_t Rng::next() {
  state_ += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

double Rng::unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

double Rng::range(double lo, double hi) { return lo + (hi - lo) * unit(); }

int Rng::rangeInt(int lo, int hi) {
  if (hi <= lo) return lo;
  auto span = static_cast<uint64_t>(hi - lo) + 1;
  return lo + static_cast<int>(next() % span);
}

void Wander::reset(Rng& rng) {
  heading = rng.range(0.0, 2.0 * kPi);
  hold = rng.rangeInt(kWanderHoldMinTicks, kWanderHoldMaxTicks);
}

void Wander::turn(Rng& rng) {
  double by = rng.range(kWanderMinTurn, kWanderMaxTurn);
  if (rng.next() & 1) by = -by;
  // The heading can be negative after a mirror at an edge. Keep it in [0, 2 pi).
  heading = std::fmod(heading + by + 2.0 * kPi, 2.0 * kPi);
  if (heading < 0) heading += 2.0 * kPi;
  hold = rng.rangeInt(kWanderHoldMinTicks, kWanderHoldMaxTicks);
}

void Wander::step(Rng& rng, const WanderBox& box, double x, double z, double stepLen, double& dx, double& dz) {
  dx = 0;
  dz = 0;
  if (box.enabled && !inBox(box, x, z)) {
    double insetX = std::min(kWanderInset, (box.maxX - box.minX) / 2.0);
    double insetZ = std::min(kWanderInset, (box.maxZ - box.minZ) / 2.0);
    double tx = std::clamp(x, box.minX + insetX, box.maxX - insetX);
    double tz = std::clamp(z, box.minZ + insetZ, box.maxZ - insetZ);
    double ddx = tx - x;
    double ddz = tz - z;
    double dist = std::hypot(ddx, ddz);
    if (dist <= 0.0) return;
    heading = std::atan2(ddz, ddx);
    double len = std::min(stepLen, dist);
    dx = ddx / dist * len;
    dz = ddz / dist * len;
    return;
  }
  hold--;
  if (hold <= 0) turn(rng);
  dx = std::cos(heading) * stepLen;
  dz = std::sin(heading) * stepLen;
  if (!box.enabled) return;
  bool mirrored = false;
  if (x + dx < box.minX || x + dx > box.maxX) {
    dx = -dx;
    mirrored = true;
  }
  if (z + dz < box.minZ || z + dz > box.maxZ) {
    dz = -dz;
    mirrored = true;
  }
  if (!mirrored) return;
  heading = std::atan2(dz, dx);
  // A box narrower than two steps: the mirrored step can leave by the other edge.
  if (x + dx < box.minX) dx = box.minX - x;
  else if (x + dx > box.maxX) dx = box.maxX - x;
  if (z + dz < box.minZ) dz = box.minZ - z;
  else if (z + dz > box.maxZ) dz = box.maxZ - z;
}

double rampAltitude(double current, double target, double maxStep) {
  double delta = target - current;
  if (std::fabs(delta) <= maxStep) return target;
  return current + (delta > 0 ? maxStep : -maxStep);
}

double rejoinBaseSeconds(int attempt) {
  if (attempt <= 1) return 2.0;
  if (attempt == 2) return 4.0;
  if (attempt == 3) return 8.0;
  if (attempt == 4) return 16.0;
  return 30.0;
}

int rejoinDelayTicks(int attempt, double unit) {
  double u = std::clamp(unit, 0.0, 1.0);
  return static_cast<int>(std::lround(rejoinBaseSeconds(attempt) * 20.0 * (0.75 + 0.5 * u)));
}

int gridCell(const Grid& grid, double x, double z) {
  if (!grid.enabled || !(grid.cell > 0.0) || grid.cols <= 0 || grid.rows <= 0) return -1;
  double col = std::floor((x - grid.originX) / grid.cell);
  double row = std::floor((z - grid.originZ) / grid.cell);
  if (!(col >= 0.0) || !(row >= 0.0) || col >= grid.cols || row >= grid.rows) return -1;
  return static_cast<int>(row) * grid.cols + static_cast<int>(col);
}

std::string formatStats(const FleetStats& s) {
  std::string out = "ok";
  auto field = [&out](const char* key, uint64_t value) {
    out += ' ';
    out += key;
    out += '=';
    out += std::to_string(value);
  };
  field("total", s.total);
  field("connecting", s.connecting);
  field("login", s.login);
  field("config", s.config);
  field("play", s.play);
  field("placed", s.placed);
  field("closed", s.closed);
  field("transfers", s.transfers);
  field("rejoins", s.rejoins);
  field("deaths", s.deaths);
  field("respawns", s.respawns);
  field("crossings", s.crossings);
  field("tick_ms_max", s.tickMsMax);
  return out;
}

}  // namespace ckmc
