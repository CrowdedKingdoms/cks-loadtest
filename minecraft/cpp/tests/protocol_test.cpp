#include "bot.hpp"
#include "buf.hpp"
#include "control.hpp"
#include "fleet.hpp"
#include "journal.hpp"
#include "logic.hpp"
#include "PacketIds.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ckmc;

static const char* kJournalPath = "/tmp/ck-mc-load-protocol-test.journal";

static void fail(const std::string& why) {
  std::cerr << "protocol_test: " << why << "\n";
  // The bots' last lines say what they were doing when the check failed.
  Journal::get().flush();
  std::ifstream in(kJournalPath);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  size_t from = lines.size() > 12 ? lines.size() - 12 : 0;
  for (size_t i = from; i < lines.size(); i++) std::cerr << "  journal: " << lines[i] << "\n";
  std::exit(1);
}

static void check(bool ok, const std::string& what) {
  if (!ok) fail(what);
}

static std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

static double secondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(now() - t).count();
}

static int listenEphemeral(int& port) {
#if defined(__APPLE__)
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
  if (fd < 0) fail("socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) fail("bind");
  if (listen(fd, 128) < 0) fail("listen");
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) fail("getsockname");
  port = ntohs(addr.sin_port);
  return fd;
}

// A loopback port with nothing listening on it.
static int closedPort() {
  int port = 0;
  int fd = listenEphemeral(port);
  ::close(fd);
  return port;
}

// The scripted server writes several small packets in a row. Without this the second one
// waits for the bot's kernel to acknowledge the first. A game server sets the same option.
static void noDelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static int acceptWithin(int lst, int ms) {
  pollfd p{};
  p.fd = lst;
  p.events = POLLIN;
  int rc = ::poll(&p, 1, ms);
  if (rc <= 0) return -1;
  int fd = ::accept(lst, nullptr, nullptr);
  if (fd >= 0) noDelay(fd);
  return fd;
}

static void sendAll(int fd, const std::vector<uint8_t>& bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
#if defined(__APPLE__)
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
#else
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
#endif
    if (n < 0) fail("send");
    off += static_cast<size_t>(n);
  }
}

static void setRecvTimeout(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static bool readFrame(int fd, std::vector<uint8_t>& acc, int compression, std::vector<uint8_t>& packet) {
  for (int spins = 0; spins < 200; spins++) {
    std::string err;
    if (decodeFrame(acc, compression, packet, err)) {
      if (!err.empty()) fail(err);
      return true;
    }
    if (!err.empty()) fail(err);
    uint8_t tmp[4096];
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return false;
      if (errno == ECONNRESET) return false;
      fail("recv");
    }
    if (n == 0) return false;
    acc.insert(acc.end(), tmp, tmp + n);
  }
  return false;
}

// The server end of one bot's connection.
struct Peer {
  int fd = -1;
  std::vector<uint8_t> acc;
  int compression = -1;
  // From the handshake.
  int nextState = -1;
  std::string host;
  int port = 0;
};

static bool nextPacket(Peer& p, int& id, Buf& body) {
  std::vector<uint8_t> packet;
  if (!readFrame(p.fd, p.acc, p.compression, packet)) return false;
  id = packetIdOf(packet, body);
  return id >= 0;
}

static void sendPacket(Peer& p, int id, const Buf& body) { sendAll(p.fd, encodeFrame(p.compression, id, body)); }

// Reads what the bot sends for `ms`. `seen` returns true to stop early.
static void collect(Peer& p, int ms, const std::function<bool(int, Buf&)>& seen) {
  auto deadline = now() + std::chrono::milliseconds(ms);
  setRecvTimeout(p.fd, 100);
  while (now() < deadline) {
    int id = -1;
    Buf body;
    if (!nextPacket(p, id, body)) continue;
    if (seen(id, body)) break;
  }
}

static Buf positionBody(uint32_t teleportId, double x, double y, double z) {
  Buf pos;
  pos.wVar(teleportId);
  pos.wF64(x);
  pos.wF64(y);
  pos.wF64(z);
  pos.wF64(0);
  pos.wF64(0);
  pos.wF64(0);
  pos.wF32(0);
  pos.wF32(0);
  pos.wI32(0);
  return pos;
}

struct Move {
  double x = 0;
  double y = 0;
  double z = 0;
  bool airborne = false;
};

static bool readMove(const Buf& body, Move& m) {
  if (body.b.size() < 8 * 3 + 4 * 2 + 1) return false;
  Buf b;
  b.b = body.b;
  m.x = b.rF64();
  m.y = b.rF64();
  m.z = b.rF64();
  b.rF32();
  b.rF32();
  uint8_t flags = b.rU8();
  m.airborne = b.ok && flags == 0;
  return b.ok;
}

struct JoinOptions {
  double x = 1990.5;
  double y = 64.0;
  double z = -55.0;
  // The first field of the play login packet. -1 sends an empty body.
  int entityId = -1;
  // Configuration: store this cookie (key, value) when the key is not empty.
  std::string storeKey;
  std::string storeValue;
  // Configuration: ask for this cookie when the key is not empty.
  std::string askKey;
  bool cookiePresent = false;
  std::string cookieValue;
};

// How long the scripted server waits for a bot's next packet, and for a connection it
// expects. Far longer than a bot needs: on a busy Mac loopback data has been seen to sit
// unacknowledged for more than 5 s (this test as it was before the survival work failed
// five runs in forty that way), and a late packet is not what these tests are about.
static const int kPatienceMs = 30000;

// Handshake, login, configuration, then the play login packet and one position.
static void joinWorld(Peer& p, JoinOptions& o) {
  setRecvTimeout(p.fd, kPatienceMs);
  std::vector<uint8_t> packet;
  if (!readFrame(p.fd, p.acc, -1, packet)) fail("handshake");
  {
    Buf body;
    int id = packetIdOf(packet, body);
    if (id != pid(SbHandshake::CLIENT_INTENTION)) fail("first packet is not the handshake");
    uint32_t protocol = body.rVar();
    p.host = body.rUtf();
    int hi = body.rU8();
    int lo = body.rU8();
    p.port = (hi << 8) | lo;
    p.nextState = static_cast<int>(body.rVar());
    if (!body.ok || protocol != static_cast<uint32_t>(kProtocolVersion)) fail("handshake fields");
  }
  if (!readFrame(p.fd, p.acc, -1, packet)) fail("hello");
  Buf comp;
  comp.wVar(256);
  sendAll(p.fd, encodeFrame(-1, pid(CbLogin::LOGIN_COMPRESSION), comp));
  p.compression = 256;
  sendPacket(p, pid(CbLogin::LOGIN_FINISHED), Buf{});
  bool info = false;
  bool ack = false;
  bool brand = false;
  for (int i = 0; i < 6 && !(info && ack && brand); i++) {
    int id = -1;
    Buf body;
    if (!nextPacket(p, id, body)) {
      fail("config prelude: after " + std::to_string(i) + " packets, ack=" + std::to_string(ack) + " brand=" +
           std::to_string(brand) + " info=" + std::to_string(info));
    }
    if (id == pid(SbConfig::CLIENT_INFORMATION)) info = true;
    if (id == pid(SbLogin::LOGIN_ACKNOWLEDGED)) ack = true;
    if (id == pid(SbConfig::CUSTOM_PAYLOAD)) brand = true;
  }
  if (!info || !ack || !brand) fail("missing login ack, brand, or client information");
  if (!o.storeKey.empty()) {
    Buf store;
    store.wUtf(o.storeKey);
    store.wVar(static_cast<uint32_t>(o.storeValue.size()));
    store.wRaw(reinterpret_cast<const uint8_t*>(o.storeValue.data()), o.storeValue.size());
    sendPacket(p, pid(CbConfig::STORE_COOKIE), store);
  }
  if (!o.askKey.empty()) {
    Buf ask;
    ask.wUtf(o.askKey);
    sendPacket(p, pid(CbConfig::COOKIE_REQUEST), ask);
  }
  sendPacket(p, pid(CbConfig::SELECT_KNOWN_PACKS), Buf{});
  sendPacket(p, pid(CbConfig::CODE_OF_CONDUCT), Buf{});
  bool packs = false;
  bool conduct = false;
  bool cookie = o.askKey.empty();
  for (int i = 0; i < 8 && !(packs && conduct && cookie); i++) {
    int id = -1;
    Buf body;
    if (!nextPacket(p, id, body)) fail("no answer to known packs, code of conduct or cookie request");
    if (id == pid(SbConfig::SELECT_KNOWN_PACKS)) packs = true;
    if (id == pid(SbConfig::ACCEPT_CODE_OF_CONDUCT)) conduct = true;
    if (id == pid(SbConfig::COOKIE_RESPONSE)) {
      cookie = true;
      std::string key = body.rUtf();
      o.cookiePresent = body.rBool();
      if (o.cookiePresent) {
        uint32_t n = body.rVar();
        if (!body.ok || n > body.left()) fail("cookie response short");
        o.cookieValue.assign(reinterpret_cast<const char*>(body.b.data() + body.r), n);
      }
      if (key != o.askKey) fail("cookie response for the wrong key");
    }
  }
  sendPacket(p, pid(CbConfig::FINISH_CONFIGURATION), Buf{});
  bool finish = false;
  for (int i = 0; i < 4 && !finish; i++) {
    int id = -1;
    Buf body;
    if (!nextPacket(p, id, body)) fail("no finish configuration");
    if (id == pid(SbConfig::FINISH_CONFIGURATION)) finish = true;
  }
  if (!finish) fail("client did not finish configuration");
  Buf login;
  if (o.entityId >= 0) login.wI32(o.entityId);
  sendPacket(p, pid(CbPlay::LOGIN), login);
  sendPacket(p, pid(CbPlay::PLAYER_POSITION), positionBody(7, o.x, o.y, o.z));
}

