#include "bot.hpp"
#include "buf.hpp"
#include "fleet.hpp"
#include "PacketIds.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace ckmc;

static void fail(const std::string& why) {
  std::cerr << "protocol_test: " << why << "\n";
  std::exit(1);
}

static int listenEphemeral(int& port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) fail("socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) fail("bind");
  if (listen(fd, 16) < 0) fail("listen");
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) fail("getsockname");
  port = ntohs(addr.sin_port);
  return fd;
}

static void sendAll(int fd, const std::vector<uint8_t>& bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
    if (n < 0) fail("send");
    off += static_cast<size_t>(n);
  }
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
      fail("recv");
    }
    if (n == 0) return false;
    acc.insert(acc.end(), tmp, tmp + n);
  }
  return false;
}

struct Seen {
  bool abilities = false;
  bool flyingBit = false;
  bool moveAir = false;
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
  if (id == pid(SbPlay::MOVE_PLAYER_POS_ROT) && body.b.size() >= 8 * 3 + 4) {
    Buf b;
    b.b = body.b;
    b.rF64();
    seen.moveY = b.rF64();
    b.rF64();
    b.rF32();
    b.rF32();
    uint8_t flags = b.rU8();
    if (b.ok && flags == 0) seen.moveAir = true;
  }
}

static void serveOne(int client, Seen& seen, bool playLongEnoughForPunch) {
  timeval tv{};
  tv.tv_sec = 5;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::vector<uint8_t> acc;
  std::vector<uint8_t> packet;
  int compression = -1;
  if (!readFrame(client, acc, -1, packet)) fail("handshake");
  if (!readFrame(client, acc, -1, packet)) fail("hello");
  Buf comp;
  comp.wVar(256);
  sendAll(client, encodeFrame(-1, pid(CbLogin::LOGIN_COMPRESSION), comp));
  compression = 256;
  sendAll(client, encodeFrame(compression, pid(CbLogin::LOGIN_FINISHED), Buf{}));
  bool info = false;
  bool ack = false;
  bool brand = false;
  for (int i = 0; i < 6 && !(info && ack && brand); i++) {
    if (!readFrame(client, acc, compression, packet)) fail("config prelude");
    Buf body;
    int id = packetIdOf(packet, body);
    if (id == pid(SbConfig::CLIENT_INFORMATION)) info = true;
    if (id == pid(SbLogin::LOGIN_ACKNOWLEDGED)) ack = true;
    if (id == pid(SbConfig::CUSTOM_PAYLOAD)) brand = true;
  }
  if (!info || !ack || !brand) fail("missing login ack, brand, or client information");
  sendAll(client, encodeFrame(compression, pid(CbConfig::SELECT_KNOWN_PACKS), Buf{}));
  sendAll(client, encodeFrame(compression, pid(CbConfig::CODE_OF_CONDUCT), Buf{}));
  // Those two answers are optional for the rest of the test. Don't block on a third.
  timeval quick{};
  quick.tv_usec = 300000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &quick, sizeof(quick));
  for (int i = 0; i < 4; i++) {
    if (!readFrame(client, acc, compression, packet)) break;
  }
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sendAll(client, encodeFrame(compression, pid(CbConfig::FINISH_CONFIGURATION), Buf{}));
  bool finish = false;
  for (int i = 0; i < 4 && !finish; i++) {
    if (!readFrame(client, acc, compression, packet)) fail("no finish configuration");
    Buf body;
    int id = packetIdOf(packet, body);
    if (id == pid(SbConfig::FINISH_CONFIGURATION)) finish = true;
  }
  if (!finish) fail("client did not finish configuration");
  sendAll(client, encodeFrame(compression, pid(CbPlay::LOGIN), Buf{}));
  Buf pos;
  pos.wVar(7);
  pos.wF64(1990.5);
  pos.wF64(64.0);
  pos.wF64(-55.0);
  pos.wF64(0);
  pos.wF64(0);
  pos.wF64(0);
  pos.wF32(0);
  pos.wF32(0);
  pos.wI32(0);
  sendAll(client, encodeFrame(compression, pid(CbPlay::PLAYER_POSITION), pos));

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(playLongEnoughForPunch ? 3 : 2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!readFrame(client, acc, compression, packet)) break;
    Buf body;
    int id = packetIdOf(packet, body);
    if (id >= 0) note(seen, id, body);
    if (seen.loaded && seen.accept && seen.abilities && seen.flyingBit && seen.moveAir && seen.forward &&
        (!playLongEnoughForPunch || (seen.sprint && seen.punch))) {
      break;
    }
  }
}

int main() {
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
  std::vector<uint8_t> back;
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
  spec.maxSeconds = 8;
  spec.logPath = "/tmp/ck-mc-load-protocol-test.log";
  std::remove(spec.logPath.c_str());
  fleet.add(spec);

  sockaddr_in clientAddr{};
  socklen_t clen = sizeof(clientAddr);
  int client = ::accept(lst, reinterpret_cast<sockaddr*>(&clientAddr), &clen);
  if (client < 0) fail("accept");
  Seen seen;
  serveOne(client, seen, true);
  ::close(client);
  if (!seen.loaded || !seen.accept) fail("did not enter the world");
  if (!seen.flyingBit) fail("flying ability bit not set");
  if (!seen.moveAir) fail("move packet was not airborne");
  if (seen.moveY < 71.0 || seen.moveY > 73.0) fail("fly height " + std::to_string(seen.moveY));
  if (!seen.forward) fail("no forward input");
  if (!seen.sprint) fail("no sprint input");
  if (!seen.punch) fail("no arm swing");

  std::ifstream log(spec.logPath);
  std::string all((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  if (all.find("phase=play") == std::string::npos) fail("missing phase=play log");
  if (all.find(" at ") == std::string::npos) fail("missing server position log");

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
    many.maxSeconds = 12;
    fleet.add(std::move(many));
  }
  for (int i = 0; i < 10; i++) {
    int c = ::accept(lst2, nullptr, nullptr);
    if (c < 0) fail("accept ten");
    servers.emplace_back([c, &joined] {
      Seen local;
      serveOne(c, local, false);
      if (local.moveAir && local.flyingBit) joined.fetch_add(1);
      ::close(c);
    });
  }
  for (auto& t : servers) t.join();
  if (joined.load() != 10) fail("ten-bot join count " + std::to_string(joined.load()));

  std::ifstream status("/proc/self/status");
  std::string line;
  long rssKb = -1;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      rssKb = std::strtol(line.c_str() + 6, nullptr, 10);
    }
  }
  std::cout << "protocol_test: ten bots VmRSS_kB=" << rssKb
            << " java_one_bot_heap_kB=" << (512 * 1024) << "\n";
  if (rssKb < 0) fail("no VmRSS");
  if (rssKb >= 512 * 1024) fail("rss is not below one Java walker heap");

  fleet.stop();
  ::close(lst);
  ::close(lst2);
  std::cout << "protocol_test: ok\n";
  return 0;
}
