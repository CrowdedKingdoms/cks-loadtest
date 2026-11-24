#include "bot.hpp"

#include "buf.hpp"
#include "../include/PacketIds.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>

namespace ckmc {
namespace {

std::mutex gLogMu;

constexpr double kEastAt = 2008.0;
constexpr double kWestAt = 1991.0;
constexpr double kEastTarget = 2013.0;
constexpr double kWestTarget = 1986.0;
constexpr double kStep = 0.21;
constexpr int kPauseTicks = 80;
constexpr int kStallTicks = 200;
constexpr double kArrive = 0.5;

float yawOf(double dx, double dz) {
  if (dx == 0.0 && dz == 0.0) return -90.f;
  return static_cast<float>(std::atan2(-dx, dz) * 180.0 / 3.14159265358979323846);
}

std::string f3(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.4f", v);
  return b;
}

}  // namespace

Bot::Bot(BotSpec spec) : spec_(std::move(spec)) {
  maxTicks_ = std::max(20, spec_.maxSeconds * 20);
  if (spec_.path.empty()) spec_.path = "seam";
  if (spec_.clearance < 1.0) spec_.clearance = 1.0;
  parseWaypoints();
  std::mt19937 rng(static_cast<uint32_t>(std::hash<std::string>{}(spec_.name)));
  std::uniform_real_distribution<double> turn(0.0, 6.283185307179586);
  wanderHeading_ = turn(rng);
  if (!spec_.logPath.empty()) log_ = std::fopen(spec_.logPath.c_str(), "a");
}

Bot::~Bot() {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
  if (log_) {
    std::fclose(log_);
    log_ = nullptr;
  }
}

void Bot::parseWaypoints() {
  if (spec_.path != "triangle" || spec_.waypoints.empty() || spec_.waypoints == "-") return;
  std::stringstream ss(spec_.waypoints);
  std::string item;
  while (std::getline(ss, item, ';')) {
    if (item.empty()) continue;
    Waypoint w;
    std::stringstream ps(item);
    std::string xs, ys, zs, side;
    if (!std::getline(ps, xs, ',')) continue;
    if (!std::getline(ps, ys, ',')) continue;
    if (!std::getline(ps, zs, ',')) continue;
    std::getline(ps, side, ',');
    w.x = std::strtod(xs.c_str(), nullptr);
    w.z = std::strtod(zs.c_str(), nullptr);
    w.side = side.empty() ? "east" : side;
    waypoints_.push_back(w);
  }
}

void Bot::say(const std::string& line) {
  std::lock_guard<std::mutex> lock(gLogMu);
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  if (!log_) return;
  std::fputs(line.c_str(), log_);
  std::fputc('\n', log_);
  std::fflush(log_);
}

void Bot::fail(const std::string& why) {
  if (dead) return;
  say(spec_.name + " closed: " + why);
  stop(why);
}

void Bot::stop(const std::string& why) {
  (void)why;
  dead = true;
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    fd = -1;
  }
}

bool Bot::start() {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* res = nullptr;
  std::string port = std::to_string(spec_.port);
  if (getaddrinfo(spec_.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    say(spec_.name + " closed: resolve " + spec_.host);
    dead = true;
    return false;
  }
  int sock = -1;
  for (addrinfo* it = res; it; it = it->ai_next) {
    sock = ::socket(it->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, it->ai_protocol);
    if (sock < 0) continue;
    int one = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rc = ::connect(sock, it->ai_addr, it->ai_addrlen);
    if (rc == 0 || errno == EINPROGRESS) {
      fd = sock;
      break;
    }
    ::close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    say(spec_.name + " closed: connect " + spec_.host);
    dead = true;
    return false;
  }
  say(spec_.name + " connecting to " + spec_.host + ":" + port);
  return true;
}

void Bot::send(int packetId, const std::vector<uint8_t>& body) {
  if (dead || fd < 0) return;
  Buf payload;
  payload.b = body;
  try {
    auto frame = encodeFrame(compression_, packetId, payload);
    out_.insert(out_.end(), frame.begin(), frame.end());
  } catch (const std::exception& e) {
    fail(std::string("encode: ") + e.what());
  }
}

void Bot::sendEmpty(int packetId) { send(packetId, {}); }

void Bot::sendAbilities() {
  // FLAG_FLYING is bit 1 (value 2). That is the bit HeightmapSnap treats as abilities.flying.
  send(pid(SbPlay::PLAYER_ABILITIES), std::vector<uint8_t>{0x02});
  sentAbilities_ = true;
}

void Bot::enterPlay() {
  sendEmpty(pid(SbConfig::FINISH_CONFIGURATION));
  phase_ = Phase::Play;
  sentAbilities_ = false;
}

void Bot::beginConfigFromPlay() {
  say(spec_.name + " reconfigured on the same connection");
  sendEmpty(pid(SbPlay::CONFIGURATION_ACKNOWLEDGED));
  phase_ = Phase::Config;
  placed_ = false;
}

void Bot::onWritable() {
  if (dead || fd < 0) return;
  if (!established_) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      fail("connect failed");
      return;
    }
    established_ = true;
    auto uuid = offlineUuid(spec_.name);
    Buf hello;
    hello.wUtf(spec_.name);
    hello.wRaw(uuid.data(), uuid.size());
    Buf handshake;
    handshake.wVar(static_cast<uint32_t>(kProtocolVersion));
    handshake.wUtf(spec_.host);
    handshake.wU16(static_cast<uint16_t>(spec_.port));
    handshake.wVar(2);
    // Handshake is uncompressed and uses its own id, then login hello on the same socket.
    compression_ = -1;
    send(pid(SbHandshake::CLIENT_INTENTION), handshake.b);
    send(pid(SbLogin::HELLO), hello.b);
  }
  while (!out_.empty()) {
    ssize_t n = ::send(fd, out_.data(), out_.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      fail("send");
      return;
    }
    out_.erase(out_.begin(), out_.begin() + n);
  }
}

