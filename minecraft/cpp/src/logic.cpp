#include "logic.hpp"

#include "buf.hpp"

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

void JavaRandom::setSeed(int64_t seed) {
  seed_ = (static_cast<uint64_t>(seed) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
}

int JavaRandom::next(int bits) {
  seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
  return static_cast<int>(seed_ >> (48 - bits));
}

bool JavaRandom::nextBoolean() { return next(1) != 0; }

double JavaRandom::nextDouble() {
  auto hi = static_cast<int64_t>(next(26));
  auto lo = static_cast<int64_t>(next(27));
  return static_cast<double>((hi << 27) + lo) / static_cast<double>(1ULL << 53);
}

int JavaRandom::nextInt(int bound) {
  if (bound <= 0) return 0;
  int r = next(31);
  int m = bound - 1;
  if ((bound & m) == 0) return static_cast<int>((static_cast<int64_t>(bound) * r) >> 31);
  // Java's rejection loop. The compare is a signed 32-bit subtract, so it wraps.
  for (int u = r;;) {
    r = u % bound;
    auto diff = static_cast<int32_t>(static_cast<uint32_t>(u) - static_cast<uint32_t>(r) + static_cast<uint32_t>(m));
    if (diff >= 0) return r;
    u = next(31);
  }
}

int32_t javaStringHash(std::string_view name) {
  int32_t h = 0;
  for (unsigned char c : name) h = static_cast<int32_t>(static_cast<uint32_t>(h) * 31u + c);
  return h;
}

float nextJavaWanderYaw(float previous, JavaRandom& rng) {
  double mag = 30.0 + rng.nextDouble() * (180.0 - 30.0);
  double signedTurn = rng.nextBoolean() ? mag : -mag;
  float yaw = previous + static_cast<float>(signedTurn);
  yaw = std::fmod(yaw, 360.f);
  if (yaw <= -180.f) yaw += 360.f;
  if (yaw > 180.f) yaw -= 360.f;
  return yaw;
}

void javaWanderDelta(float yawDeg, double blocksPerTick, double& dx, double& dz) {
  double yawRad = static_cast<double>(yawDeg) * kPi / 180.0;
  dx = -std::sin(yawRad) * blocksPerTick;
  dz = std::cos(yawRad) * blocksPerTick;
}

bool JavaWander::tick(JavaRandom& rng, double blocksPerTick, double& dx, double& dz) {
  bool opened = legTicks <= 0;
  if (opened) {
    yaw = nextJavaWanderYaw(yaw, rng);
    legTicks = 160 + rng.nextInt(240);
  }
  legTicks--;
  javaWanderDelta(yaw, blocksPerTick, dx, dz);
  return opened;
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

bool motionColumnFeet(const uint64_t* data, int count, int localX, int localZ, int& feetY) {
  if (data == nullptr || count != kHeightmapLongs) return false;
  if (localX < 0 || localX > 15 || localZ < 0 || localZ > 15) return false;
  int index = localX + localZ * 16;
  int valuesPerLong = 64 / kHeightmapBits;
  int cell = index / valuesPerLong;
  int bit = (index - cell * valuesPerLong) * kHeightmapBits;
  uint64_t mask = (1ULL << kHeightmapBits) - 1ULL;
  int raw = static_cast<int>((data[cell] >> bit) & mask);
  if (raw <= 0) return false;
  feetY = raw + kOverworldMinY;
  return true;
}

void approachGround(double& y, double& vy, double feet) {
  if (y > feet + 1e-6) {
    vy = (vy - 0.08) * 0.98;
    if (vy < -3.92) vy = -3.92;
    y += vy;
    if (y <= feet) {
      y = feet;
      vy = 0;
    }
    return;
  }
  if (y < feet - 1e-6) {
    vy = 0;
    y = rampAltitude(y, feet);
    return;
  }
  y = feet;
  vy = 0;
}

bool readChunkGround(const uint8_t* body, size_t len, ChunkGround& out) {
  out = {};
  out.feet.fill(kNoColumn);
  Buf b = bufFrom(body, len);
  out.x = b.rI32();
  out.z = b.rI32();
  uint32_t maps = b.rVar();
  if (!b.ok || maps > 16) return false;
  uint64_t packed[kHeightmapLongs];
  bool found = false;
  for (uint32_t i = 0; i < maps && b.ok; i++) {
    uint32_t type = b.rVar();
    uint32_t n = b.rVar();
    if (!b.ok || n > 64) return false;
    if (type == static_cast<uint32_t>(kMotionBlockingId) && n == static_cast<uint32_t>(kHeightmapLongs)) {
      for (int j = 0; j < kHeightmapLongs; j++) packed[j] = static_cast<uint64_t>(b.rI64());
      found = b.ok;
    } else {
      for (uint32_t j = 0; j < n; j++) b.rI64();
    }
  }
  if (!b.ok || !found) return false;
  for (int i = 0; i < kHeightmapColumns; i++) {
    int feet = 0;
    if (motionColumnFeet(packed, kHeightmapLongs, i & 15, i >> 4, feet)) {
      out.feet[static_cast<size_t>(i)] = static_cast<int16_t>(feet);
    }
  }
  out.ok = true;
  return true;
}

}  // namespace ckmc
