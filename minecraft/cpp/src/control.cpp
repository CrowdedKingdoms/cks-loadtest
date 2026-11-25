#include "control.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ckmc {

std::vector<std::string> takeLines(std::string& buf) {
  std::vector<std::string> lines;
  size_t from = 0;
  for (;;) {
    size_t nl = buf.find('\n', from);
    if (nl == std::string::npos) break;
    std::string line = buf.substr(from, nl - from);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    from = nl + 1;
  }
  buf.erase(0, from);
  return lines;
}

std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
    if (i > start) fields.push_back(line.substr(start, i - start));
  }
  return fields;
}

std::vector<std::string> splitCommas(const std::string& text) {
  std::vector<std::string> parts;
  size_t from = 0;
  for (;;) {
    size_t comma = text.find(',', from);
    if (comma == std::string::npos) {
      parts.push_back(text.substr(from));
      break;
    }
    parts.push_back(text.substr(from, comma - from));
    from = comma + 1;
  }
  return parts;
}

double parseNumber(const std::string& text) {
  if (text.empty()) throw std::runtime_error("bad number");
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) throw std::runtime_error("bad number " + text);
  return v;
}

long parseWhole(const std::string& text) {
  if (text.empty()) throw std::runtime_error("bad number");
  errno = 0;
  char* end = nullptr;
  long v = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) throw std::runtime_error("bad whole number " + text);
  return v;
}

BotSpec parseAdd(const std::vector<std::string>& tok) {
  // add name host port path bps clearance maxSeconds waypoints logpath
  if (tok.size() != 10) throw std::runtime_error("add needs 9 fields");
  BotSpec s;
  s.name = tok[1];
  s.host = tok[2];
  long port = parseWhole(tok[3]);
  if (port < 1 || port > 65535) throw std::runtime_error("bad port " + tok[3]);
  s.port = static_cast<int>(port);
  s.path = tok[4];
  s.bps = parseNumber(tok[5]);
  s.clearance = parseNumber(tok[6]);
  long maxSeconds = parseWhole(tok[7]);
  if (maxSeconds < 0 || maxSeconds > 100000000L) throw std::runtime_error("bad maxSeconds " + tok[7]);
  s.maxSeconds = static_cast<int>(maxSeconds);
  s.waypoints = tok[8];
  s.logPath = tok[9];
  return s;
}

WanderBox parseBounds(const std::vector<std::string>& f) {
  if (f.size() != 4) throw std::runtime_error("bounds needs minX minZ maxX maxZ");
  WanderBox box;
  box.minX = parseNumber(f[0]);
  box.minZ = parseNumber(f[1]);
  box.maxX = parseNumber(f[2]);
  box.maxZ = parseNumber(f[3]);
  if (box.minX >= box.maxX || box.minZ >= box.maxZ) throw std::runtime_error("bounds needs min below max");
  box.enabled = true;
  return box;
}

Grid parseGrid(const std::vector<std::string>& f) {
  if (f.size() != 5) throw std::runtime_error("grid needs originX originZ cellBlocks cols rows");
  Grid grid;
  grid.originX = parseNumber(f[0]);
  grid.originZ = parseNumber(f[1]);
  grid.cell = parseNumber(f[2]);
  long cols = parseWhole(f[3]);
  long rows = parseWhole(f[4]);
  if (!(grid.cell > 0.0)) throw std::runtime_error("grid cellBlocks must be above 0");
  if (cols < 1 || rows < 1 || cols > 10000 || rows > 10000) throw std::runtime_error("grid cols and rows are 1 to 10000");
  grid.cols = static_cast<int>(cols);
  grid.rows = static_cast<int>(rows);
  grid.enabled = true;
  return grid;
}

std::string ControlHandler::execute(const std::string& line, bool& quit) {
  quit = false;
  std::vector<std::string> tok = splitFields(line);
  if (tok.empty()) return {};
  const std::string& cmd = tok[0];
  try {
    if (cmd == "add") {
      fleet_.add(parseAdd(tok));
      return "ok";
    }
    if (cmd == "stop" && tok.size() == 2) {
      fleet_.stopBot(tok[1]);
      return "ok";
    }
    if (cmd == "stop-all") {
      fleet_.stopAll();
      return "ok";
    }
    if (cmd == "quit") {
      quit = true;
      return "ok";
    }
    if (cmd == "stats") {
      return formatStats(fleet_.stats());
    }
    if (cmd == "list") {
      if (tok.size() != 2) throw std::runtime_error("list needs a phase");
      return fleet_.list(tok[1]);
    }
    if (cmd == "bounds") {
      if (tok.size() == 2 && tok[1] == "off") {
        fleet_.setBounds(WanderBox{});
        return "ok";
      }
      fleet_.setBounds(parseBounds(std::vector<std::string>(tok.begin() + 1, tok.end())));
      return "ok";
    }
    if (cmd == "rally") {
      if (tok.size() != 4 && tok.size() != 5) throw std::runtime_error("rally needs x z radius [count]");
      double x = parseNumber(tok[1]);
      double z = parseNumber(tok[2]);
      double radius = parseNumber(tok[3]);
      if (!(radius > 0.0)) throw std::runtime_error("rally radius must be above 0");
      long count = -1;
      if (tok.size() == 5) {
        count = parseWhole(tok[4]);
        if (count < 0) throw std::runtime_error("rally count must not be negative");
      }
      size_t picked = fleet_.rally(x, z, radius, count);
      return "ok rallied=" + std::to_string(picked);
    }
    if (cmd == "unrally") {
      fleet_.unrally();
      return "ok";
    }
    if (cmd == "speed") {
      if (tok.size() != 2) throw std::runtime_error("speed needs bps or off");
      if (tok[1] == "off") {
        fleet_.setSpeed(false, 0);
        return "ok";
      }
      double bps = parseNumber(tok[1]);
      if (bps < 0.05 || bps > 100.0) throw std::runtime_error("speed is 0.05 to 100 blocks per second");
      fleet_.setSpeed(true, bps);
      return "ok";
    }
    if (cmd == "altitude") {
      if (tok.size() != 2) throw std::runtime_error("altitude needs y or off");
      if (tok[1] == "off") {
        fleet_.setAltitude(false, 0);
        return "ok";
      }
      double y = parseNumber(tok[1]);
      if (y < -2048.0 || y > 100000.0) throw std::runtime_error("altitude is -2048 to 100000");
      fleet_.setAltitude(true, y);
      return "ok";
    }
    if (cmd == "grid") {
      if (tok.size() == 2 && tok[1] == "off") {
        fleet_.setGrid(Grid{});
        return "ok";
      }
      fleet_.setGrid(parseGrid(std::vector<std::string>(tok.begin() + 1, tok.end())));
      return "ok";
    }
    return "err unknown";
  } catch (const std::exception& e) {
    return std::string("err ") + e.what();
  }
}