struct Seen {
  bool abilities = false;
  bool flyingBit = false;
  bool moveAir = false;
  bool moveGround = false;
  bool forward = false;
  bool sprint = false;
  bool punch = false;
  bool loaded = false;
  bool accept = false;
  double moveY = 0;
};

static void note(Seen& seen, int id, const Buf& body) {
  if (id == pid(SbPlay::PLAYER_LOADED)) seen.loaded = true;
  if (id == pid(SbPlay::ACCEPT_TELEPORTATION)) seen.accept = true;
  if (id == pid(SbPlay::PLAYER_ABILITIES) && !body.b.empty()) {
    seen.abilities = true;
    seen.flyingBit = (body.b[0] & 0x02) != 0;
  }
  if (id == pid(SbPlay::PLAYER_INPUT) && !body.b.empty()) {
    if (body.b[0] & 0x01) seen.forward = true;
    if (body.b[0] & 0x40) seen.sprint = true;
  }
  if (id == pid(SbPlay::PUNCH)) seen.punch = true;
  if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) {
    Move m;
    if (readMove(body, m)) {
      seen.moveY = m.y;
      if (m.airborne) seen.moveAir = true;
      else seen.moveGround = true;
    }
  }
}

static void serveOne(int client, Seen& seen, bool playLongEnoughForPunch, bool wantAir = true) {
  Peer p;
  p.fd = client;
  JoinOptions o;
  joinWorld(p, o);
  // Ends as soon as everything was seen: about 3 s with the arm swing, well under 1 s without.
  // Seam and altitude bots fly. A wander bot with no altitude stands: abilities with the
  // flying bit clear, and a move that says its feet are on the ground.
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    note(seen, id, body);
    bool pose = wantAir ? (seen.flyingBit && seen.moveAir && seen.forward)
                        : (!seen.flyingBit && seen.moveGround);
    return seen.loaded && seen.accept && seen.abilities && pose &&
           (!playLongEnoughForPunch || (seen.sprint && seen.punch));
  });
}

static long selfRssKb() {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return -1;
  }
  return static_cast<long>(info.resident_size / 1024);
#else
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::strtol(line.c_str() + 6, nullptr, 10);
  }
  return -1;
#endif
}