void Bot::onReadable() {
  if (dead || fd < 0) return;
  uint8_t tmp[65536];
  while (true) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) {
      fail("connection closed");
      return;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      fail("recv");
      return;
    }
    in_.insert(in_.end(), tmp, tmp + n);
    if (n < static_cast<ssize_t>(sizeof(tmp))) break;
  }
  while (!dead) {
    std::vector<uint8_t> packet;
    std::string err;
    size_t before = in_.size();
    if (!decodeFrame(in_, compression_, packet, err)) {
      if (!err.empty()) fail(err);
      return;
    }
    if (in_.size() == before) return;
    Buf body;
    int id = packetIdOf(packet, body);
    if (id < 0 || !body.ok) {
      fail("bad packet id");
      return;
    }
    handle(id, body.b.data(), body.b.size());
  }
}

void Bot::handle(int packetId, const uint8_t* body, size_t len) {
  if (phase_ == Phase::Login) handleLogin(packetId, body, len);
  else if (phase_ == Phase::Config) handleConfig(packetId, body, len);
  else handlePlay(packetId, body, len);
}

void Bot::handleLogin(int packetId, const uint8_t* body, size_t len) {
  if (packetId == pid(CbLogin::LOGIN_COMPRESSION)) {
    Buf b = bufFrom(body, len);
    compression_ = static_cast<int>(b.rVar());
    if (!b.ok) fail("compression");
    return;
  }
  if (packetId == pid(CbLogin::HELLO)) {
    fail("encryption requested");
    return;
  }
  if (packetId == pid(CbLogin::LOGIN_DISCONNECT)) {
    fail("login disconnect");
    return;
  }
  if (packetId == pid(CbLogin::CUSTOM_QUERY)) {
    Buf b = bufFrom(body, len);
    uint32_t tx = b.rVar();
    if (!b.ok) return;
    Buf ans;
    ans.wVar(tx);
    ans.wBool(false);
    send(pid(SbLogin::CUSTOM_QUERY_ANSWER), ans.b);
    return;
  }
  if (packetId == pid(CbLogin::COOKIE_REQUEST)) {
    Buf b = bufFrom(body, len);
    std::string key = b.rUtf();
    Buf ans;
    ans.wUtf(key);
    ans.wBool(false);
    send(pid(SbLogin::COOKIE_RESPONSE), ans.b);
    return;
  }
  if (packetId == pid(CbLogin::LOGIN_FINISHED)) {
    sendEmpty(pid(SbLogin::LOGIN_ACKNOWLEDGED));
    phase_ = Phase::Config;
    Buf brand;
    brand.wUtf("minecraft:brand");
    brand.wUtf("vanilla");
    send(pid(SbConfig::CUSTOM_PAYLOAD), brand.b);
    Buf info;
    info.wUtf("en_us");
    info.wU8(2);
    info.wVar(0);
    info.wBool(true);
    info.wU8(0);
    info.wVar(1);
    info.wBool(false);
    info.wBool(false);
    info.wVar(0);
    send(pid(SbConfig::CLIENT_INFORMATION), info.b);
  }
}

