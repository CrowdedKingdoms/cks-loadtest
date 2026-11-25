#include "journal.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ckmc {

Journal& Journal::get() {
  // Leaked on purpose: a worker may still log while the process is exiting.
  static Journal* journal = new Journal();
  return *journal;
}

Journal::Journal() {
  writer_ = std::thread([this] { run(); });
}

void Journal::open(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "a");
  if (!f) throw std::runtime_error("cannot open journal " + path);
  std::lock_guard<std::mutex> io(ioMu_);
  if (journal_) std::fclose(journal_);
  journal_ = f;
  intervalMs_.store(kJournalIntervalMs);
}

std::shared_ptr<LogSink> Journal::openSink(const std::string& path) {
  if (path.empty() || path == "-" || path == "/dev/null") return nullptr;
  std::FILE* f = std::fopen(path.c_str(), "a");
  if (!f) return nullptr;
  return std::make_shared<LogSink>(f);
}

void Journal::line(const std::shared_ptr<LogSink>& sink, std::string text) {
  std::lock_guard<std::mutex> lock(qMu_);
  if (qBytes_ >= kMaxQueueBytes) {
    dropped_++;
    return;
  }
  qBytes_ += text.size() + sizeof(Entry);
  q_.push_back(Entry{sink, std::move(text)});
}

void Journal::drain() {
  std::lock_guard<std::mutex> io(ioMu_);
  std::vector<Entry> batch;
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(qMu_);
    batch.swap(q_);
    qBytes_ = 0;
    dropped = dropped_;
    dropped_ = 0;
  }
  if (dropped > 0) {
    std::fprintf(stderr, "ck-mc-load: log queue was full, %llu lines dropped\n",
                 static_cast<unsigned long long>(dropped));
  }
  if (batch.empty()) return;
  std::FILE* out = journal_ ? journal_ : stdout;
  std::vector<LogSink*> touched;
  for (Entry& e : batch) {
    std::fwrite(e.text.data(), 1, e.text.size(), out);
    std::fputc('\n', out);
    if (e.sink && e.sink->file) {
      std::fwrite(e.text.data(), 1, e.text.size(), e.sink->file);
      std::fputc('\n', e.sink->file);
      if (!e.sink->dirty) {
        e.sink->dirty = true;
        touched.push_back(e.sink.get());
      }
    }
  }
  std::fflush(out);
  for (LogSink* sink : touched) {
    std::fflush(sink->file);
    sink->dirty = false;
  }
}

void Journal::flush() { drain(); }

void Journal::run() {
  std::unique_lock<std::mutex> lock(wakeMu_);
  while (!stopping_) {
    wake_.wait_for(lock, std::chrono::milliseconds(intervalMs_.load()), [this] { return stopping_; });
    lock.unlock();
    drain();
    lock.lock();
  }
}

void Journal::stop() {
  {
    std::lock_guard<std::mutex> lock(wakeMu_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (writer_.joinable()) writer_.join();
  drain();
}

}  // namespace ckmc
