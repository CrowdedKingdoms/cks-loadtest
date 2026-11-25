#include "bot.hpp"

#include "buf.hpp"
#include "journal.hpp"
#include "../include/PacketIds.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ckmc {

Counters& counters() {
  static Counters totals;
  return totals;
}

std::vector<uint8_t> clientCommandBody(int action) {
  Buf b;
  b.wVar(static_cast<uint32_t>(action));
  return b.b;
}

namespace {

[[maybe_unused]] constexpr double kEastAt = 2008.0;
[[maybe_unused]] constexpr double kWestAt = 1991.0;
constexpr double kEastTarget = 2013.0;
constexpr double kWestTarget = 1986.0;
constexpr double kStep = 0.21;
constexpr int kPauseTicks = 80;
constexpr int kStallTicks = 200;
constexpr double kArrive = 0.5;
// A dead bot asks to respawn again this often until the server answers.
constexpr int kRespawnAskTicks = 40;
// After a respawn packet the bot waits this long for a position, then flies on from where it was.
constexpr int kRespawnWaitTicks = 60;
// At most one `crossed` line per bot in this many ticks.
constexpr int kCrossLogTicks = 40;
// A server that stops reading leaves the bot's packets here. Past this the connection is lost.
constexpr size_t kMaxOutBytes = 1024 * 1024;
// One readable event reads at most this many 64 KB blocks, so one busy socket cannot hold the worker.
constexpr int kMaxReadRounds = 64;
constexpr size_t kInShrinkBytes = 256 * 1024;

const Shared& defaultShared() {
  static const Shared none{};
  return none;
}

float yawOf(double dx, double dz) {
  if (dx == 0.0 && dz == 0.0) return -90.f;
  return static_cast<float>(std::atan2(-dx, dz) * 180.0 / kPi);
}

std::string f3(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.4f", v);
  return b;
}

}  // namespace

Bot::Bot(BotSpec spec, const Shared* shared, std::shared_ptr<BotView> view)
    : spec_(std::move(spec)),
      shared_(shared ? shared : &defaultShared()),
      view_(std::move(view)),
      rng_(seedFromName(spec_.name)) {
  origHost_ = spec_.host;
  origPort_ = spec_.port;
  long long ticks = static_cast<long long>(spec_.maxSeconds) * 20;
  maxTicks_ = static_cast<int>(std::clamp<long long>(ticks, 20, 2000000000LL));
  if (spec_.path.empty()) spec_.path = "seam";
  if (spec_.clearance < 1.0) spec_.clearance = 1.0;
  parseWaypoints();
  parseWanderBox();
  wander_.reset(rng_);
  javaRng_.setSeed(javaStringHash(spec_.name));
  statusOffset_ = static_cast<int>(seedFromName(spec_.name) % static_cast<uint64_t>(kStatusTicks));
  sink_ = Journal::get().openSink(spec_.logPath);
}