void Bot::handleConfig(int packetId, const uint8_t* body, size_t len) {
  if (packetId == pid(CbConfig::COOKIE_REQUEST)) {
    Buf b = bufFrom(body, len);
    std::string key = b.rUtf();
    Buf ans;
    ans.wUtf(key);
    auto it = std::find_if(cookies_.begin(), cookies_.end(), [&](auto& kv) { return kv.first == key; });
    if (it == cookies_.end()) {
      ans.wBool(false);
    } else {
      ans.wBool(true);
      ans.wVar(static_cast<uint32_t>(it->second.size()));
      ans.wRaw(reinterpret_cast<const uint8_t*>(it->second.data()), it->second.size());
    }
    send(pid(SbConfig::COOKIE_RESPONSE), ans.b);
    return;
  }
  if (packetId == pid(CbConfig::STORE_COOKIE)) {
    Buf b = bufFrom(body, len);
    std::string key = b.rUtf();
    uint32_t n = b.rVar();
    if (!b.ok || n > b.left()) return;
    std::string payload(reinterpret_cast<const char*>(b.b.data() + b.r), n);
    cookies_.push_back({key, payload});
    return;
  }
  if (packetId == pid(CbConfig::DISCONNECT)) {
    fail("config disconnect");
    return;
  }
  if (packetId == pid(CbConfig::KEEP_ALIVE)) {
    Buf b = bufFrom(body, len);
    int64_t id = b.rI64();
    if (!b.ok) return;
    Buf ans;
    ans.wI64(id);
    send(pid(SbConfig::KEEP_ALIVE), ans.b);
    return;
  }
  if (packetId == pid(CbConfig::PING)) {
    Buf b = bufFrom(body, len);
    int32_t id = b.rI32();
    if (!b.ok) return;
    Buf ans;
    ans.wI32(id);
    send(pid(SbConfig::PONG), ans.b);
    return;
  }
  if (packetId == pid(CbConfig::RESOURCE_PACK_PUSH)) {
    if (len < 16) return;
    Buf ans;
    ans.wRaw(body, 16);
    ans.wVar(3);  // ACCEPTED
    send(pid(SbConfig::RESOURCE_PACK), ans.b);
    Buf down;
    down.wRaw(body, 16);
    down.wVar(4);  // DOWNLOADED
    send(pid(SbConfig::RESOURCE_PACK), down.b);
    Buf loaded;
    loaded.wRaw(body, 16);
    loaded.wVar(0);  // SUCCESSFULLY_LOADED
    send(pid(SbConfig::RESOURCE_PACK), loaded.b);
    return;
  }
  if (packetId == pid(CbConfig::SELECT_KNOWN_PACKS)) {
    Buf ans;
    ans.wVar(0);
    send(pid(SbConfig::SELECT_KNOWN_PACKS), ans.b);
    return;
  }
  if (packetId == pid(CbConfig::CODE_OF_CONDUCT)) {
    sendEmpty(pid(SbConfig::ACCEPT_CODE_OF_CONDUCT));
    return;
  }
  if (packetId == pid(CbConfig::TRANSFER)) {
    Buf b = bufFrom(body, len);
    std::string host = b.rUtf();
    uint32_t port = b.rVar();
    transfer_ = true;
    say("TRANSFER FAILURE " + spec_.name + " ClientboundTransferPacket to " + host + ":" + std::to_string(port));
    stop("transfer");
    return;
  }
  if (packetId == pid(CbConfig::FINISH_CONFIGURATION)) {
    enterPlay();
  }
}

void Bot::handlePlay(int packetId, const uint8_t* body, size_t len) {
  if (packetId == pid(CbPlay::BUNDLE_DELIMITER)) return;
  if (packetId == pid(CbPlay::KEEP_ALIVE)) {
    Buf b = bufFrom(body, len);
    int64_t id = b.rI64();
    if (!b.ok) return;
    Buf ans;
    ans.wI64(id);
    send(pid(SbPlay::KEEP_ALIVE), ans.b);
    return;
  }
  if (packetId == pid(CbPlay::PING)) {
    Buf b = bufFrom(body, len);
    int32_t id = b.rI32();
    if (!b.ok) return;
    Buf ans;
    ans.wI32(id);
    send(pid(SbPlay::PONG), ans.b);
    return;
  }
  if (packetId == pid(CbPlay::CHUNK_BATCH_FINISHED)) {
    Buf ans;
    ans.wF32(20.f);
    send(pid(SbPlay::CHUNK_BATCH_RECEIVED), ans.b);
    return;
  }
  if (packetId == pid(CbPlay::LOGIN)) {
    sendEmpty(pid(SbPlay::PLAYER_LOADED));
    say(spec_.name + " login packet, waiting for a position");
    return;
  }
  if (packetId == pid(CbPlay::PLAYER_POSITION)) {
    onPosition(body, len);
    return;
  }
  if (packetId == pid(CbPlay::START_CONFIGURATION)) {
    beginConfigFromPlay();
    return;
  }
  if (packetId == pid(CbPlay::TRANSFER)) {
    Buf b = bufFrom(body, len);
    std::string host = b.rUtf();
    uint32_t port = b.rVar();
    transfer_ = true;
    say("TRANSFER FAILURE " + spec_.name + " ClientboundTransferPacket to " + host + ":" + std::to_string(port));
    stop("transfer");
    return;
  }
  if (packetId == pid(CbPlay::DISCONNECT)) {
    fail("play disconnect");
  }
}