bool ControlHandler::feed(std::string& buf, std::string& out) {
  for (const std::string& line : takeLines(buf)) {
    bool quit = false;
    std::string reply = execute(line, quit);
    if (!reply.empty()) {
      out += reply;
      out += '\n';
    }
    if (quit) return true;
  }
  return false;
}

namespace {

bool sendAll(int fd, const std::string& bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
#if defined(__APPLE__)
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
#else
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      // Includes the send timeout: a client that stopped reading its replies is dropped.
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

ControlServer::ControlServer(Fleet& fleet, std::atomic<bool>& run) : fleet_(fleet), run_(run) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::listen(const std::string& where) {
  auto colon = where.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("control bind needs host:port");
  std::string host = where.substr(0, colon);
  long port = parseWhole(where.substr(colon + 1));
  if (port < 0 || port > 65535) throw std::runtime_error("control port is 0 to 65535");
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket");
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    throw std::runtime_error("control host must be an ipv4 address");
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, kBacklog) < 0) {
    ::close(fd);
    throw std::runtime_error("bind control");
  }
  // accept() after poll() must not block when the client has already gone.
  int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) port_ = ntohs(addr.sin_port);
  lst_ = fd;
}

void ControlServer::start() {
  if (lst_ < 0 || acceptor_.joinable()) return;
  acceptor_ = std::thread([this] { acceptLoop(); });
}

void ControlServer::reap(bool all) {
  for (auto it = conns_.begin(); it != conns_.end();) {
    Conn& conn = **it;
    if (all || conn.done.load()) {
      if (conn.thread.joinable()) conn.thread.join();
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
}

void ControlServer::acceptLoop() {
  while (alive()) {
    reap(false);
    pollfd p{};
    p.fd = lst_;
    p.events = POLLIN;
    int rc = ::poll(&p, 1, 200);
    if (rc <= 0) continue;
    int client = ::accept(lst_, nullptr, nullptr);
    if (client < 0) continue;
    ::fcntl(client, F_SETFD, FD_CLOEXEC);
    // Some systems hand the listener's nonblocking flag to the accepted socket. A
    // connection thread wants blocking calls with timeouts.
    int fl = ::fcntl(client, F_GETFL, 0);
    if (fl >= 0) ::fcntl(client, F_SETFL, fl & ~O_NONBLOCK);
    timeval sendWait{};
    sendWait.tv_sec = 5;
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &sendWait, sizeof(sendWait));
#if defined(__APPLE__)
    int nosig = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    if (conns_.size() >= kMaxConnections) {
      sendAll(client, "err too many control connections\n");
      ::close(client);
      continue;
    }
    auto conn = std::make_unique<Conn>();
    conn->fd = client;
    Conn* raw = conn.get();
    try {
      conn->thread = std::thread([this, raw] { serve(*raw); });
    } catch (const std::system_error&) {
      sendAll(client, "err cannot start a thread\n");
      ::close(client);
      continue;
    }
    conns_.push_back(std::move(conn));
  }
}

void ControlServer::serve(Conn& conn) {
  ControlHandler handler(fleet_);
  std::string buf;
  std::string out;
  char tmp[16384];
  while (alive()) {
    pollfd p{};
    p.fd = conn.fd;
    p.events = POLLIN;
    int rc = ::poll(&p, 1, 200);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) continue;
    ssize_t n = ::recv(conn.fd, tmp, sizeof(tmp), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    buf.append(tmp, static_cast<size_t>(n));
    out.clear();
    bool quit = handler.feed(buf, out);
    if (!sendAll(conn.fd, out)) break;
    if (quit) {
      run_.store(false);
      break;
    }
    if (buf.size() > kMaxLineBytes) {
      sendAll(conn.fd, "err line too long\n");
      break;
    }
  }
  ::close(conn.fd);
  conn.done.store(true);
}

void ControlServer::stop() {
  stop_.store(true);
  if (acceptor_.joinable()) acceptor_.join();
  reap(true);
  if (lst_ >= 0) {
    ::close(lst_);
    lst_ = -1;
  }
}

}  // namespace ckmc