Bot::~Bot() {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void Bot::parseWanderBox() {
  // LoadWalker.parseWaypoints("wander", ...) is empty. The old rectangle mirrored the
  // heading at a cell edge, so a bot bounced inside its server instead of walking
  // Benjamin's heading. Rally still has its own box.
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

void Bot::say(const std::string& line) { Journal::get().line(sink_, line); }

void Bot::publish() {
  if (!view_) return;
  BotPhase phase = BotPhase::Connecting;
  if (dead) phase = BotPhase::Closed;
  else if (!established_) phase = BotPhase::Connecting;
  else if (phase_ == Phase::Login) phase = BotPhase::Login;
  else if (phase_ == Phase::Config) phase = BotPhase::Config;
  else phase = BotPhase::Play;
  bool placed = phase == BotPhase::Play && placed_;
  if (placed != pubPlaced_) {
    pubPlaced_ = placed;
    view_->placed.store(placed, std::memory_order_relaxed);
  }
  if (phase != pubPhase_) {
    pubPhase_ = phase;
    view_->phase.store(static_cast<uint8_t>(phase), std::memory_order_release);
  }
}

void Bot::settle() {
  if (!dead && established_ && fd >= 0 && !out_.empty()) flushOut();
  publish();
}

void Bot::fail(const std::string& why) {
  if (dead) return;
  say(spec_.name + " closed: " + why);
  stop(why);
}

void Bot::stop(const std::string& why) {
  if (dead) return;
  // The reason is in place before a control thread can see the bot as closed.
  if (view_) view_->reason = why;
  dead = true;
  rejoinAtTick_ = -1;
  closeSocket();
  publish();
}

void Bot::closeSocket() {
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    fd = -1;
  }
  armedFd = -1;
  connGen_++;
}

void Bot::resetConnection() {
  established_ = false;
  compression_ = -1;
  phase_ = Phase::Login;
  placed_ = false;
  connPlaced_ = false;
  sentAbilities_ = false;
  respawnSeen_ = false;
  posSinceDeath_ = false;
  waitPosTicks_ = 0;
  needLoaded_ = false;
  haveEntityId_ = false;
  cells_.reset();
  in_.clear();
  out_.clear();
}

void Bot::lost(const std::string& why) {
  if (dead) return;
  if (!everPlaced_) {
    fail(why);
    return;
  }
  if (rejoinAttempts_ >= kRejoinMaxAttempts) {
    fail(why + " (" + std::to_string(kRejoinMaxAttempts) + " rejoins used)");
    return;
  }
  closeSocket();
  resetConnection();
  rejoinAttempts_++;
  rejoins_++;
  rejoinAtTick_ = ticks_ + rejoinDelayTicks(rejoinAttempts_, rng_.unit());
  counters().rejoins.fetch_add(1, std::memory_order_relaxed);
  say(spec_.name + " rejoin " + std::to_string(rejoinAttempts_) + " after: " + why);
}

bool Bot::start() {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* res = nullptr;
  std::string port = std::to_string(spec_.port);
  if (getaddrinfo(spec_.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    lost("resolve " + spec_.host);
    return false;
  }
  int sock = -1;
  for (addrinfo* it = res; it; it = it->ai_next) {
#if defined(__APPLE__)
    sock = ::socket(it->ai_family, SOCK_STREAM, it->ai_protocol);
    if (sock < 0) continue;
    int fl = ::fcntl(sock, F_GETFL, 0);
    if (fl >= 0) ::fcntl(sock, F_SETFL, fl | O_NONBLOCK);
    ::fcntl(sock, F_SETFD, FD_CLOEXEC);
    int nosig = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#else
    sock = ::socket(it->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, it->ai_protocol);
    if (sock < 0) continue;
#endif
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
    lost("connect " + spec_.host);
    return false;
  }
  say(spec_.name + " connecting to " + spec_.host + ":" + port);
  return true;
}

void Bot::send(int packetId, const std::vector<uint8_t>& body) {
  if (dead || fd < 0) return;
  if (out_.size() > kMaxOutBytes) {
    lost("send buffer full");
    return;
  }
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
  waitPosTicks_ = 0;
}

void Bot::followTransfer(const std::string& host, uint32_t port) {
  if (host.empty() || port == 0 || port > 65535) {
    fail("transfer to " + host + ":" + std::to_string(port));
    return;
  }
  say(spec_.name + " transferred to " + host + ":" + std::to_string(port));
  transfersFollowed_++;
  counters().transfers.fetch_add(1, std::memory_order_relaxed);
  reconnect(host, static_cast<int>(port), 3);
}

void Bot::reconnect(const std::string& host, int port, int handshakeState) {
  closeSocket();
  resetConnection();
  spec_.host = host;
  spec_.port = port;
  handshakeState_ = handshakeState;
  if (handshakeState == 2) cookies_.clear();
  start();
}

void Bot::onWritable() {
  if (dead || fd < 0) return;
  writeSocket();
  settle();
}

void Bot::writeSocket() {
  if (!established_) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      lost("connect failed");
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
    handshake.wVar(static_cast<uint32_t>(handshakeState_));
    // Handshake is uncompressed and uses its own id, then login hello on the same socket.
    compression_ = -1;
    send(pid(SbHandshake::CLIENT_INTENTION), handshake.b);
    send(pid(SbLogin::HELLO), hello.b);
  }
  flushOut();
}

void Bot::flushOut() {
  while (!dead && fd >= 0 && !out_.empty()) {
#if defined(__APPLE__)
    ssize_t n = ::send(fd, out_.data(), out_.size(), 0);
#else
    ssize_t n = ::send(fd, out_.data(), out_.size(), MSG_NOSIGNAL);
#endif
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      lost("send failed (errno " + std::to_string(errno) + ")");
      return;
    }
    out_.erase(out_.begin(), out_.begin() + n);
  }
}

