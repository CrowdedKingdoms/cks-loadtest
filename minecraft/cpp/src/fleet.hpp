#pragma once

#include "bot.hpp"
#include "logic.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ckmc {

// Worker threads, each with its own wait loop. Adds are round-robin so one process holds
// many TCP clients. A bot belongs to one worker for its whole life.
//
// After start() every method except stop() may be called from any thread. None of them
// waits for a worker: a change is a message in the worker's inbox, and a read is from
// atomics the workers publish.
class Fleet {
 public:
  explicit Fleet(int threads);
  ~Fleet();

  void start();
  void stop();
  void add(BotSpec spec);
  void stopBot(const std::string& name);
  void stopAll();

  // The wander box of every bot that has none of its own, now and later.
  void setBounds(const WanderBox& box);
  // Wander speed for every bot. Off gives each bot its own speed back.
  void setSpeed(bool on, double bps);
  // Absolute feet Y for every bot. Off goes back to the clearance above the last server Y.
  void setAltitude(bool on, double y);
  void setGrid(const Grid& grid);
  // Sends the first `count` bots in play (in the order they were added; all when count < 0)
  // to the point. Returns how many were picked.
  size_t rally(double x, double z, double radius, long count);
  void unrally();

  // Counts by phase and the process totals. Reading it resets the tick maximum.
  FleetStats stats();
  // The `list` reply, without the newline. Throws std::runtime_error on an unknown phase.
  std::string list(const std::string& phase, size_t cap = kListCap);

  // Bots added, and how many of those are closed.
  int started() const;
  int finished() const;

  static constexpr size_t kListCap = 64 * 1024;
  // Closed bots remembered by name. Older ones stay in the counts only.
  static constexpr size_t kKeepClosed = 4096;

 private:
  struct Worker;
  void pruneLocked();

  int threads_;
  std::atomic<bool> run_{false};
  std::atomic<int> next_{0};
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threadsPool_;

  // The registry. Only control threads and main take this mutex. A worker never does:
  // it writes the atomics of the views it was handed.
  mutable std::mutex viewsMu_;
  std::vector<std::shared_ptr<BotView>> views_;
  uint64_t nextSeq_ = 1;
  uint64_t closedDropped_ = 0;
  size_t pruneAt_ = 2 * kKeepClosed;
};

}  // namespace ckmc
