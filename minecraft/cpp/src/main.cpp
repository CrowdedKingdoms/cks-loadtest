#include "fleet.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> gRun{true};

void onSignal(int) { gRun = false; }

void usage() {
  std::cerr
      << "ck-mc-load — many flying Minecraft 26.3 clients in one process\n"
      << "  --control host:port     line protocol: add, stop, stop-all, quit\n"
      << "  --threads N             epoll workers (default 1)\n"
      << "  --name NAME --host H --port P   one bot, no control socket\n"
      << "  --path seam|triangle|wander     default seam\n"
      << "  --waypoints x,y,z,side;...      triangle only\n"
      << "  --bps N                 wander speed, blocks per second (default 2)\n"
      << "  --clearance N           blocks above the last server feet Y (default 8)\n"
      << "  --max-seconds N         default 180\n"
      << "  --log PATH              also append this bot's lines to PATH\n";
}

std::string argValue(const std::vector<std::string>& args, const std::string& key, const std::string& fallback) {
  for (size_t i = 0; i + 1 < args.size(); i++) {
    if (args[i] == key) return args[i + 1];
  }
  return fallback;
}

bool has(const std::vector<std::string>& args, const std::string& key) {
  for (const auto& a : args)
    if (a == key) return true;
  return false;
}

ckmc::BotSpec specFrom(const std::vector<std::string>& args) {
  ckmc::BotSpec s;
  s.name = argValue(args, "--name", "Load1");
  s.host = argValue(args, "--host", "127.0.0.1");
  s.port = std::stoi(argValue(args, "--port", "25565"));
  s.path = argValue(args, "--path", "seam");
  s.waypoints = argValue(args, "--waypoints", "-");
  s.bps = std::stod(argValue(args, "--bps", "2"));
  s.clearance = std::stod(argValue(args, "--clearance", "8"));
  s.maxSeconds = std::stoi(argValue(args, "--max-seconds", "180"));
  s.logPath = argValue(args, "--log", "");
  return s;
}

ckmc::BotSpec specFromAdd(const std::vector<std::string>& tok) {
  // add name host port path bps clearance maxSeconds waypoints logpath
  if (tok.size() != 10) throw std::runtime_error("add needs 9 fields");
  ckmc::BotSpec s;
  s.name = tok[1];
  s.host = tok[2];
  s.port = std::stoi(tok[3]);
  s.path = tok[4];
  s.bps = std::stod(tok[5]);
  s.clearance = std::stod(tok[6]);
  s.maxSeconds = std::stoi(tok[7]);
  s.waypoints = tok[8];
  s.logPath = tok[9];
  return s;
}

int listenOn(const std::string& where) {
  auto colon = where.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("control bind needs host:port");
  std::string host = where.substr(0, colon);
  int port = std::stoi(where.substr(colon + 1));
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw std::runtime_error("socket");
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    throw std::runtime_error("control host must be an ipv4 address");
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 1) < 0) {
    ::close(fd);
    throw std::runtime_error("bind control");
  }
  return fd;
}

void reply(int fd, const std::string& line) {
  std::string out = line + "\n";
  (void)::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (has(args, "--help") || has(args, "-h")) {
    usage();
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  int threads = std::stoi(argValue(args, "--threads", "1"));
  ckmc::Fleet fleet(threads);
  fleet.start();

  std::string control = argValue(args, "--control", "");
  if (control.empty()) {
    if (!has(args, "--host") && !has(args, "--name")) {
      usage();
      fleet.stop();
      return 2;
    }
    fleet.add(specFrom(args));
    while (gRun.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (fleet.started() > 0 && fleet.finished() >= fleet.started()) break;
    }
    fleet.stop();
    return 0;
  }

  int lst = -1;
  try {
    lst = listenOn(control);
  } catch (const std::exception& e) {
    std::cerr << "ck-mc-load: " << e.what() << "\n";
    fleet.stop();
    return 2;
  }
  std::cerr << "ck-mc-load: control on " << control << "\n";
  while (gRun.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(lst, &fds);
    timeval tv{0, 200000};
    int rc = select(lst + 1, &fds, nullptr, nullptr, &tv);
    if (rc <= 0) continue;
    int client = ::accept(lst, nullptr, nullptr);
    if (client < 0) continue;
    std::string buf;
    bool quit = false;
    while (gRun.load() && !quit) {
      char tmp[1024];
      fd_set cfds;
      FD_ZERO(&cfds);
      FD_SET(client, &cfds);
      timeval ctv{0, 200000};
      int cr = select(client + 1, &cfds, nullptr, nullptr, &ctv);
      if (cr <= 0) continue;
      ssize_t n = ::recv(client, tmp, sizeof(tmp), 0);
      if (n <= 0) break;
      buf.append(tmp, tmp + n);
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::vector<std::string> tok;
        std::string word;
        while (ss >> word) tok.push_back(word);
        if (tok.empty()) continue;
        try {
          if (tok[0] == "add") {
            fleet.add(specFromAdd(tok));
            reply(client, "ok");
          } else if (tok[0] == "stop" && tok.size() == 2) {
            fleet.stopBot(tok[1]);
            reply(client, "ok");
          } else if (tok[0] == "stop-all") {
            fleet.stopAll();
            reply(client, "ok");
          } else if (tok[0] == "quit") {
            reply(client, "ok");
            quit = true;
            gRun = false;
          } else {
            reply(client, "err unknown");
          }
        } catch (const std::exception& e) {
          reply(client, std::string("err ") + e.what());
        }
      }
    }
    ::close(client);
  }
  ::close(lst);
  fleet.stop();
  return 0;
}