void Bot::onReadable() {
  if (dead || fd < 0) return;
  readSocket();
  settle();
}

void Bot::onSocketError() {
  if (dead || fd < 0) return;
  if (!established_) {
    lost("connect failed");
  } else {
    // Take what the server sent before it closed: a transfer or a disconnect can be in there.
    uint64_t gen = connGen_;
    readSocket();
    if (!dead && connGen_ == gen && fd >= 0) lost("socket error");
  }
  settle();
}

void Bot::readSocket() {
  // closeSocket() moves connGen_. A new socket can get the old fd number, so the number
  // alone does not say whether this is still the connection being read.
  uint64_t gen = connGen_;
  bool eof = false;
  uint8_t tmp[65536];
  for (int round = 0; round < kMaxReadRounds && !dead && connGen_ == gen; round++) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n == 0) {
      eof = true;
      break;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      lost("recv failed (errno " + std::to_string(errno) + ")");
      return;
    }
    in_.insert(in_.end(), tmp, tmp + n);
    while (!dead && connGen_ == gen) {
      std::vector<uint8_t> packet;
      std::string err;
      size_t before = in_.size();
      if (!decodeFrame(in_, compression_, packet, err)) {
        if (!err.empty()) lost(err);
        break;
      }
      if (in_.size() == before) break;
      Buf body;
      int id = packetIdOf(packet, body);
      if (id < 0 || !body.ok) {
        lost("bad packet id");
        return;
      }
      handle(id, body.b.data(), body.b.size());
    }
    if (n < static_cast<ssize_t>(sizeof(tmp))) break;
  }
  if (dead || connGen_ != gen) return;
  // A login burst of chunks can leave a large empty buffer behind. Give it back.
  if (in_.empty() && in_.capacity() > kInShrinkBytes) std::vector<uint8_t>().swap(in_);
  if (!eof) return;
  // A transfer closes this socket after the packet that names the next server.
  // That end is the handoff, not a drop, once the bot has followed it. A connection
  // made to follow a transfer that closes before play is the destination still busy; try again.
  if (phase_ != Phase::Play && handshakeState_ == 3 && transferRetries_ < 4 && !spec_.host.empty()) {
    transferRetries_++;
    reconnect(spec_.host, spec_.port, 3);
    return;
  }
  lost("connection closed");
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
    lost("login disconnect");
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
    // A server that stores the same key again replaces it, so the list cannot grow without end.
    auto it = std::find_if(cookies_.begin(), cookies_.end(), [&](auto& kv) { return kv.first == key; });
    if (it != cookies_.end()) it->second = payload;
    else cookies_.push_back({key, payload});
    return;
  }
  if (packetId == pid(CbConfig::DISCONNECT)) {
    lost("config disconnect");
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
    if (!b.ok) {
      fail("transfer packet short");
      return;
    }
    followTransfer(host, port);
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
    // The first field is this player's entity id. A combat-kill names the player by it.
    if (len >= 4) {
      Buf b = bufFrom(body, len);
      entityId_ = b.rI32();
      haveEntityId_ = b.ok;
    }
    sendEmpty(pid(SbPlay::PLAYER_LOADED));
    say(spec_.name + " login packet, waiting for a position");
    return;
  }
  if (packetId == pid(CbPlay::PLAYER_POSITION)) {
    onPosition(body, len);
    return;
  }
  if (packetId == pid(CbPlay::SET_HEALTH)) {
    // float health, VarInt food, float saturation. Only the health matters here.
    Buf b = bufFrom(body, len);
    float health = b.rF32();
    if (b.ok) onHealth(health);
    return;
  }
  if (packetId == pid(CbPlay::PLAYER_COMBAT_KILL)) {
    // VarInt player entity id, then the death message.
    Buf b = bufFrom(body, len);
    uint32_t who = b.rVar();
    if (!b.ok) return;
    if (!haveEntityId_ || static_cast<int32_t>(who) == entityId_) onDeath();
    return;
  }
  if (packetId == pid(CbPlay::RESPAWN)) {
    onRespawnPacket();
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
    if (!b.ok) {
      fail("transfer packet short");
      return;
    }
    followTransfer(host, port);
    return;
  }
  if (packetId == pid(CbPlay::DISCONNECT)) {
    lost("play disconnect");
  }
}

