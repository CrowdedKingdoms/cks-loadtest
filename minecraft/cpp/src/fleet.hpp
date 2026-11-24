#pragma once

#include "bot.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ckmc {

// epoll workers. Adds are round-robin so one process holds many TCP clients.
class Fleet {
 public:
  explicit Fleet(int threads);
  ~Fleet();

  void start();
  void stop();
  void add(BotSpec spec);
  void stopBot(const std::string& name);
  void stopAll();
  // Bots the workers have accepted, and how many of those are finished.
  int started() const;
  int finished() const;

 private:
  struct Worker;
  int threads_;
  std::atomic<bool> run_{false};
  std::atomic<int> next_{0};
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threadsPool_;
};

}  // namespace ckmc
