#include "fleet.hpp"

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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
#if defined(__APPLE__)
  int wakeWrite = -1;
#endif
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
#if defined(__APPLE__)
    char one = 1;
    if (wakeWrite >= 0) {
      for (;;) {
        ssize_t n = ::write(wakeWrite, &one, 1);
        if (n >= 0 || errno != EINTR) break;
      }
    }
#else
    uint64_t one = 1;
    if (wake >= 0) (void)::write(wake, &one, sizeof(one));
#endif
  }

  void arm(Bot& bot) {
    if (bot.fd < 0) return;
#if defined(__APPLE__)
    struct kevent ch;
    EV_SET(&ch, bot.fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    (void)::kevent(ep, &ch, 1, nullptr, 0, nullptr);
    if (bot.wantsWrite()) {
      EV_SET(&ch, bot.fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    } else {
      EV_SET(&ch, bot.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    (void)::kevent(ep, &ch, 1, nullptr, 0, nullptr);
#else
    epoll_event ev{};
    ev.data.fd = bot.fd;
    ev.events = EPOLLIN;
    if (bot.wantsWrite()) ev.events |= EPOLLOUT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, bot.fd, &ev) < 0) {
      epoll_ctl(ep, EPOLL_CTL_ADD, bot.fd, &ev);
    }
#endif
  }

  void dropFd(int fd) {
    if (fd < 0) return;
#if defined(__APPLE__)
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)::kevent(ep, ch, 2, nullptr, 0, nullptr);
#else
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
#endif
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
          if (b->fd != before) dropFd(before);
          if (b->dead) dropFd(b->fd);
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
#if defined(__APPLE__)
      struct timespec ts;
      ts.tv_sec = waitMs / 1000;
      ts.tv_nsec = static_cast<long>(waitMs % 1000) * 1000000L;
      struct kevent evs[64];
      int n = ::kevent(ep, nullptr, 0, evs, 64, &ts);
      struct Ready {
        int fd;
        bool in;
        bool out;
        bool err;
      };
      Ready ready[64];
      int readyN = 0;
      auto mark = [&](int fd, bool in, bool out, bool err) {
        for (int i = 0; i < readyN; i++) {
          if (ready[i].fd == fd) {
            ready[i].in = ready[i].in || in;
            ready[i].out = ready[i].out || out;
            ready[i].err = ready[i].err || err;
            return;
          }
        }
        if (readyN < 64) ready[readyN++] = Ready{fd, in, out, err};
      };
      if (n > 0) {
        for (int i = 0; i < n; i++) {
          int fd = static_cast<int>(evs[i].ident);
          if (fd == wake) {
            char junk[64];
            while (true) {
              ssize_t got = ::read(wake, junk, sizeof(junk));
              if (got > 0) continue;
              if (got < 0 && errno == EINTR) continue;
              break;
            }
            continue;
          }
          bool err = (evs[i].flags & (EV_ERROR | EV_EOF)) != 0;
          bool in = evs[i].filter == EVFILT_READ;
          bool out = evs[i].filter == EVFILT_WRITE;
          mark(fd, in, out, err);
        }
      }
      for (int i = 0; i < readyN; i++) {
        int fd = ready[i].fd;
        Bot* bot = findFd(fd);
        if (!bot || bot->dead) continue;
        if (ready[i].err) {
          bot->stop("socket error");
          dropFd(fd);
          continue;
        }
        if (ready[i].out) bot->onWritable();
        if (!bot->dead && bot->fd == fd && ready[i].in) bot->onReadable();
        if (bot->fd != fd) dropFd(fd);
        if (bot->dead) dropFd(bot->fd);
        else arm(*bot);
      }
#else
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
        if (!bot->dead && bot->fd == fd && (evs[i].events & EPOLLIN)) bot->onReadable();
        if (bot->fd != fd) dropFd(fd);
        if (bot->dead) dropFd(bot->fd);
        else arm(*bot);
      }
#endif
    }
#if defined(__APPLE__)
    if (ep >= 0) {
      ::close(ep);
      ep = -1;
    }
    if (wake >= 0) {
      ::close(wake);
      wake = -1;
    }
    if (wakeWrite >= 0) {
      ::close(wakeWrite);
      wakeWrite = -1;
    }
#endif
  }

};

Fleet::Fleet(int threads) : threads_(std::max(1, threads)) {}

Fleet::~Fleet() { stop(); }

void Fleet::start() {
  if (run_.exchange(true)) return;
  for (int i = 0; i < threads_; i++) {
    auto w = std::make_unique<Worker>();
    w->run = &run_;
#if defined(__APPLE__)
    w->ep = ::kqueue();
    if (w->ep >= 0) {
      int flags = ::fcntl(w->ep, F_GETFD, 0);
      if (flags >= 0) ::fcntl(w->ep, F_SETFD, flags | FD_CLOEXEC);
    }
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
      for (int fd : fds) {
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int fdflags = ::fcntl(fd, F_GETFD, 0);
        if (fdflags >= 0) ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
      }
      w->wake = fds[0];
      w->wakeWrite = fds[1];
      struct kevent ev;
      EV_SET(&ev, w->wake, EVFILT_READ, EV_ADD, 0, 0, nullptr);
      (void)::kevent(w->ep, &ev, 1, nullptr, 0, nullptr);
    }
#else
    w->ep = epoll_create1(EPOLL_CLOEXEC);
    w->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event ev{};
    ev.data.fd = w->wake;
    ev.events = EPOLLIN;
    epoll_ctl(w->ep, EPOLL_CTL_ADD, w->wake, &ev);
#endif
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