void Bot::onDeath() {
  if (awaitingRespawn_) return;
  awaitingRespawn_ = true;
  respawnSeen_ = false;
  posSinceDeath_ = false;
  placed_ = false;
  sentAbilities_ = false;
  waitPosTicks_ = 0;
  respawnAskTick_ = ticks_;
  deaths_++;
  counters().deaths.fetch_add(1, std::memory_order_relaxed);
  say(spec_.name + " died");
  send(pid(SbPlay::CLIENT_COMMAND), clientCommandBody(kPerformRespawn));
}

void Bot::onHealth(float health) {
  if (health <= 0.f) {
    onDeath();
    return;
  }
  if (!awaitingRespawn_ || respawnSeen_) return;
  // Alive again with no respawn packet in between, as after a rejoin that found the player alive.
  respawnSeen_ = true;
  if (posSinceDeath_) place();
  else if (connPlaced_) waitPosTicks_ = kRespawnWaitTicks;
}

void Bot::onRespawnPacket() {
  // The body (spawn info) is not read. A death respawn and a move to another dimension or
  // server both end the same way: the next position packet is where the bot is.
  placed_ = false;
  sentAbilities_ = false;
  needLoaded_ = true;
  // Only a bot that already has a position on this connection may fly on without a new one.
  waitPosTicks_ = connPlaced_ ? kRespawnWaitTicks : 0;
  if (awaitingRespawn_) {
    respawnSeen_ = true;
    posSinceDeath_ = false;
  } else {
    say(spec_.name + " respawn packet, waiting for a position");
  }
}

void Bot::place() {
  bool respawned = awaitingRespawn_;
  awaitingRespawn_ = false;
  respawnSeen_ = false;
  posSinceDeath_ = false;
  waitPosTicks_ = 0;
  placed_ = true;
  everPlaced_ = true;
  if (!connPlaced_) {
    connPlaced_ = true;
    placedAtTick_ = ticks_;
    statusNow_ = true;
  }
  if (respawned) {
    counters().respawns.fetch_add(1, std::memory_order_relaxed);
    say(spec_.name + " respawned");
  }
  if (needLoaded_) {
    needLoaded_ = false;
    sendEmpty(pid(SbPlay::PLAYER_LOADED));
  }
  if (!sentAbilities_) sendAbilities();
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

  // Pulled down from where the bot was flying: the flying bit has to go out again.
  bool yanked = placed_ && ny < y_ - 2.0;
  double prevX = x_;
  double prevY = y_;
  double prevZ = z_;
  bool wasPlaced = placed_;
  surfaceY_ = ny;
  x_ = nx;
  z_ = nz;
  // On the clearance rule the first move is the whole clearance at once, as it always was.
  // With `altitude` on, or on the way back from it, the bot climbs from here 0.4 a tick.
  // A LoadWalker wander stays on the server's feet Y. It does not add clearance.
  if (shared_->altitudeOn || ramping_) y_ = ny;
  else if (spec_.path == "wander" && !rally_.active) y_ = ny;
  else y_ = surfaceY_ + spec_.clearance;
  if (!progressSet_) {
    progressX_ = nx;
    progressSet_ = true;
  }
  transferRetries_ = 0;
  bool toward = direction_ > 0 ? nx > progressX_ + 0.05 : nx < progressX_ - 0.05;
  if (toward) {
    progressX_ = nx;
    stallTicks_ = 0;
    stuck_ = 0;
  }
  serverX_ = nx;
  serverZ_ = nz;
  serverObserved_ = true;
  if (spec_.path == "wander" && !rally_.active) {
    bool moved = !wasPlaced || std::fabs(nx - prevX) > 0.05 || std::fabs(nz - prevZ) > 0.05 ||
                 std::fabs(ny - prevY) > 0.01;
    if (moved) wanderSettle_ = 2;
  }
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
  noteCell();
  if (awaitingRespawn_ && !respawnSeen_) {
    // Sent before the server handled the respawn request. The bot is still dead.
    posSinceDeath_ = true;
    return;
  }
  place();
}