static std::string readFile(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Lines of the journal that are exactly `line`.
static int journalCount(const std::string& line) {
  Journal::get().flush();
  std::ifstream in(kJournalPath);
  std::string got;
  int n = 0;
  while (std::getline(in, got)) {
    if (got == line) n++;
  }
  return n;
}

static bool journalHasPrefix(const std::string& prefix) {
  Journal::get().flush();
  std::ifstream in(kJournalPath);
  std::string got;
  while (std::getline(in, got)) {
    if (got.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

static void waitFor(const std::function<bool()>& done, int ms, const std::string& what) {
  auto deadline = now() + std::chrono::milliseconds(ms);
  while (now() < deadline) {
    if (done()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!done()) fail("timed out: " + what);
}

// ---------------------------------------------------------------------------------------
// Unit tests: no sockets.

static void testControlLines() {
  std::string buf = "add A 127.0.0.1 1 seam 2 8 5 - /dev/null\r\nstats\n\n  bogus  \nlist closed\nspee";
  std::string copy = buf;
  std::vector<std::string> lines = takeLines(copy);
  check(lines.size() == 5, "takeLines: five complete lines, got " + std::to_string(lines.size()));
  check(lines[0] == "add A 127.0.0.1 1 seam 2 8 5 - /dev/null", "takeLines: \\r\\n is stripped");
  check(lines[1] == "stats" && lines[2].empty() && lines[3] == "  bogus  " && lines[4] == "list closed", "takeLines: lines");
  check(copy == "spee", "takeLines: the partial line stays in the buffer");
  std::string none = "no newline yet";
  check(takeLines(none).empty() && none == "no newline yet", "takeLines: nothing to take");

  auto f = splitFields("  rally \t 10  -20 5  ");
  check(f.size() == 4 && f[0] == "rally" && f[1] == "10" && f[2] == "-20" && f[3] == "5", "splitFields");
  auto c = splitCommas("-100,-200,300,400");
  check(c.size() == 4 && c[0] == "-100" && c[3] == "400", "splitCommas");

  bool threw = false;
  try {
    parseNumber("12abc");
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "parseNumber refuses trailing text");
  check(parseNumber("-2.5") == -2.5 && parseWhole("42") == 42, "parseNumber, parseWhole");

  WanderBox box = parseBounds({"-10", "-20", "30", "40"});
  check(box.enabled && box.minX == -10 && box.minZ == -20 && box.maxX == 30 && box.maxZ == 40, "parseBounds");
  threw = false;
  try {
    parseBounds({"10", "0", "5", "9"});
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "parseBounds refuses min above max");
  Grid grid = parseGrid({"-5000", "-5000", "1000", "10", "10"});
  check(grid.enabled && grid.cols == 10 && grid.rows == 10 && grid.cell == 1000, "parseGrid");

  // The same buffer through a handler: one reply per command, in order, none for the blank line.
  Fleet fleet(1);
  fleet.start();
  ControlHandler handler(fleet);
  std::string out;
  bool quit = handler.feed(buf, out);
  check(!quit, "feed: no quit yet");
  check(buf == "spee", "feed: the partial line stays");
  std::vector<std::string> replies = takeLines(out);
  check(replies.size() == 4, "feed: four replies, got " + std::to_string(replies.size()));
  check(replies[0] == "ok", "feed: add -> ok, got " + replies[0]);
  check(replies[1].rfind("ok total=1 ", 0) == 0, "feed: stats sees the add before it, got " + replies[1]);
  check(replies[2] == "err unknown", "feed: unknown command, got " + replies[2]);
  check(replies[3].rfind("ok", 0) == 0, "feed: list closed, got " + replies[3]);

  buf += "d 3\naltitude 200\naltitude off\nbounds -100 -100 100 100\ngrid 0 0 512 10 10\nrally 0 0 30 5\nunrally\n"
         "speed fast\nlist nowhere\nadd too few\nquit\nstats\n";
  out.clear();
  quit = handler.feed(buf, out);
  check(quit, "feed: quit is seen");
  check(buf.empty(), "feed: the buffer was consumed");
  replies = takeLines(out);
  std::vector<std::string> want = {"ok", "ok", "ok", "ok", "ok", "ok rallied=0", "ok",
                                   "err bad number fast", "err unknown phase nowhere", "err add needs 9 fields", "ok"};
  check(replies.size() == want.size(), "feed: " + std::to_string(want.size()) + " replies and none after quit, got " +
                                           std::to_string(replies.size()));
  for (size_t i = 0; i < want.size(); i++) {
    check(replies[i] == want[i], "feed: reply " + std::to_string(i) + " is '" + replies[i] + "', want '" + want[i] + "'");
  }
  fleet.stop();
}

static void testStatsFormat() {
  FleetStats s;
  s.total = 400;
  s.connecting = 3;
  s.login = 2;
  s.config = 1;
  s.play = 390;
  s.placed = 388;
  s.closed = 4;
  s.transfers = 17;
  s.rejoins = 5;
  s.deaths = 9;
  s.respawns = 8;
  s.crossings = 1234;
  s.tickMsMax = 6;
  std::string got = formatStats(s);
  std::string want = "ok total=400 connecting=3 login=2 config=1 play=390 placed=388 closed=4 transfers=17 rejoins=5 "
                     "deaths=9 respawns=8 crossings=1234 tick_ms_max=6";
  check(got == want, "formatStats: " + got);
  check(formatStats(FleetStats{}) ==
            "ok total=0 connecting=0 login=0 config=0 play=0 placed=0 closed=0 transfers=0 rejoins=0 deaths=0 "
            "respawns=0 crossings=0 tick_ms_max=0",
        "formatStats: zeros");
}

static double angleBetween(double a, double b) {
  double d = std::fabs(std::fmod(a - b, 2.0 * kPi));
  if (d > kPi) d = 2.0 * kPi - d;
  return d;
}

static void packColumn(uint64_t* data, int localX, int localZ, int raw) {
  int index = localX + localZ * 16;
  int per = 64 / kHeightmapBits;
  int cell = index / per;
  int bit = (index - cell * per) * kHeightmapBits;
  data[cell] |= static_cast<uint64_t>(raw) << bit;
}

static Buf flatChunk(int cx, int cz, int feetY) {
  uint64_t data[kHeightmapLongs] = {};
  int raw = feetY - kOverworldMinY;
  for (int lz = 0; lz < 16; lz++) {
    for (int lx = 0; lx < 16; lx++) packColumn(data, lx, lz, raw);
  }
  Buf body;
  body.wI32(cx);
  body.wI32(cz);
  body.wVar(1);
  body.wVar(static_cast<uint32_t>(kMotionBlockingId));
  body.wVar(static_cast<uint32_t>(kHeightmapLongs));
  for (int i = 0; i < kHeightmapLongs; i++) body.wI64(static_cast<int64_t>(data[i]));
  return body;
}

static void testGroundColumns() {
  uint64_t data[kHeightmapLongs] = {};
  packColumn(data, 6, 9, 128);
  int feet = 0;
  check(motionColumnFeet(data, kHeightmapLongs, 6, 9, feet) && feet == 64, "heightmap: column feet " + std::to_string(feet));
  check(!motionColumnFeet(data, kHeightmapLongs, 0, 0, feet), "heightmap: an empty column is not ground");
  Buf chunk = flatChunk(124, -4, 70);
  ChunkGround got;
  check(readChunkGround(chunk.b.data(), chunk.b.size(), got) && got.ok && got.x == 124 && got.z == -4,
        "heightmap: chunk was not read");
  check(got.feet[6 + 9 * 16] == 70, "heightmap: packed feet " + std::to_string(got.feet[6 + 9 * 16]));
  check(chunkKey(124, -4) == (static_cast<uint64_t>(static_cast<uint32_t>(124)) |
                              (static_cast<uint64_t>(static_cast<uint32_t>(-4)) << 32)),
        "heightmap: chunk key packs x low and z high");

  double y = 90;
  double vy = 0;
  approachGround(y, vy, 84);
  check(y < 90 && y > 84, "ground: a drop starts falling, y=" + std::to_string(y));
  for (int i = 0; i < 80 && std::fabs(y - 84) > 1e-6; i++) approachGround(y, vy, 84);
  check(y == 84 && vy == 0, "ground: the fall stops on the surface, y=" + std::to_string(y));
  y = 40;
  vy = 0;
  approachGround(y, vy, 64);
  check(std::fabs(y - 40.4) < 1e-9, "ground: a climb is 0.4, y=" + std::to_string(y));
}

// A wander bot given a chunk stands on that motion-blocking surface, and does not
// keep the flying bit that would stop the server from agreeing.
static void testWanderLands() {
  int port = 0;
  int lst = listenEphemeral(port);
  Fleet fleet(1);
  fleet.start();
  BotSpec spec;
  spec.name = "Ground1";
  spec.host = "127.0.0.1";
  spec.port = port;
  spec.path = "wander";
  spec.bps = 2;
  spec.clearance = 8;
  spec.maxSeconds = 30;
  spec.logPath = "/dev/null";
  fleet.add(spec);

  Peer p;
  p.fd = acceptWithin(lst, kPatienceMs);
  if (p.fd < 0) fail("ground: no connection");
  JoinOptions o;
  o.x = 8.5;
  o.y = 100.0;
  o.z = 8.5;
  joinWorld(p, o);
  sendPacket(p, pid(CbPlay::LEVEL_CHUNK_WITH_LIGHT), flatChunk(0, 0, 64));

  bool flying = false;
  bool landed = false;
  double lowest = 1000;
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    if (id == pid(SbPlay::PLAYER_ABILITIES) && !body.b.empty() && (body.b[0] & 0x02)) flying = true;
    Move m;
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT) && readMove(body, m)) {
      if (m.y < lowest) lowest = m.y;
      if (std::fabs(m.y - 64.0) < 1e-6 && !m.airborne) landed = true;
    }
    return landed;
  });
  check(!flying, "ground: wander sent the flying bit");
  check(landed, "ground: did not stand on the heightmap, lowest y=" + std::to_string(lowest));

  fleet.stopAll();
  fleet.stop();
  ::close(p.fd);
  ::close(lst);
}

static void testJavaWander() {
  check(javaStringHash("Load1") == 73590763, "java hash Load1");
  check(javaStringHash("") == 0, "java hash empty");
  JavaRandom rng(javaStringHash("Load1"));
  JavaWander w;
  double x = 2000.5;
  double z = 2000.5;
  const double step = 0.1;
  const double wantX[5] = {2000.4803908604342, 2000.4607817208685, 2000.4411725813027, 2000.421563441737,
                           2000.4019543021711};
  const double wantZ[5] = {2000.4019414376737, 2000.3038828753474, 2000.2058243130211, 2000.1077657506949,
                           2000.0097071883686};
  for (int t = 0; t < 5; t++) {
    double dx = 0;
    double dz = 0;
    bool opened = w.tick(rng, step, dx, dz);
    x += dx;
    z += dz;
    if (t == 0) {
      check(opened, "java wander: the first tick opens a heading");
      check(std::fabs(w.yaw - 168.69151f) < 0.001f, "java wander: first yaw " + std::to_string(w.yaw));
    } else {
      check(!opened, "java wander: the leg holds");
    }
    check(std::fabs(x - wantX[t]) < 1e-6 && std::fabs(z - wantZ[t]) < 1e-6,
          "java wander: step " + std::to_string(t) + " at " + std::to_string(x) + " " + std::to_string(z));
  }
  check(w.legTicks == 288, "java wander: leg ticks left " + std::to_string(w.legTicks));
  JavaRandom turns(7);
  float yaw = 0.f;
  bool past90 = false;
  for (int i = 0; i < 40; i++) {
    float next = nextJavaWanderYaw(yaw, turns);
    float turn = std::fabs(next - yaw);
    if (turn > 180.f) turn = 360.f - turn;
    check(turn + 1e-3f >= 30.f, "java wander: turn " + std::to_string(turn));
    if (turn > 90.f) past90 = true;
    yaw = next;
  }
  check(past90, "java wander: a turn can be more than 90 degrees");
}

static void testGround() {
  uint64_t data[kHeightmapLongs] = {};
  auto put = [&](int lx, int lz, int raw) {
    int index = lx + lz * 16;
    int per = 64 / kHeightmapBits;
    int cell = index / per;
    int bit = (index - cell * per) * kHeightmapBits;
    data[cell] |= static_cast<uint64_t>(raw) << bit;
  };
  put(3, 5, 156);
  int feet = 0;
  check(motionColumnFeet(data, kHeightmapLongs, 3, 5, feet) && feet == 92,
        "ground: column feet " + std::to_string(feet));
  check(!motionColumnFeet(data, kHeightmapLongs, 0, 0, feet), "ground: an empty column is not ground");
  check(!motionColumnFeet(data, 3, 3, 5, feet), "ground: a short heightmap is ignored");

  double y = 100;
  double vy = 0;
  bool sank = false;
  for (int i = 0; i < 400; i++) {
    double prev = y;
    approachGround(y, vy, 64);
    if (y < prev - 1e-9) sank = true;
    check(y + 1e-6 >= 64.0, "ground: fell through the floor to " + std::to_string(y));
  }
  check(sank, "ground: did not fall");
  check(std::fabs(y - 64.0) < 1e-6 && vy == 0.0, "ground: landed at " + std::to_string(y));
  approachGround(y, vy, 64);
  check(std::fabs(y - 64.0) < 1e-6, "ground: did not stay down");

  y = 64;
  vy = 1;
  approachGround(y, vy, 66);
  check(std::fabs(y - 64.4) < 1e-9 && vy == 0.0, "ground: step up " + std::to_string(y));

  Buf body;
  body.wI32(4);
  body.wI32(-2);
  body.wVar(2);
  body.wVar(1);
  body.wVar(1);
  body.wI64(0);
  body.wVar(static_cast<uint32_t>(kMotionBlockingId));
  body.wVar(static_cast<uint32_t>(kHeightmapLongs));
  for (int i = 0; i < kHeightmapLongs; i++) body.wI64(static_cast<int64_t>(data[i]));
  ChunkGround chunk;
  check(readChunkGround(body.b.data(), body.b.size(), chunk), "ground: chunk parsed");
  check(chunk.ok && chunk.x == 4 && chunk.z == -2, "ground: chunk coords");
  check(chunk.feet[static_cast<size_t>(3 + 5 * 16)] == 92, "ground: stored feet");
  check(chunk.feet[0] == kNoColumn, "ground: empty stored as none");
}

static void testWander() {
  WanderBox box;
  box.enabled = true;
  box.minX = 0;
  box.minZ = 0;
  box.maxX = 400;
  box.maxZ = 400;
  // 10 blocks per second: 10,000 blocks of flight in 20,000 ticks, twenty-five widths of the
  // box. Every one of 5,000 names sees all four quadrants at that speed. At the default 2 a
  // bot flies only 2,000 blocks in that time and about four in ten names get round the box,
  // so this test would not hold there (CK-MC-LOAD.md, "Wander").
  const double step = 10.0 / 20.0;
  const double eps = 1e-9;
  for (int bot = 1; bot <= 64; bot++) {
    std::string name = "Load" + std::to_string(bot);
    Rng rng(seedFromName(name));
    Wander w;
    w.reset(rng);
    // Start in the middle of one quadrant.
    double x = 100;
    double z = 100;
    std::set<int> quadrants;
    double worst = 0;
    for (int tick = 0; tick < 20000; tick++) {
      double dx = 0;
      double dz = 0;
      w.step(rng, box, x, z, step, dx, dz);
      check(std::hypot(dx, dz) <= step + eps, name + ": a step is no longer than the speed allows");
      x += dx;
      z += dz;
      double out = std::max({box.minX - x, x - box.maxX, box.minZ - z, z - box.maxZ, 0.0});
      worst = std::max(worst, out);
      quadrants.insert((x >= 200 ? 1 : 0) + (z >= 200 ? 2 : 0));
    }
    check(worst <= step + eps, name + ": left the box by " + std::to_string(worst));
    check(worst <= eps, name + ": a bot that starts inside never leaves, left by " + std::to_string(worst));
    check(quadrants.size() >= 4, name + ": visited " + std::to_string(quadrants.size()) + " quadrants of 4");
  }

  // The same name gives the same run.
  {
    Rng a(seedFromName("Load7"));
    Rng b(seedFromName("Load7"));
    Rng other(seedFromName("Load8"));
    Wander wa, wb, wo;
    wa.reset(a);
    wb.reset(b);
    wo.reset(other);
    double ax = 50, az = 50, bx = 50, bz = 50, ox = 50, oz = 50;
    for (int tick = 0; tick < 5000; tick++) {
      double dx = 0, dz = 0;
      wa.step(a, box, ax, az, step, dx, dz);
      ax += dx;
      az += dz;
      wb.step(b, box, bx, bz, step, dx, dz);
      bx += dx;
      bz += dz;
      wo.step(other, box, ox, oz, step, dx, dz);
      ox += dx;
      oz += dz;
    }
    check(ax == bx && az == bz, "wander: the same name repeats its path");
    check(ax != ox || az != oz, "wander: another name takes another path");
    // Known answers, so a run on Linux and a run on macOS start from the same numbers.
    check(seedFromName("") == 0xcbf29ce484222325ULL && seedFromName("a") == 0xaf63dc4c8601ec8cULL, "seed: FNV-1a 64");
    Rng zero(0);
    check(zero.next() == 0xe220a8397b1dcdafULL, "rng: splitmix64");
  }

  // A turn is at least 30 degrees and the next hold is 8 to 20 s.
  {
    Rng rng(seedFromName("turns"));
    Wander w;
    w.reset(rng);
    double least = 10;
    double most = 0;
    int holdMin = 1000000;
    int holdMax = 0;
    int left = 0;
    for (int i = 0; i < 20000; i++) {
      double before = w.heading;
      w.turn(rng);
      least = std::min(least, angleBetween(before, w.heading));
      most = std::max(most, angleBetween(before, w.heading));
      if (std::sin(w.heading - before) > 0) left++;
      holdMin = std::min(holdMin, w.hold);
      holdMax = std::max(holdMax, w.hold);
      check(w.heading >= 0 && w.heading < 2.0 * kPi, "wander: the heading stays in one turn of the circle");
    }
    check(least >= kPi / 6.0 - 1e-9, "wander: a turn of " + std::to_string(least * 180.0 / kPi) + " degrees");
    check(most <= kPi / 2.0 + 1e-9, "wander: a turn of " + std::to_string(most * 180.0 / kPi) + " degrees");
    check(left > 9000 && left < 11000, "wander: turns go to both sides");
    check(holdMin >= 160 && holdMax <= 400, "wander: hold ticks " + std::to_string(holdMin) + ".." + std::to_string(holdMax));
    check(holdMin <= 170 && holdMax >= 390, "wander: holds cover 8 to 20 s");
  }

  // The heading is kept for the hold, in open space.
  {
    Rng rng(seedFromName("holds"));
    Wander w;
    w.reset(rng);
    WanderBox open;
    double x = 0, z = 0;
    int run = 0;
    int shortest = 1000000;
    int longest = 0;
    double last = w.heading;
    for (int tick = 0; tick < 200000; tick++) {
      double dx = 0, dz = 0;
      w.step(rng, open, x, z, step, dx, dz);
      x += dx;
      z += dz;
      if (w.heading != last) {
        if (tick > 400) {
          shortest = std::min(shortest, run);
          longest = std::max(longest, run);
        }
        check(angleBetween(w.heading, last) >= kPi / 6.0 - 1e-9, "wander: a new heading is 30 degrees away");
        last = w.heading;
        run = 0;
      }
      run++;
    }
    check(shortest >= 160 && longest <= 400, "wander: kept a heading for " + std::to_string(shortest) + ".." +
                                                 std::to_string(longest) + " ticks");
  }

  // Outside the box: straight back in, then it stays in.
  {
    Rng rng(seedFromName("outside"));
    Wander w;
    w.reset(rng);
    double x = 520;
    double z = -90;
    double lastDist = 1e18;
    bool entered = false;
    for (int tick = 0; tick < 5000; tick++) {
      double dx = 0, dz = 0;
      w.step(rng, box, x, z, step, dx, dz);
      x += dx;
      z += dz;
      if (!entered) {
        // Distance to the box shrinks on every step until the bot is in.
        double ddx = std::max({box.minX - x, x - box.maxX, 0.0});
        double ddz = std::max({box.minZ - z, z - box.maxZ, 0.0});
        double dist = std::hypot(ddx, ddz);
        check(dist < lastDist, "wander: outside the box every step goes toward it");
        lastDist = dist;
        if (inBox(box, x, z)) entered = true;
      } else {
        check(inBox(box, x, z), "wander: back inside, the bot stays inside");
      }
    }
    check(entered, "wander: the bot got back into its box");
  }

  // A box narrower than a step.
  {
    WanderBox tiny;
    tiny.enabled = true;
    tiny.minX = 10;
    tiny.maxX = 10.05;
    tiny.minZ = 10;
    tiny.maxZ = 10.05;
    Rng rng(seedFromName("tiny"));
    Wander w;
    w.reset(rng);
    double x = 10.02, z = 10.02;
    for (int tick = 0; tick < 2000; tick++) {
      double dx = 0, dz = 0;
      w.step(rng, tiny, x, z, step, dx, dz);
      x += dx;
      z += dz;
      check(x >= 10 - eps && x <= 10.05 + eps && z >= 10 - eps && z <= 10.05 + eps, "wander: a tiny box holds the bot");
    }
  }
}

static void testAltitudeRamp() {
  double y = 64.0;
  int ticks = 0;
  while (y != 200.0) {
    double next = rampAltitude(y, 200.0);
    check(next > y && next - y <= kMaxClimbPerTick + 1e-12, "ramp: climbs at most 0.4 a tick");
    y = next;
    check(++ticks < 1000, "ramp: reaches the target");
  }
  check(ticks == 340, "ramp: 136 blocks take 340 ticks, took " + std::to_string(ticks));
  while (y != 72.0) {
    double next = rampAltitude(y, 72.0);
    check(next < y && y - next <= kMaxClimbPerTick + 1e-12, "ramp: descends at most 0.4 a tick");
    y = next;
  }
  check(rampAltitude(72.0, 72.0) == 72.0, "ramp: holds at the target");
  check(rampAltitude(72.0, 72.3) == 72.3, "ramp: a short last step lands on the target");
  check(kMaxClimbPerTick == 0.4, "ramp: the limit is 0.4");
}

static void testRejoinBackoff() {
  const double base[10] = {2, 4, 8, 16, 30, 30, 30, 30, 30, 30};
  for (int attempt = 1; attempt <= 10; attempt++) {
    double b = base[attempt - 1];
    check(rejoinBaseSeconds(attempt) == b, "backoff: attempt " + std::to_string(attempt));
    int lo = rejoinDelayTicks(attempt, 0.0);
    int mid = rejoinDelayTicks(attempt, 0.5);
    int hi = rejoinDelayTicks(attempt, 0.999999);
    check(lo == static_cast<int>(std::lround(b * 20 * 0.75)), "backoff: -25%");
    check(mid == static_cast<int>(std::lround(b * 20)), "backoff: no jitter in the middle");
    check(hi <= static_cast<int>(std::lround(b * 20 * 1.25)) && hi > mid, "backoff: +25%");
    Rng rng(seedFromName("backoff" + std::to_string(attempt)));
    for (int i = 0; i < 1000; i++) {
      int t = rejoinDelayTicks(attempt, rng.unit());
      check(t >= lo && t <= static_cast<int>(std::lround(b * 20 * 1.25)), "backoff: jitter stays within 25%");
    }
  }
  check(kRejoinMaxAttempts == 10, "backoff: ten attempts");
  check(kRejoinResetTicks == 120 * 20, "backoff: 120 s of play clears the count");
}

static void testGrid() {
  Grid g;
  g.enabled = true;
  g.originX = -1000;
  g.originZ = -1000;
  g.cell = 100;
  g.cols = 20;
  g.rows = 20;
  check(gridCell(g, -1000, -1000) == 0, "grid: the origin is cell 0");
  check(gridCell(g, -900.01, -1000) == 0 && gridCell(g, -900, -1000) == 1, "grid: a cell ends where the next begins");
  check(gridCell(g, -1, -1) == 9 * 20 + 9, "grid: just below zero");
  check(gridCell(g, 0, 0) == 10 * 20 + 10, "grid: zero");
  check(gridCell(g, -0.5, 0.5) == 10 * 20 + 9, "grid: x below zero, z above");
  check(gridCell(g, 999.99, 999.99) == 399, "grid: the last cell");
  check(gridCell(g, 1000, 0) == -1 && gridCell(g, 0, 1000) == -1, "grid: past the far edge");
  check(gridCell(g, -1000.01, 0) == -1 && gridCell(g, 0, -1000.01) == -1, "grid: before the origin");

  // Floor, not truncation: -0.5 / 16 truncates to column 0, and that would be wrong.
  Grid zero;
  zero.enabled = true;
  zero.cell = 16;
  zero.cols = 4;
  zero.rows = 4;
  check(gridCell(zero, -0.5, 8) == -1, "grid: floor division on a negative coordinate");
  check(gridCell(zero, 8, -0.5) == -1, "grid: floor division on a negative z");
  check(gridCell(zero, 15.99, 0) == 0 && gridCell(zero, 16, 0) == 1 && gridCell(zero, 0, 16) == 4, "grid: columns and rows");
  Grid shifted = zero;
  shifted.originX = -64;
  shifted.originZ = -64;
  shifted.cols = 8;
  shifted.rows = 8;
  check(gridCell(shifted, -0.5, -0.5) == 3 * 8 + 3, "grid: negative coordinates inside a grid that starts below zero");
  check(gridCell(shifted, -63.9, -48) == 1 * 8 + 0, "grid: negative row");
  check(gridCell(Grid{}, 0, 0) == -1, "grid: off");

  CellTracker t;
  int from = -1;
  check(!t.update(5, from), "cells: the first cell is not a crossing");
  check(!t.update(5, from), "cells: the same cell is not a crossing");
  check(t.update(6, from) && from == 5, "cells: 5 -> 6");
  check(!t.update(-1, from), "cells: outside the grid is not a crossing");
  check(!t.update(6, from), "cells: back in the same cell after being outside");
  check(!t.update(-1, from) && t.update(7, from) && from == 6, "cells: out, then in by another cell");
  t.reset();
  check(!t.update(9, from), "cells: a new connection starts over");
}

static void testRespawnPacket() {
  check(pid(SbPlay::CLIENT_COMMAND) == 12, "client command id");
  std::vector<uint8_t> body = clientCommandBody(kPerformRespawn);
  check(body == std::vector<uint8_t>{0x00}, "perform respawn is one VarInt, 0");
  Buf b;
  b.b = body;
  // Before compression is set: length, id, action.
  check(encodeFrame(-1, pid(SbPlay::CLIENT_COMMAND), b) == (std::vector<uint8_t>{0x02, 0x0C, 0x00}), "respawn frame");
  // In play, under the compression threshold: length, 0 for "not compressed", id, action.
  check(encodeFrame(256, pid(SbPlay::CLIENT_COMMAND), b) == (std::vector<uint8_t>{0x03, 0x00, 0x0C, 0x00}),
        "respawn frame with compression on");
  check(clientCommandBody(1) == std::vector<uint8_t>{0x01}, "another action");
}

static void testStatusCadence() {
  int due = 0;
  for (int tick = 1; tick <= 6000; tick++) due += statusDue(tick, 137) ? 1 : 0;
  check(due == 10, "status: once every 600 ticks");
  check(statusDue(600 - 137, 137) && !statusDue(600, 137), "status: the offset moves the tick");
  std::set<int> offsets;
  for (int bot = 1; bot <= 400; bot++) {
    offsets.insert(static_cast<int>(seedFromName("Load" + std::to_string(bot)) % static_cast<uint64_t>(kStatusTicks)));
  }
  check(offsets.size() > 200, "status: 400 bots spread over " + std::to_string(offsets.size()) + " ticks");
}

// ---------------------------------------------------------------------------------------
// Against a scripted server on loopback.

// Death, respawn, a respawn packet that is not a death, the altitude ramp and the crossing counter.
static void testSurvival() {
  Counters& c = counters();
  uint64_t deaths0 = c.deaths.load();
  uint64_t respawns0 = c.respawns.load();
  uint64_t crossings0 = c.crossings.load();

  int port = 0;
  int lst = listenEphemeral(port);
  Fleet fleet(1);
  fleet.start();
  fleet.setGrid(parseGrid({"-2000", "-2000", "100", "80", "40"}));
  fleet.setAltitude(true, 70.0);
  BotSpec spec;
  spec.name = "Surv1";
  spec.host = "127.0.0.1";
  spec.port = port;
  spec.path = "wander";
  // Its own small box, inside grid cell 19 * 80 + 39 = 1559.
  spec.waypoints = "1985,-60,1995,-50";
  spec.bps = 2;
  spec.clearance = 8;
  spec.maxSeconds = 120;
  spec.logPath = "/dev/null";
  fleet.add(spec);

  Peer p;
  p.fd = acceptWithin(lst, kPatienceMs);
  if (p.fd < 0) fail("survival: no connection");
  JoinOptions o;
  o.entityId = 4242;
  joinWorld(p, o);
  check(p.nextState == 2, "survival: a first login has handshake state 2");

  // Altitude 70 from a server Y of 64: 0.4 a tick, never the whole way at once.
  std::vector<Move> moves;
  int atTarget = 0;
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    Move m;
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT) && readMove(body, m)) {
      moves.push_back(m);
      if (m.y == 70.0) atTarget++;
    }
    return atTarget >= 5;
  });
  check(atTarget >= 5, "survival: the bot reached altitude 70");
  check(moves.front().y <= 64.4 + 1e-9 && moves.front().y > 64.0, "survival: first move Y " + std::to_string(moves.front().y));
  for (size_t i = 1; i < moves.size(); i++) {
    check(std::fabs(moves[i].y - moves[i - 1].y) <= 0.4 + 1e-9, "survival: climbed more than 0.4 in one tick");
    check(moves[i].airborne, "survival: on the ground");
    double dist = std::hypot(moves[i].x - moves[i - 1].x, moves[i].z - moves[i - 1].z);
    check(std::fabs(dist - 0.1) < 1e-6, "survival: wander step " + std::to_string(dist));
  }
  // 64.4, 64.8, ... 70.0 is fifteen moves, and four more at 70 make the five that end the wait.
  check(moves.size() >= 19, "survival: the 6-block climb took " + std::to_string(moves.size() - 4) + " moves, not 15");

  // A kill of somebody else is not this bot's death.
  Buf other;
  other.wVar(99);
  other.wU8(0);
  sendPacket(p, pid(CbPlay::PLAYER_COMBAT_KILL), other);
  bool asked = false;
  collect(p, 400, [&](int id, Buf&) {
    if (id == pid(SbPlay::CLIENT_COMMAND)) asked = true;
    return asked;
  });
  check(!asked, "survival: answered a kill of another player");

  // Health 0: the bot asks to respawn and stops flying.
  Buf health;
  health.wF32(0.f);
  health.wVar(20);
  health.wF32(5.f);
  sendPacket(p, pid(CbPlay::SET_HEALTH), health);
  Buf kill;
  kill.wVar(4242);
  kill.wU8(0);
  sendPacket(p, pid(CbPlay::PLAYER_COMBAT_KILL), kill);
  std::vector<uint8_t> commandBody;
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    if (id == pid(SbPlay::CLIENT_COMMAND)) {
      asked = true;
      commandBody = body.b;
    }
    return asked;
  });
  check(asked, "survival: no respawn request after health 0");
  check(commandBody == std::vector<uint8_t>{0x00}, "survival: the respawn request is action 0");
  int movesWhileDead = 0;
  int asksWhileDead = 0;
  collect(p, 600, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) movesWhileDead++;
    if (id == pid(SbPlay::CLIENT_COMMAND)) asksWhileDead++;
    return false;
  });
  check(movesWhileDead == 0, "survival: moved while dead");
  check(asksWhileDead == 0, "survival: the health packet and the kill packet are one death");

  // The server respawns the player in the next cell east.
  sendPacket(p, pid(CbPlay::RESPAWN), Buf{});
  sendPacket(p, pid(CbPlay::PLAYER_POSITION), positionBody(9, 2050.5, 64.0, -55.0));
  bool accepted = false;
  bool loaded = false;
  bool flying = false;
  moves.clear();
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    if (id == pid(SbPlay::ACCEPT_TELEPORTATION) && body.rVar() == 9) accepted = true;
    if (id == pid(SbPlay::PLAYER_LOADED)) loaded = true;
    if (id == pid(SbPlay::PLAYER_ABILITIES) && !body.b.empty() && (body.b[0] & 0x02)) flying = true;
    Move m;
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT) && readMove(body, m)) moves.push_back(m);
    return moves.size() >= 10;
  });
  check(accepted, "survival: the respawn position was not accepted");
  check(loaded, "survival: no player-loaded after the respawn");
  check(flying, "survival: the flying abilities were not sent again");
  check(moves.size() >= 10, "survival: the bot did not fly on after the respawn");
  check(moves.front().y <= 64.4 + 1e-9, "survival: after the respawn the climb starts from the server Y");
  for (size_t i = 1; i < moves.size(); i++) {
    check(std::fabs(moves[i].y - moves[i - 1].y) <= 0.4 + 1e-9, "survival: climbed more than 0.4 after the respawn");
    double dist = std::hypot(moves[i].x - moves[i - 1].x, moves[i].z - moves[i - 1].z);
    check(std::fabs(dist - 0.1) < 1e-6, "survival: wander step after respawn " + std::to_string(dist));
  }

  // A respawn packet with no death. The keep-alive answer marks the point the bot has read it.
  auto respawnSent = now();
  auto respawnNoDeath = [&](int64_t marker) {
    respawnSent = now();
    sendPacket(p, pid(CbPlay::RESPAWN), Buf{});
    Buf keep;
    keep.wI64(marker);
    sendPacket(p, pid(CbPlay::KEEP_ALIVE), keep);
    bool marked = false;
    collect(p, kPatienceMs, [&](int id, Buf& body) {
      if (id == pid(SbPlay::KEEP_ALIVE) && body.rI64() == marker) marked = true;
      return marked;
    });
    check(marked, "survival: no keep-alive answer");
  };
  respawnNoDeath(777);
  int movesWaiting = 0;
  bool askedAgain = false;
  collect(p, 500, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) movesWaiting++;
    if (id == pid(SbPlay::CLIENT_COMMAND)) askedAgain = true;
    return false;
  });
  check(movesWaiting == 0, "survival: moved after a respawn packet, before the position");
  check(!askedAgain, "survival: asked to respawn without a death");
  sendPacket(p, pid(CbPlay::PLAYER_POSITION), positionBody(10, 2050.5, 64.0, -55.0));
  accepted = false;
  loaded = false;
  int movesAfter = 0;
  collect(p, kPatienceMs, [&](int id, Buf& body) {
    if (id == pid(SbPlay::ACCEPT_TELEPORTATION) && body.rVar() == 10) accepted = true;
    if (id == pid(SbPlay::PLAYER_LOADED)) loaded = true;
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) movesAfter++;
    return movesAfter >= 5;
  });
  check(accepted && loaded && movesAfter >= 5, "survival: did not carry on after a respawn packet and a position");

  // A respawn packet and no position at all: the bot flies on from where it was after 3 s.
  respawnNoDeath(778);
  double firstMove = -1;
  collect(p, kPatienceMs, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) firstMove = secondsSince(respawnSent);
    return firstMove >= 0;
  });
  // 60 ticks after the bot read the packet, so never under 3 s after the server sent it.
  check(firstMove >= 2.95, "survival: flew on only " + std::to_string(firstMove) + " s after a respawn packet");
  check(firstMove <= 12.0, "survival: flew on " + std::to_string(firstMove) + " s after a respawn packet");

  check(c.deaths.load() - deaths0 == 1, "survival: deaths counted " + std::to_string(c.deaths.load() - deaths0));
  check(c.respawns.load() - respawns0 == 1, "survival: respawns counted " + std::to_string(c.respawns.load() - respawns0));
  check(c.crossings.load() - crossings0 == 1, "survival: crossings counted " + std::to_string(c.crossings.load() - crossings0));
  FleetStats stats = fleet.stats();
  check(stats.total == 1 && stats.play == 1 && stats.placed == 1, "survival: stats " + formatStats(stats));
  check(fleet.list("placed") == "ok Surv1" && fleet.list("play") == "ok Surv1" && fleet.list("closed") == "ok",
        "survival: list");
  check(journalCount("Surv1 died") == 1, "survival: one died line");
  check(journalCount("Surv1 respawned") == 1, "survival: one respawned line");
  check(journalCount("Surv1 crossed c1559->c1560") == 1, "survival: the crossed line");
  check(journalCount("Surv1 respawn packet, waiting for a position") == 2, "survival: the respawn packet lines");
  check(journalCount("Surv1 no position after respawn, flying on") == 1, "survival: the fly-on line");
  check(journalCount("Surv1 box off") == 1, "survival: wander has no box");

  fleet.stopAll();
  waitFor([&] { return fleet.stats().closed == 1; }, kPatienceMs, "survival: stop-all");
  check(fleet.list("closed") == "ok Surv1:stopped", "survival: list closed is " + fleet.list("closed"));
  fleet.stop();
  ::close(p.fd);
  ::close(lst);
}

