#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The parts of a bot that need no socket: wander headings, the altitude ramp, the rejoin
// schedule, grid cells, and the `stats` reply. protocol_test steps these directly.
namespace ckmc {

inline constexpr double kPi = 3.14159265358979323846;

// Rectangle a wandering bot stays in. Not enabled leaves wander unbounded.
struct WanderBox {
  bool enabled = false;
  double minX = 0;
  double minZ = 0;
  double maxX = 0;
  double maxZ = 0;
};

inline bool inBox(const WanderBox& box, double x, double z) {
  return x >= box.minX && x <= box.maxX && z >= box.minZ && z <= box.maxZ;
}

// FNV-1a, 64 bit. std::hash differs between standard libraries and this must not.
uint64_t seedFromName(std::string_view name);

// splitmix64. The same name gives the same run on every platform, which the
// distributions in <random> do not promise.
class Rng {
 public:
  explicit Rng(uint64_t seed = 0) : state_(seed) {}
  uint64_t next();
  // [0, 1)
  double unit();
  // [lo, hi)
  double range(double lo, double hi);
  // lo..hi, both included
  int rangeInt(int lo, int hi);

 private:
  uint64_t state_;
};

inline constexpr int kWanderHoldMinTicks = 160;  // 8 s
inline constexpr int kWanderHoldMaxTicks = 400;  // 20 s
// A turn is 30 to 90 degrees, to either side. Never less than 30. Not more than 90, so a
// bot keeps going roughly the way it was and gets across a large box: with turns of up to
// 180 degrees it doubles back half the time and stays near where it started.
inline constexpr double kWanderMinTurn = kPi / 6.0;
inline constexpr double kWanderMaxTurn = kPi / 2.0;
// How far inside the nearest edge a bot outside its box aims.
inline constexpr double kWanderInset = 1.0;

// One bot's wander heading. `heading` is radians, the step is (cos, sin) in (x, z).
struct Wander {
  double heading = 0;
  // Ticks left on this heading.
  int hold = 0;

  void reset(Rng& rng);
  // A new heading 30 to 90 degrees from this one, and a new 8 to 20 s hold.
  void turn(Rng& rng);
  // One tick from (x, z). Sets the step to take. Inside the box the step never leaves it:
  // the heading is mirrored on the edge it would cross. Outside the box the step goes
  // straight toward the nearest spot inside.
  void step(Rng& rng, const WanderBox& box, double x, double z, double stepLen, double& dx, double& dz);
};

// java.util.Random. LoadWalker seeds it with the bot name's Java String.hashCode so the
// same name walks the same headings the Java bots walk. splitmix64 does not.
class JavaRandom {
 public:
  JavaRandom() = default;
  explicit JavaRandom(int64_t seed) { setSeed(seed); }
  void setSeed(int64_t seed);
  int next(int bits);
  bool nextBoolean();
  double nextDouble();
  // 0 .. bound-1. `bound` must be positive.
  int nextInt(int bound);

 private:
  uint64_t seed_ = 0;
};

// Java String.hashCode, 32-bit signed. ASCII names match a Java string of the same bytes.
int32_t javaStringHash(std::string_view name);

// Next Minecraft yaw, at least 30 and less than 180 degrees off `previous`, either way.
// Not aimed at a coordinate. LoadWalker.nextWanderYaw.
float nextJavaWanderYaw(float previous, JavaRandom& rng);

// One unguided step. `yawDeg` is Minecraft yaw (east -90, south 0).
void javaWanderDelta(float yawDeg, double blocksPerTick, double& dx, double& dz);

// LoadWalker's wander. yaw starts at 0 and legTicks at 0, so the first tick picks a heading.
// A leg then lasts 160 + nextInt(240) ticks: 8 s to just under 20 s.
struct JavaWander {
  float yaw = 0.f;
  int legTicks = 0;

  // Writes this tick's step. True when the tick opened a new heading.
  bool tick(JavaRandom& rng, double blocksPerTick, double& dx, double& dz);
};

inline constexpr double kMaxClimbPerTick = 0.4;

// The next Y on the way to `target`, at most `maxStep` from `current`.
double rampAltitude(double current, double target, double maxStep = kMaxClimbPerTick);

inline constexpr int kRejoinMaxAttempts = 10;
// 120 s of play on one connection clears the attempts used.
inline constexpr int kRejoinResetTicks = 2400;

// 2, 4, 8, 16, then 30 s. `attempt` starts at 1.
double rejoinBaseSeconds(int attempt);
// The base delay, jittered by 25% either way, in ticks. `unit` is in [0, 1).
int rejoinDelayTicks(int attempt, double unit);

// The mesh as a grid of square cells, one per server. Cell (0, 0) starts at the origin.
struct Grid {
  bool enabled = false;
  double originX = 0;
  double originZ = 0;
  double cell = 0;
  int cols = 0;
  int rows = 0;
};

// row * cols + col, or -1 when the grid is off or the position is outside it.
int gridCell(const Grid& grid, double x, double z);

// The cell a bot was last seen in on its current connection.
struct CellTracker {
  int cell = -1;

  void reset() { cell = -1; }
  // True when `next` is a cell and is not the last cell seen. `from` is that last cell.
  // A position outside the grid (-1) changes nothing.
  bool update(int next, int& from) {
    if (next < 0) return false;
    if (cell < 0) {
      cell = next;
      return false;
    }
    if (next == cell) return false;
    from = cell;
    cell = next;
    return true;
  }
};

inline constexpr int kStatusTicks = 600;  // 30 s

// True on the tick a bot prints its `phase=play` line. `offset` spreads the bots out.
inline bool statusDue(int ticks, int offset) { return (ticks + offset) % kStatusTicks == 0; }

struct FleetStats {
  uint64_t total = 0;
  uint64_t connecting = 0;
  uint64_t login = 0;
  uint64_t config = 0;
  uint64_t play = 0;
  uint64_t placed = 0;
  uint64_t closed = 0;
  uint64_t transfers = 0;
  uint64_t rejoins = 0;
  uint64_t deaths = 0;
  uint64_t respawns = 0;
  uint64_t crossings = 0;
  uint64_t tickMsMax = 0;
};

// The `stats` reply, without the newline.
std::string formatStats(const FleetStats& stats);

}  // namespace ckmc