void Bot::noteCell() {
  const Grid& grid = shared_->grid;
  if (!grid.enabled) return;
  if (cellGen_ != shared_->gridGen) {
    cellGen_ = shared_->gridGen;
    cells_.reset();
  }
  int from = -1;
  int cell = gridCell(grid, x_, z_);
  if (!cells_.update(cell, from)) return;
  crossings_++;
  counters().crossings.fetch_add(1, std::memory_order_relaxed);
  if (ticks_ - lastCrossLogTick_ < kCrossLogTicks) return;
  lastCrossLogTick_ = ticks_;
  say(spec_.name + " crossed c" + std::to_string(from) + "->c" + std::to_string(cell));
}

void Bot::setRally(double x, double z, double radius) {
  rally_.active = true;
  rally_.arrived = false;
  rally_.x = x;
  rally_.z = z;
  rally_.radius = radius;
  rally_.box.enabled = true;
  rally_.box.minX = x - radius;
  rally_.box.maxX = x + radius;
  rally_.box.minZ = z - radius;
  rally_.box.maxZ = z + radius;
}

void Bot::clearRally() { rally_ = Rally{}; }

const WanderBox& Bot::activeBox() const {
  if (rally_.active) return rally_.box;
  if (box_.enabled) return box_;
  return shared_->box;
}

double Bot::wanderStep() const {
  double bps = kStep * 20.0;
  if (shared_->speedOn) bps = shared_->bps;
  else if (spec_.path == "wander") bps = spec_.bps;
  return std::max(0.05, bps) / 20.0;
}

void Bot::updateY() {
  if (spec_.path == "wander" && !rally_.active && !shared_->altitudeOn && !ramping_) {
    y_ = surfaceY_;
    return;
  }
  double clearanceY = surfaceY_ + spec_.clearance;
  if (shared_->altitudeOn) {
    ramping_ = true;
    y_ = rampAltitude(y_, shared_->altitude);
  } else if (ramping_) {
    y_ = rampAltitude(y_, clearanceY);
    if (y_ == clearanceY) ramping_ = false;
  } else {
    y_ = clearanceY;
  }
}

