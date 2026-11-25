#include "control.hpp"
#include "fleet.hpp"
#include "journal.hpp"

#include <signal.h>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> gRun{true};

void onSignal(int) { gRun = false; }

void usage() {
  std::cerr
      << "ck-mc-load: many flying Minecraft 26.3 clients in one process\n"
      << "  --control host:port     line protocol: add, stop, stop-all, quit, stats, list,\n"
      << "                          bounds, rally, unrally, speed, altitude, grid\n"
      << "  --threads N             worker threads (default 1)\n"
      << "  --journal PATH          every bot's lines go to PATH, not to stdout\n"
      << "  --bounds minX,minZ,maxX,maxZ   wander turns back at this rectangle\n"
      << "  --grid originX,originZ,cellBlocks,cols,rows   count line crossings on this grid\n"
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

// Each bot is one socket. Ask for as many descriptors as the system allows this process.
void raiseFdLimit() {
  rlimit lim{};
  if (getrlimit(RLIMIT_NOFILE, &lim) != 0) return;
  rlim_t want = lim.rlim_max;
  if (want == RLIM_INFINITY || want > 1048576) want = 1048576;
  if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur >= want) return;
  rlim_t had = lim.rlim_cur;
  lim.rlim_cur = want;
  if (setrlimit(RLIMIT_NOFILE, &lim) == 0) return;
  // macOS refuses more than its per-process maximum even when the hard limit says otherwise.
  if (had < 10240 && want > 10240) {
    lim.rlim_cur = 10240;
    (void)setrlimit(RLIMIT_NOFILE, &lim);
  }
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
  raiseFdLimit();

  ckmc::WanderBox bounds;
  ckmc::Grid grid;
  int threads = 1;
  ckmc::BotSpec single;
  std::string control = argValue(args, "--control", "");
  try {
    std::string rawBounds = argValue(args, "--bounds", "");
    if (!rawBounds.empty()) {
      try {
        bounds = ckmc::parseBounds(ckmc::splitCommas(rawBounds));
      } catch (const std::exception&) {
        throw std::runtime_error("--bounds needs minX,minZ,maxX,maxZ");
      }
    }
    std::string rawGrid = argValue(args, "--grid", "");
    if (!rawGrid.empty()) {
      try {
        grid = ckmc::parseGrid(ckmc::splitCommas(rawGrid));
      } catch (const std::exception& e) {
        throw std::runtime_error(std::string("--grid needs originX,originZ,cellBlocks,cols,rows: ") + e.what());
      }
    }
    threads = std::stoi(argValue(args, "--threads", "1"));
    if (control.empty()) single = specFrom(args);
    std::string journal = argValue(args, "--journal", "");
    if (!journal.empty()) ckmc::Journal::get().open(journal);
  } catch (const std::exception& e) {
    std::cerr << "ck-mc-load: " << e.what() << "\n";
    return 2;
  }

  if (control.empty() && !has(args, "--host") && !has(args, "--name")) {
    usage();
    return 2;
  }

  ckmc::Fleet fleet(threads);
  fleet.start();
  // These reach every worker before any bot does: each worker reads its inbox in order.
  if (bounds.enabled) fleet.setBounds(bounds);
  if (grid.enabled) fleet.setGrid(grid);

  int rc = 0;
  if (control.empty()) {
    fleet.add(single);
    while (gRun.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (fleet.started() > 0 && fleet.finished() >= fleet.started()) break;
    }
  } else {
    ckmc::ControlServer server(fleet, gRun);
    try {
      server.listen(control);
      std::cerr << "ck-mc-load: control on " << control << "\n";
      server.start();
      // The control socket has its own threads. This one only waits for the end.
      while (gRun.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } catch (const std::exception& e) {
      std::cerr << "ck-mc-load: " << e.what() << "\n";
      rc = 2;
    }
    server.stop();
  }
  fleet.stop();
  ckmc::Journal::get().stop();
  return rc;
}