// A transfer keeps the cookies and uses handshake state 3. A dropped socket rejoins the
// server `add` named, after the backoff, with state 2 and no cookies.
static void testTransferAndRejoin() {
  Counters& c = counters();
  uint64_t transfers0 = c.transfers.load();
  uint64_t rejoins0 = c.rejoins.load();

  int portA = 0;
  int portB = 0;
  int lstA = listenEphemeral(portA);
  int lstB = listenEphemeral(portB);
  Fleet fleet(1);
  fleet.start();
  BotSpec spec;
  spec.name = "Rej1";
  spec.host = "127.0.0.1";
  spec.port = portA;
  spec.path = "wander";
  spec.bps = 2;
  spec.clearance = 8;
  spec.maxSeconds = 120;
  spec.logPath = "/dev/null";
  fleet.add(spec);

  // First login on A. A stores a cookie and transfers the bot to B.
  Peer a;
  a.fd = acceptWithin(lstA, kPatienceMs);
  if (a.fd < 0) fail("rejoin: no first connection");
  JoinOptions first;
  first.storeKey = "ck:test";
  first.storeValue = "from-a";
  joinWorld(a, first);
  check(a.nextState == 2, "rejoin: first login state " + std::to_string(a.nextState));
  int moves = 0;
  collect(a, kPatienceMs, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) moves++;
    return moves >= 3;
  });
  check(moves >= 3, "rejoin: the bot did not fly on A");
  Buf transfer;
  transfer.wUtf("127.0.0.1");
  transfer.wVar(static_cast<uint32_t>(portB));
  sendPacket(a, pid(CbPlay::TRANSFER), transfer);

  Peer b;
  b.fd = acceptWithin(lstB, kPatienceMs);
  if (b.fd < 0) fail("rejoin: the transfer was not followed");
  ::close(a.fd);
  JoinOptions second;
  second.askKey = "ck:test";
  joinWorld(b, second);
  check(b.nextState == 3, "rejoin: a transfer has handshake state 3, got " + std::to_string(b.nextState));
  check(b.port == portB, "rejoin: the transfer handshake names B's port");
  check(second.cookiePresent && second.cookieValue == "from-a", "rejoin: the transfer kept the cookie");
  moves = 0;
  collect(b, kPatienceMs, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) moves++;
    return moves >= 3;
  });
  check(moves >= 3, "rejoin: the bot did not fly on B");

  // B drops the socket. The bot waits 2 s (1.5 to 2.5 with jitter) and logs in again on A.
  ::close(b.fd);
  auto dropped = now();
  Peer again;
  again.fd = acceptWithin(lstA, kPatienceMs);
  double waited = secondsSince(dropped);
  if (again.fd < 0) fail("rejoin: the bot did not come back to the server it was added with");
  // Never sooner than 2 s less 25%. Later is the machine, not the bot: 2.5 s is the most it waits.
  check(waited >= 1.4, "rejoin: came back after only " + std::to_string(waited) + " s");
  check(waited <= 10.0, "rejoin: came back after " + std::to_string(waited) + " s");
  pollfd onB{};
  onB.fd = lstB;
  onB.events = POLLIN;
  check(::poll(&onB, 1, 0) == 0, "rejoin: the bot went back to the transfer target");
  JoinOptions third;
  third.askKey = "ck:test";
  joinWorld(again, third);
  check(again.nextState == 2, "rejoin: a rejoin is a fresh login, state " + std::to_string(again.nextState));
  check(again.port == portA && again.host == "127.0.0.1", "rejoin: the handshake names the first server");
  check(!third.cookiePresent, "rejoin: a rejoin carries no cookies");
  moves = 0;
  collect(again, kPatienceMs, [&](int id, Buf&) {
    if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT)) moves++;
    return moves >= 3;
  });
  check(moves >= 3, "rejoin: the bot did not fly after the rejoin");

  check(c.transfers.load() - transfers0 == 1, "rejoin: transfers counted");
  check(c.rejoins.load() - rejoins0 == 1, "rejoin: rejoins counted");
  FleetStats stats = fleet.stats();
  check(stats.play == 1 && stats.placed == 1 && stats.closed == 0, "rejoin: stats " + formatStats(stats));
  check(journalCount("Rej1 transferred to 127.0.0.1:" + std::to_string(portB)) == 1, "rejoin: the transferred line");
  check(journalCount("Rej1 rejoin 1 after: connection closed") == 1, "rejoin: the rejoin line");
  check(journalCount("Rej1 connecting to 127.0.0.1:" + std::to_string(portA)) == 2, "rejoin: two connects to A");

  // A kick is a rejoin too, and the count goes up.
  sendPacket(again, pid(CbPlay::DISCONNECT), Buf{});
  waitFor([&] { return journalCount("Rej1 rejoin 2 after: play disconnect") == 1; }, kPatienceMs, "rejoin: the second rejoin line");
  check(fleet.list("connecting") == "ok Rej1", "rejoin: a bot waiting to rejoin is connecting");

  fleet.stop();
  ::close(again.fd);
  ::close(lstA);
  ::close(lstB);
}