void Bot::onPosition(const uint8_t* body, size_t len) {
  Buf b = bufFrom(body, len);
  uint32_t teleportId = b.rVar();
  double px = b.rF64();
  double py = b.rF64();
  double pz = b.rF64();
  double _dx = b.rF64();
  double _dy = b.rF64();
  double _dz = b.rF64();
  (void)_dx;
  (void)_dy;
  (void)_dz;
  float pyaw = b.rF32();
  float ppitch = b.rF32();
  int32_t rel = b.rI32();
  if (!b.ok) {
    say(spec_.name + " position packet short");
    return;
  }
  double nx = (rel & 1) ? x_ + px : px;
  double ny = (rel & 2) ? surfaceY_ + py : py;
  double nz = (rel & 4) ? z_ + pz : pz;
  if (rel & 8) yaw_ = yaw_ + pyaw;
  else yaw_ = pyaw;
  if (rel & 16) pitch_ = pitch_ + ppitch;
  else pitch_ = ppitch;

  bool yanked = placed_ && ny < surfaceY_ + spec_.clearance - 2.0;
  surfaceY_ = ny;
  x_ = nx;
  z_ = nz;
  y_ = surfaceY_ + spec_.clearance;
  if (!progressSet_) {
    progressX_ = nx;
    progressSet_ = true;
  }
  bool toward = direction_ > 0 ? nx > progressX_ + 0.05 : nx < progressX_ - 0.05;
  if (toward) {
    progressX_ = nx;
    stallTicks_ = 0;
    stuck_ = 0;
  }
  serverX_ = nx;
  placed_ = true;
  if (yanked) sentAbilities_ = false;

  Buf ans;
  ans.wVar(teleportId);
  ans.wF64(nx);
  ans.wF64(ny);
  ans.wF64(nz);
  ans.wF32(yaw_);
  ans.wF32(pitch_);
  send(pid(SbPlay::ACCEPT_TELEPORTATION), ans.b);
  say(spec_.name + " at " + f3(nx) + " " + f3(ny) + " " + f3(nz));
  if (!sentAbilities_) sendAbilities();
}

void Bot::sendFlyPose(bool withInput) {
  Buf move;
  move.wF64(x_);
  move.wF64(y_);
  move.wF64(z_);
  move.wF32(yaw_);
  move.wF32(pitch_);
  move.wU8(0);
  send(pid(SbPlay::MOVE_PLAYER_POS_ROT), move.b);
  if (withInput) {
    bool sprint = (ticks_ / 60) % 2 == 0;
    uint8_t bits = 0x01;
    if (sprint) bits = static_cast<uint8_t>(bits | 0x40);
    send(pid(SbPlay::PLAYER_INPUT), std::vector<uint8_t>{bits});
  } else {
    send(pid(SbPlay::PLAYER_INPUT), std::vector<uint8_t>{0});
  }
  if (ticks_ % 40 == 0) sendEmpty(pid(SbPlay::PUNCH));
  sendEmpty(pid(SbPlay::CLIENT_TICK_END));
}

