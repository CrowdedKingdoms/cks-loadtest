#pragma once

#include "fleet.hpp"

#include <atomic>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ckmc {

// Takes every complete line off the front of `buf`, without its line end (\n or \r\n).
// A last line with no line end yet stays in `buf`.
std::vector<std::string> takeLines(std::string& buf);

// Fields of one line, split on spaces and tabs.
std::vector<std::string> splitFields(const std::string& line);
std::vector<std::string> splitCommas(const std::string& text);

// These throw std::runtime_error with the text of the `err` reply.
double parseNumber(const std::string& text);
long parseWhole(const std::string& text);
// `fields` is the whole line: "add" and the nine fields after it.
BotSpec parseAdd(const std::vector<std::string>& fields);
// minX minZ maxX maxZ
WanderBox parseBounds(const std::vector<std::string>& four);
// originX originZ cellBlocks cols rows
Grid parseGrid(const std::vector<std::string>& five);

// Runs control lines against a fleet. No state of its own: several connections may each have one.
class ControlHandler {
 public:
  explicit ControlHandler(Fleet& fleet) : fleet_(fleet) {}

  // The reply to one line, without the newline. Empty for a blank line, which gets no reply.
  std::string execute(const std::string& line, bool& quit);

  // Runs every complete line in `buf`, in order, and appends one reply line per command to
  // `out`. Returns true when one of them was `quit`; the lines after it are not run.
  bool feed(std::string& buf, std::string& out);

 private:
  Fleet& fleet_;
};

// The control socket. One thread accepts and does nothing else. Each connection has its own
// thread, so a slow or silent client holds nobody else up.
class ControlServer {
 public:
  ControlServer(Fleet& fleet, std::atomic<bool>& run);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Binds host:port (IPv4). Throws std::runtime_error.
  void listen(const std::string& hostPort);
  // The bound port, for a caller that asked for port 0.
  int port() const { return port_; }
  void start();
  // Ends the accept thread and every connection thread. `quit` on a connection clears `run`.
  void stop();

  static constexpr int kBacklog = 64;
  static constexpr size_t kMaxConnections = 64;
  static constexpr size_t kMaxLineBytes = 64 * 1024;

 private:
  struct Conn {
    int fd = -1;
    std::atomic<bool> done{false};
    std::thread thread;
  };

  void acceptLoop();
  void serve(Conn& conn);
  void reap(bool all);
  bool alive() const { return run_.load() && !stop_.load(); }

  Fleet& fleet_;
  std::atomic<bool>& run_;
  std::atomic<bool> stop_{false};
  int lst_ = -1;
  int port_ = 0;
  std::thread acceptor_;
  // Touched by the accept thread, and by stop() once that thread has ended.
  std::list<std::unique_ptr<Conn>> conns_;
};

}  // namespace ckmc