// 400 bots on two workers: every one placed, and a tick pass far below the 50 ms tick.
static void testFourHundred() {
  const int kBots = 400;
  int port = 0;
  int lst = listenEphemeral(port);
  std::atomic<bool> serving{true};
  std::atomic<int> joined{0};
  std::vector<std::thread> peers;
  std::thread acceptor([&] {
    while (serving.load() && static_cast<int>(peers.size()) < kBots) {
      int fd = acceptWithin(lst, 100);
      if (fd < 0) continue;
      peers.emplace_back([fd, &serving, &joined] {
        Peer p;
        p.fd = fd;
        JoinOptions o;
        joinWorld(p, o);
        joined.fetch_add(1);
        setRecvTimeout(fd, 200);
        uint8_t sink[8192];
        while (serving.load()) {
          ssize_t n = ::recv(fd, sink, sizeof(sink), 0);
          if (n == 0) break;
          if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
        }
        ::close(fd);
      });
    }
  });

  Fleet fleet(2);
  fleet.start();
  fleet.setBounds(parseBounds({"1790.5", "-255", "2190.5", "145"}));
  for (int i = 0; i < kBots; i++) {
    BotSpec spec;
    spec.name = "Mass" + std::to_string(i + 1);
    spec.host = "127.0.0.1";
    spec.port = port;
    spec.path = "wander";
    spec.bps = 4;
    spec.clearance = 8;
    spec.maxSeconds = 120;
    spec.logPath = "/dev/null";
    fleet.add(std::move(spec));
    if (i % 20 == 19) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  waitFor([&] { return fleet.stats().placed == static_cast<uint64_t>(kBots); }, 120000, "400 bots placed");
  fleet.stats();  // start a fresh tick maximum
  std::this_thread::sleep_for(std::chrono::seconds(3));
  FleetStats stats = fleet.stats();
  check(stats.total == static_cast<uint64_t>(kBots) && stats.play == static_cast<uint64_t>(kBots) &&
            stats.placed == static_cast<uint64_t>(kBots) && stats.closed == 0,
        "400 bots: " + formatStats(stats));
  std::cout << "protocol_test: 400 bots on 2 workers, 3 s of play: tick_ms_max=" << stats.tickMsMax << "\n";
  // The tick is 50 ms. A pass normally takes a few; the limit leaves room for a busy machine.
  check(stats.tickMsMax < 50, "400 bots: a tick pass took " + std::to_string(stats.tickMsMax) + " ms");

  check(fleet.rally(1990.5, -55, 20, 10) == 10, "400 bots: rally picks the first ten in play");
  check(fleet.rally(1990.5, -55, 20, -1) == static_cast<size_t>(kBots), "400 bots: rally with no count picks all");
  fleet.unrally();
  std::string all = fleet.list("play");
  check(all.rfind("ok Mass1,Mass2,", 0) == 0, "400 bots: list is in add order");
  check(static_cast<int>(std::count(all.begin(), all.end(), ',')) == kBots - 1, "400 bots: list play names every bot");
  std::string capped = fleet.list("play", 100);
  check(capped.size() < 100 && capped.size() > 80 && capped.rfind(",...") == capped.size() - 4,
        "400 bots: a capped list ends with ..., got " + capped);
  std::string full = fleet.list("placed");
  check(full.size() + 1 <= Fleet::kListCap, "400 bots: the list fits the 64 KB cap");

  fleet.stopAll();
  waitFor([&] { return fleet.stats().closed == static_cast<uint64_t>(kBots); }, kPatienceMs, "400 bots stopped");
  fleet.stop();
  serving.store(false);
  acceptor.join();
  for (auto& t : peers) t.join();
  check(joined.load() == kBots, "400 bots: joined " + std::to_string(joined.load()));
  ::close(lst);
}

// Bots that cannot connect end as closed, with a reason, and their memory is given back.
static void testClosedBots() {
  int port = closedPort();
  Fleet fleet(2);
  fleet.start();
  const int kBots = 300;
  for (int i = 0; i < kBots; i++) {
    BotSpec spec;
    spec.name = "Dead" + std::to_string(i + 1);
    spec.host = "127.0.0.1";
    spec.port = port;
    spec.path = "wander";
    spec.maxSeconds = 30;
    spec.logPath = "/dev/null";
    fleet.add(std::move(spec));
  }
  waitFor([&] { return fleet.stats().closed == static_cast<uint64_t>(kBots); }, kPatienceMs, "300 refused bots closed");
  FleetStats stats = fleet.stats();
  check(stats.total == static_cast<uint64_t>(kBots) && stats.connecting == 0 && stats.play == 0, "closed: " + formatStats(stats));
  check(fleet.started() == kBots && fleet.finished() == kBots, "closed: started and finished");
  std::string closed = fleet.list("closed");
  check(closed.rfind("ok Dead1:connect", 0) == 0, "closed: the list gives name:reason, got " + closed.substr(0, 60));
  std::string capped = fleet.list("closed", 256);
  check(capped.size() < 256 && capped.rfind(",...") == capped.size() - 4, "closed: capped list");
  check(journalHasPrefix("Dead1 closed: connect"), "closed: the closed line");
  fleet.stop();
}

// The checks this test had before the mesh work: the offline UUID, framing, one bot's
// packets, and ten bots' memory.
static void testBasics() {
  auto uuid = offlineUuid("Load1");
  const char* expect = "0992312cc0783fb3907d03a0edcf65ef";
  std::string hex;
  for (uint8_t byte : uuid) {
    char b[3];
    std::snprintf(b, sizeof(b), "%02x", byte);
    hex += b;
  }
  if (hex != expect) fail("offline uuid " + hex);

  Buf raw;
  raw.wVar(2);
  raw.wU8(0x80);
  raw.wU8(1);
  std::string err;
  std::vector<uint8_t> packet;
  if (!decodeFrame(raw.b, -1, packet, err) || packet.size() != 2 || packet[0] != 0x80 || packet[1] != 1) {
    fail("frame roundtrip");
  }
  Buf body;
  body.wUtf("hi");
  auto framed = encodeFrame(256, 3, body);
  std::vector<uint8_t> acc = framed;
  if (!decodeFrame(acc, 256, packet, err) || packet.empty() || packet[0] != 3) fail("compressed frame");

  int port = 0;
  int lst = listenEphemeral(port);
  Fleet fleet(2);
  fleet.start();
  BotSpec spec;
  spec.name = "Load1";
  spec.host = "127.0.0.1";
  spec.port = port;
  spec.path = "seam";
  spec.clearance = 8;
  spec.maxSeconds = 120;
  spec.logPath = "/tmp/ck-mc-load-protocol-test.log";
  std::remove(spec.logPath.c_str());
  fleet.add(spec);

  sockaddr_in clientAddr{};
  socklen_t clen = sizeof(clientAddr);
  int client = ::accept(lst, reinterpret_cast<sockaddr*>(&clientAddr), &clen);
  if (client < 0) fail("accept");
  noDelay(client);
  Seen seen;
  serveOne(client, seen, true);
  if (!seen.loaded || !seen.accept) fail("did not enter the world");
  if (!seen.flyingBit) fail("flying ability bit not set");
  if (!seen.moveAir) fail("move packet was not airborne");
  if (seen.moveY < 71.0 || seen.moveY > 73.0) fail("fly height " + std::to_string(seen.moveY));
  if (!seen.forward) fail("no forward input");
  if (!seen.sprint) fail("no sprint input");
  if (!seen.punch) fail("no arm swing");
  fleet.stopBot("Load1");
  ::close(client);

  // The bot's own log file gets the same lines the journal does.
  Journal::get().flush();
  std::string all = readFile(spec.logPath);
  if (all.find("phase=play") == std::string::npos) fail("missing phase=play log");
  if (all.find(" at ") == std::string::npos) fail("missing server position log");
  if (all.find("Load1 box off") == std::string::npos) fail("missing box log");
  if (journalCount("Load1 at 1990.5000 64.0000 -55.0000") != 1) fail("the journal is missing the position line");

  // Ten clients in this same process. RSS must stay under one Java walker's 512 MB heap.
  int port2 = 0;
  int lst2 = listenEphemeral(port2);
  std::vector<std::thread> servers;
  std::atomic<int> joined{0};
  for (int i = 0; i < 10; i++) {
    BotSpec many;
    many.name = "Load" + std::to_string(i + 1);
    many.host = "127.0.0.1";
    many.port = port2;
    many.path = "wander";
    many.bps = 2;
    many.clearance = 8;
    many.maxSeconds = 120;
    fleet.add(std::move(many));
  }
  for (int i = 0; i < 10; i++) {
    int c = ::accept(lst2, nullptr, nullptr);
    if (c < 0) fail("accept ten");
    noDelay(c);
    servers.emplace_back([c, &joined] {
      Seen local;
      serveOne(c, local, false, false);
      if (local.moveGround && local.abilities && !local.flyingBit) joined.fetch_add(1);
      ::close(c);
    });
  }
  for (auto& t : servers) t.join();
  if (joined.load() != 10) fail("ten-bot join count " + std::to_string(joined.load()));

  long rssKb = selfRssKb();
  std::cout << "protocol_test: ten bots VmRSS_kB=" << rssKb
            << " java_one_bot_heap_kB=" << (512 * 1024) << "\n";
  if (rssKb < 0) fail("no VmRSS");
  if (rssKb >= 512 * 1024) fail("rss is not below one Java walker heap");

  fleet.stop();
  ::close(lst);
  ::close(lst2);
}

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  rlimit lim{};
  if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY && lim.rlim_cur < 4096) {
    lim.rlim_cur = (lim.rlim_max == RLIM_INFINITY || lim.rlim_max > 4096) ? 4096 : lim.rlim_max;
    setrlimit(RLIMIT_NOFILE, &lim);
  }
  // Bot lines go to one file, the way --journal sends them, and stdout stays readable.
  std::remove(kJournalPath);
  Journal::get().open(kJournalPath);

  // With no argument every section runs. One argument names the only section to run:
  // unit, basics, survival, rejoin, closed or mass.
  std::string only = argc > 1 ? argv[1] : "";
  auto wanted = [&](const char* section) { return only.empty() || only == section; };

  if (wanted("unit")) {
    testControlLines();
    testStatsFormat();
    testGroundColumns();
    testWanderLands();
    testJavaWander();
    testGround();
    testWander();
    testAltitudeRamp();
    testRejoinBackoff();
    testGrid();
    testRespawnPacket();
    testStatusCadence();
    std::cout << "protocol_test: unit tests ok\n";
  }
  if (wanted("basics")) testBasics();
  if (wanted("survival")) {
    testSurvival();
    std::cout << "protocol_test: survival ok\n";
  }
  if (wanted("rejoin")) {
    testTransferAndRejoin();
    std::cout << "protocol_test: transfer and rejoin ok\n";
  }
  if (wanted("closed")) {
    testClosedBots();
    std::cout << "protocol_test: closed bots ok\n";
  }
  if (wanted("mass")) {
    testFourHundred();
    std::cout << "protocol_test: after 400 bots VmRSS_kB=" << selfRssKb() << "\n";
  }

  Journal::get().stop();
  std::cout << "protocol_test: ok\n";
  return 0;
}