void Bot::tickMotion() {
  if (hold_ > 0) {
    hold_--;
    if (waypoints_.empty()) yaw_ = direction_ > 0 ? -90.f : 90.f;
    sendFlyPose(false);
    if (hold_ == 0) {
      std::string side = pauseLabel_;
      int count = 0;
      if (side.empty()) {
        if (pauseSide_ > 0) {
          side = "east";
          eastPauses_++;
          count = eastPauses_;
        } else if (pauseSide_ < 0) {
          side = "west";
          westPauses_++;
          count = westPauses_;
        }
      } else if (side == "east") {
        eastPauses_++;
        count = eastPauses_;
      } else if (side == "west") {
        westPauses_++;
        count = westPauses_;
      } else if (side == "north") {
        northPauses_++;
        count = northPauses_;
      } else {
        count = 1;
      }
      if (!side.empty()) {
        say(spec_.name + " " + side + " pause done " + side + "_pauses=" + std::to_string(count));
      }
      if (!waypoints_.empty()) {
        waypointIndex_ = (waypointIndex_ + 1) % static_cast<int>(waypoints_.size());
      }
      pauseLabel_.clear();
      pauseSide_ = 0;
    }
    return;
  }

  auto beginPause = [&](const std::string& side, int sideCode) {
    pauseLabel_ = side;
    pauseSide_ = sideCode;
    hold_ = kPauseTicks;
    say(spec_.name + " " + side + " pause start x=" + f3(x_) + " z=" + f3(z_));
    sendEmpty(pid(SbPlay::CLIENT_TICK_END));
  };

  if (spec_.path == "wander") {
    double step = std::max(0.05, spec_.bps) / 20.0;
    if (ticks_ % 100 == 0) {
      std::mt19937 rng(static_cast<uint32_t>(std::hash<std::string>{}(spec_.name) ^ static_cast<uint32_t>(ticks_)));
      std::uniform_real_distribution<double> turn(-1.0, 1.0);
      wanderHeading_ += turn(rng);
    }
    double dx = std::cos(wanderHeading_) * step;
    double dz = std::sin(wanderHeading_) * step;
    x_ += dx;
    z_ += dz;
    yaw_ = yawOf(dx, dz);
    y_ = surfaceY_ + spec_.clearance;
    sendFlyPose(true);
    return;
  }

  if (!waypoints_.empty()) {
    const Waypoint& target = waypoints_[static_cast<size_t>(waypointIndex_)];
    double dx = target.x - x_;
    double dz = target.z - z_;
    double dist = std::hypot(dx, dz);
    if (dist <= kArrive) {
      beginPause(target.side, target.side == "west" ? -1 : target.side == "north" ? 2 : 1);
      return;
    }
    double scale = std::min(kStep, dist) / dist;
    x_ += dx * scale;
    z_ += dz * scale;
    yaw_ = yawOf(dx, dz);
    y_ = surfaceY_ + spec_.clearance;
    sendFlyPose(true);
    return;
  }

  double targetX = direction_ > 0 ? kEastTarget : kWestTarget;
  if (direction_ > 0 && x_ >= targetX) {
    direction_ = -1;
    escaping_ = false;
    beginPause("east", 1);
    return;
  }
  if (direction_ < 0 && x_ <= targetX) {
    direction_ = 1;
    escaping_ = false;
    beginPause("west", -1);
    return;
  }
  stallTicks_++;
  if (stallTicks_ >= kStallTicks) {
    stuck_++;
    laneZ_ = z_ + (stuck_ % 2 == 0 ? 8.0 : -8.0);
    escaping_ = true;
    stallTicks_ = 0;
    say(spec_.name + " blocked at x=" + f3(serverX_));
  }
  double nx = x_;
  double nz = z_;
  if (escaping_) {
    double dz = laneZ_ - z_;
    if (std::fabs(dz) <= kStep) {
      escaping_ = false;
      nz = laneZ_;
      nx = x_ + direction_ * kStep;
    } else {
      nz = z_ + std::copysign(kStep, dz);
    }
    yaw_ = yawOf(nx - x_, nz - z_);
  } else {
    nx = x_ + direction_ * kStep;
    yaw_ = direction_ > 0 ? -90.f : 90.f;
  }
  x_ = nx;
  z_ = nz;
  y_ = surfaceY_ + spec_.clearance;
  sendFlyPose(true);
}

void Bot::tick() {
  if (dead || !established_) return;
  ticks_++;
  if (ticks_ > maxTicks_) {
    say(spec_.name + " max runtime (~" + std::to_string(spec_.maxSeconds) + "s)");
    stop("max runtime");
    return;
  }
  if (phase_ != Phase::Play || !placed_ || transfer_) {
    if (phase_ == Phase::Play && !transfer_) sendEmpty(pid(SbPlay::CLIENT_TICK_END));
    return;
  }
  if (!sentAbilities_) sendAbilities();
  tickMotion();
  if (ticks_ % 40 == 0) {
    say(spec_.name + " phase=play placed=1 x=" + f3(x_) + " y=" + f3(y_) + " z=" + f3(z_) +
        " walkY=" + f3(surfaceY_) + " dir=" + (direction_ > 0 ? "east" : "west") +
        (escaping_ ? " lane" : "") + " east_pauses=" + std::to_string(eastPauses_) +
        " west_pauses=" + std::to_string(westPauses_) + " north_pauses=" + std::to_string(northPauses_) +
        " transfer=0 path=" + spec_.path);
  }
}

}  // namespace ckmc