void Bot::sendWalkPose(bool onGround) {
  Buf move;
  move.wF64(x_);
  move.wF64(y_);
  move.wF64(z_);
  move.wF32(yaw_);
  move.wF32(0.f);
  move.wU8(onGround ? 1 : 0);
  send(pid(SbPlay::MOVE_PLAYER_POS_ROT), move.b);
  sendEmpty(pid(SbPlay::CLIENT_TICK_END));
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

void Bot::tickJavaWander(double step) {
  constexpr int kJumpTicks = 12;
  constexpr double kJumpHeight = 1.25;
  if (jumpUnstick_ > 0) {
    int t = kJumpTicks - jumpUnstick_;
    int half = kJumpTicks / 2;
    double sendY = t < half ? surfaceY_ + kJumpHeight * (t + 1) / static_cast<double>(half)
                            : surfaceY_ + kJumpHeight * (kJumpTicks - t) / static_cast<double>(half);
    double dx = 0;
    double dz = 0;
    javaWanderDelta(javaWander_.yaw, step, dx, dz);
    x_ += dx;
    z_ += dz;
    yaw_ = javaWander_.yaw;
    y_ = sendY;
    sendWalkPose(sendY <= surfaceY_ + 0.001);
    jumpUnstick_--;
    return;
  }
  double dx = 0;
  double dz = 0;
  bool opened = javaWander_.tick(javaRng_, step, dx, dz);
  if (opened) {
    say(spec_.name + " heading yaw=" + std::to_string(javaWander_.yaw));
  }
  x_ += dx;
  z_ += dz;
  yaw_ = javaWander_.yaw;
  bool onGround = !shared_->altitudeOn && !ramping_ && std::fabs(y_ - surfaceY_) <= 0.001;
  sendWalkPose(onGround);
  if (serverObserved_) {
    if (!wanderStallSet_) {
      wanderStallX_ = serverX_;
      wanderStallZ_ = serverZ_;
      wanderStallSet_ = true;
      wanderStallTicks_ = 0;
    } else if (std::hypot(serverX_ - wanderStallX_, serverZ_ - wanderStallZ_) > 2.0) {
      wanderStallX_ = serverX_;
      wanderStallZ_ = serverZ_;
      wanderStallTicks_ = 0;
    } else if (++wanderStallTicks_ >= 200) {
      x_ = serverX_;
      z_ = serverZ_;
      jumpUnstick_ = kJumpTicks;
      javaWander_.yaw = nextJavaWanderYaw(javaWander_.yaw, javaRng_);
      javaWander_.legTicks = 160 + javaRng_.nextInt(240);
      wanderStallTicks_ = 0;
      say(spec_.name + " wander unstick");
    }
  }
}

void Bot::tickWander() {
  double step = wanderStep();
  if (javaWanderPath()) {
    tickJavaWander(step);
    return;
  }
  double dx = 0;
  double dz = 0;
  bool approaching = false;
  if (rally_.active && !rally_.arrived) {
    double tx = rally_.x - x_;
    double tz = rally_.z - z_;
    double dist = std::hypot(tx, tz);
    if (dist <= rally_.radius) {
      rally_.arrived = true;
    } else {
      approaching = true;
      double len = std::min(step, dist);
      dx = tx / dist * len;
      dz = tz / dist * len;
      wander_.heading = std::atan2(tz, tx);
    }
  }
  if (!approaching) wander_.step(rng_, activeBox(), x_, z_, step, dx, dz);
  x_ += dx;
  z_ += dz;
  if (dx != 0.0 || dz != 0.0) yaw_ = yawOf(dx, dz);
  sendFlyPose(true);
}

void Bot::tickMotion() {
  if (javaWanderPath() && wanderSettle_ > 0) {
    wanderSettle_--;
    sendEmpty(pid(SbPlay::CLIENT_TICK_END));
    return;
  }
  updateY();
  if (rally_.active || spec_.path == "wander") {
    tickWander();
    return;
  }

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
  sendFlyPose(true);
}

void Bot::sayStatus() {
  statusNow_ = false;
  say(spec_.name + " phase=play placed=1 x=" + f3(x_) + " y=" + f3(y_) + " z=" + f3(z_) +
      " walkY=" + f3(surfaceY_) +
      (spec_.path == "wander" ? " path=wander heading=" + std::to_string(javaWander_.yaw) : "") +
      " dir=" + (direction_ > 0 ? "east" : "west") +
      (escaping_ ? " lane" : "") + " east_pauses=" + std::to_string(eastPauses_) +
      " west_pauses=" + std::to_string(westPauses_) + " north_pauses=" + std::to_string(northPauses_) +
      " transfers=" + std::to_string(transfersFollowed_) + " rejoins=" + std::to_string(rejoins_) +
      " deaths=" + std::to_string(deaths_) + " crossings=" + std::to_string(crossings_) +
      " path=" + spec_.path);
}

void Bot::tick() {
  if (dead) return;
  ticks_++;
  if (ticks_ == 1) {
    const WanderBox& box = activeBox();
    if (box.enabled) {
      say(spec_.name + " box " + f3(box.minX) + " " + f3(box.minZ) + " " + f3(box.maxX) + " " + f3(box.maxZ));
    } else {
      say(spec_.name + " box off");
    }
  }
  if (ticks_ > maxTicks_) {
    say(spec_.name + " max runtime (~" + std::to_string(spec_.maxSeconds) + "s)");
    stop("max runtime");
    return;
  }
  if (rejoinAtTick_ >= 0) {
    // No socket. The rejoin is a fresh login on the server `add` named.
    if (ticks_ >= rejoinAtTick_) {
      rejoinAtTick_ = -1;
      reconnect(origHost_, origPort_, 2);
      publish();
    }
    return;
  }
  if (!established_) return;
  if (phase_ != Phase::Play || !placed_) {
    if (phase_ == Phase::Play) {
      if (awaitingRespawn_ && !respawnSeen_ && ticks_ > respawnAskTick_ &&
          (ticks_ - respawnAskTick_) % kRespawnAskTicks == 0) {
        send(pid(SbPlay::CLIENT_COMMAND), clientCommandBody(kPerformRespawn));
      }
      if (waitPosTicks_ > 0 && --waitPosTicks_ == 0) {
        say(spec_.name + " no position after respawn, flying on");
        place();
      }
      sendEmpty(pid(SbPlay::CLIENT_TICK_END));
    }
    settle();
    return;
  }
  if (rejoinAttempts_ > 0 && ticks_ - placedAtTick_ >= kRejoinResetTicks) rejoinAttempts_ = 0;
  if (!sentAbilities_) sendAbilities();
  tickMotion();
  noteCell();
  if (statusNow_ || statusDue(ticks_, statusOffset_)) sayStatus();
  settle();
}

}  // namespace ckmc
