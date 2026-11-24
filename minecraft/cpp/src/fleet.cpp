#include "fleet.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace ckmc {
namespace {

struct Cmd {
  enum class Kind { Add, StopOne, StopAll } kind;
  BotSpec spec;
  std::string name;
};

}  // namespace

struct Fleet::Worker {
  int ep = -1;
  int wake = -1;
  std::mutex mu;
  std::vector<Cmd> inbox;
  std::vector<std::unique_ptr<Bot>> bots;
  std::atomic<bool>* run = nullptr;
  std::atomic<int> started{0};
  std::atomic<int> finished{0};

  void push(Cmd cmd) {
    {
      std::lock_guard<std::mutex> lock(mu);
      inbox.push_back(std::move(cmd));
    }
    uint64_t one = 1;
    if (wake >= 0) (void)::write(wake, &one, sizeof(one));
  }

  void arm(Bot& bot) {
    if (bot.fd < 0) return;
    epoll_event ev{};
    ev.data.fd = bot.fd;
    ev.events = EPOLLIN;
    if (bot.wantsWrite()) ev.events |= EPOLLOUT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, bot.fd, &ev) < 0) {
      epoll_ctl(ep, EPOLL_CTL_ADD, bot.fd, &ev);
    }
  }

  void dropFd(int fd) {
    if (fd >= 0) epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
  }

  Bot* findFd(int fd) {
    for (auto& b : bots) {
      if (b && b->fd == fd) return b.get();
    }
    return nullptr;
  }

  void drain() {
    std::vector<Cmd> batch;
    {
      std::lock_guard<std::mutex> lock(mu);
      batch.swap(inbox);
    }
    for (auto& cmd : batch) {
      if (cmd.kind == Cmd::Kind::StopAll) {
        for (auto& b : bots) {
          if (b && !b->dead) {
            dropFd(b->fd);
            b->stop("stopped");
          }
        }
      } else if (cmd.kind == Cmd::Kind::StopOne) {
        for (auto& b : bots) {
          if (b && b->name() == cmd.name && !b->dead) {
            dropFd(b->fd);
            b->stop("stopped");
          }
        }
      } else if (cmd.kind == Cmd::Kind::Add) {
        auto bot = std::make_unique<Bot>(std::move(cmd.spec));
        if (!bot->start()) {
          bots.push_back(std::move(bot));
          continue;
        }
        arm(*bot);
        bots.push_back(std::move(bot));
      }
    }
  }

  void loop() {
    auto next = std::chrono::steady_clock::now();
    while (run->load()) {
      drain();
      auto now = std::chrono::steady_clock::now();
      if (now >= next) {
        for (auto& b : bots) {
          if (!b || b->dead) continue;
          int before = b->fd;
          b->tick();
          if (b->dead) dropFd(before);
          else arm(*b);
        }
        next += std::chrono::milliseconds(50);
        if (next < now) next = now + std::chrono::milliseconds(50);
        int total = 0;
        int done = 0;
        for (auto& b : bots) {
          if (!b) continue;
          total++;
          if (b->dead) done++;
        }
        started.store(total);
        finished.store(done);
      }
      int waitMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count());
      if (waitMs < 0) waitMs = 0;
      if (waitMs > 50) waitMs = 50;
      epoll_event evs[64];
      int n = epoll_wait(ep, evs, 64, waitMs);
      for (int i = 0; i < n; i++) {
        int fd = evs[i].data.fd;
        if (fd == wake) {
          uint64_t junk;
          while (::read(wake, &junk, sizeof(junk)) > 0) {
          }
          continue;
        }
        Bot* bot = findFd(fd);
        if (!bot || bot->dead) continue;
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
          bot->stop("socket error");
          dropFd(fd);
          continue;
        }
        if (evs[i].events & EPOLLOUT) bot->onWritable();
        if (!bot->dead && (evs[i].events & EPOLLIN)) bot->onReadable();
        if (bot->dead) dropFd(fd);
        else arm(*bot);
      }
    }
  }

};

Fleet::Fleet(int threads) : threads_(std::max(1, threads)) {}

Fleet::~Fleet() { stop(); }

void Fleet::start() {
  if (run_.exchange(true)) return;
  for (int i = 0; i < threads_; i++) {
    auto w = std::make_unique<Worker>();
    w->run = &run_;
    w->ep = epoll_create1(EPOLL_CLOEXEC);
    w->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event ev{};
    ev.data.fd = w->wake;
    ev.events = EPOLLIN;
    epoll_ctl(w->ep, EPOLL_CTL_ADD, w->wake, &ev);
    workers_.push_back(std::move(w));
  }
  for (auto& w : workers_) {
    Worker* raw = w.get();
    threadsPool_.emplace_back([raw] { raw->loop(); });
  }
}

void Fleet::stop() {
  if (!run_.exchange(false)) return;
  for (auto& w : workers_) w->push(Cmd{Cmd::Kind::StopAll, {}, {}});
  for (auto& t : threadsPool_) {
    if (t.joinable()) t.join();
  }
  threadsPool_.clear();
  workers_.clear();
}

void Fleet::add(BotSpec spec) {
  int i = next_.fetch_add(1) % threads_;
  workers_[static_cast<size_t>(i)]->push(Cmd{Cmd::Kind::Add, std::move(spec), {}});
}

void Fleet::stopBot(const std::string& name) {
  for (auto& w : workers_) w->push(Cmd{Cmd::Kind::StopOne, {}, name});
}

void Fleet::stopAll() {
  for (auto& w : workers_) w->push(Cmd{Cmd::Kind::StopAll, {}, {}});
}

int Fleet::started() const {
  int n = 0;
  for (auto& w : workers_) n += w->started.load();
  return n;
}

int Fleet::finished() const {
  int n = 0;
  for (auto& w : workers_) n += w->finished.load();
  return n;
}

}  // namespace ckmc
