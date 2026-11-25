#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ckmc {

// One bot's own log file. Only the writer thread writes to it.
class LogSink {
 public:
  explicit LogSink(std::FILE* f) : file(f) {}
  ~LogSink() {
    if (file) std::fclose(file);
  }
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;

  std::FILE* file;
  bool dirty = false;
};

// Every log line of every bot goes through here. A worker only appends to a queue in
// memory. One writer thread does the file and stdout writes, so a slow disk or a pipe
// nobody reads cannot hold a worker.
class Journal {
 public:
  // The one journal of the process. It is never destroyed.
  static Journal& get();

  // --journal PATH. Bot lines go to this file and no longer to stdout. Throws when the
  // file cannot be opened.
  void open(const std::string& path);

  // A bot's own log file. Empty, `-` and `/dev/null` give no sink.
  std::shared_ptr<LogSink> openSink(const std::string& path);

  // Queue one line. Never does I/O. When the queue is full the line is dropped and counted.
  void line(const std::shared_ptr<LogSink>& sink, std::string text);

  // Write what is queued, on the calling thread. For tests and for the end of the process.
  void flush();

  // Stop the writer thread and write what is left. `flush` still works afterwards.
  void stop();

  static constexpr size_t kMaxQueueBytes = 16u * 1024u * 1024u;
  static constexpr int kStdoutIntervalMs = 100;
  static constexpr int kJournalIntervalMs = 500;

 private:
  Journal();
  void run();
  void drain();

  struct Entry {
    std::shared_ptr<LogSink> sink;
    std::string text;
  };

  // Guards the queue only. Never held during I/O.
  std::mutex qMu_;
  std::vector<Entry> q_;
  size_t qBytes_ = 0;
  uint64_t dropped_ = 0;

  // Held by whoever is writing: the writer thread, or a caller of flush().
  std::mutex ioMu_;
  std::FILE* journal_ = nullptr;

  std::mutex wakeMu_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::atomic<int> intervalMs_{kStdoutIntervalMs};
  std::thread writer_;
};

}  // namespace ckmc
