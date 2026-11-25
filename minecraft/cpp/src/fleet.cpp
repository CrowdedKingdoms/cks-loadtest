#include "fleet.hpp"

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ckmc {
namespace {

// One message from a control thread to a worker.
struct Cmd {
  enum class Kind { Add, StopOne, StopAll, Bounds, Speed, Altitude, GridSet, Rally, Unrally };
  Kind kind = Kind::StopAll;
  BotSpec spec;
  std::shared_ptr<BotView> view;
  std::string name;
  WanderBox box;
  Grid grid;
  bool on = false;
  // Speed: a is blocks per second. Altitude: a is Y. Rally: a, b are x, z and c is the radius.
  double a = 0;
  double b = 0;
  double c = 0;
  // Rally: the bots picked, by seq, ascending.
  std::shared_ptr<const std::vector<uint64_t>> seqs;
};

Cmd cmdOf(Cmd::Kind kind) {
  Cmd cmd;
  cmd.kind = kind;
  return cmd;
}

}  // namespace

struct Fleet::Worker {
  int ep = -1;
  int wake = -1;
#if defined(__APPLE__)
  int wakeWrite = -1;
#endif
  // Guards the inbox only. The worker holds it for one swap, never during I/O or logging.
  std::mutex mu;
  std::vector<Cmd> inbox;
  std::vector<std::unique_ptr<Bot>> bots;
  // fd -> the bot registered on it. Indexed by fd number.
  std::vector<Bot*> byFd;
  // What `bounds`, `speed`, `altitude` and `grid` set. Only this worker's thread touches it.
  Shared shared;
  std::atomic<bool>* run = nullptr;
  // Longest tick pass since `stats` last read it, in microseconds.
  std::atomic<int64_t> tickMaxUs{0};

  ~Worker() {
    if (ep >= 0) ::close(ep);
    if (wake >= 0) ::close(wake);
#if defined(__APPLE__)
    if (wakeWrite >= 0) ::close(wakeWrite);
#endif
  }

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
    if (wake >= 0) {
      ssize_t n = ::write(wake, &one, sizeof(one));
      (void)n;
    }
#endif
  }

  void arm(Bot& bot) {
    if (bot.fd < 0) return;
    bool wantWrite = bot.wantsWrite();
    // Nothing to change: no system call. closeSocket() clears armedFd, so a new socket
    // that got the old fd number is registered again.
    if (bot.armedFd == bot.fd && bot.armedWrite == wantWrite) return;
#if defined(__APPLE__)
    struct kevent ch;
    EV_SET(&ch, bot.fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (::kevent(ep, &ch, 1, nullptr, 0, nullptr) < 0) return;
    if (wantWrite) {
      EV_SET(&ch, bot.fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
      if (::kevent(ep, &ch, 1, nullptr, 0, nullptr) < 0) return;
    } else {
      EV_SET(&ch, bot.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
      (void)::kevent(ep, &ch, 1, nullptr, 0, nullptr);
    }
#else
    epoll_event ev{};
    ev.data.fd = bot.fd;
    ev.events = EPOLLIN;
    if (wantWrite) ev.events |= EPOLLOUT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, bot.fd, &ev) < 0) {
      if (epoll_ctl(ep, EPOLL_CTL_ADD, bot.fd, &ev) < 0) return;
    }
#endif
    bot.armedFd = bot.fd;
    bot.armedWrite = wantWrite;
    auto slot = static_cast<size_t>(bot.fd);
    if (slot >= byFd.size()) byFd.resize(slot + 64, nullptr);
    byFd[slot] = &bot;
  }

  // `owner` no longer has a socket on `fd`. There is nothing to take out of the wait
  // set: a bot's socket is never duplicated, so closing it removed it there, on epoll and
  // on kqueue alike. And by now the number may belong to a new socket, of another worker
  // even, which a delete by number would wrongly name. Only the table is cleared.
  void dropFd(int fd, const Bot* owner) {
    if (fd < 0) return;
    auto slot = static_cast<size_t>(fd);
    if (slot < byFd.size() && byFd[slot] == owner) byFd[slot] = nullptr;
  }

  Bot* findFd(int fd) {
    if (fd < 0) return nullptr;
    auto slot = static_cast<size_t>(fd);
    if (slot >= byFd.size()) return nullptr;
    Bot* bot = byFd[slot];
    if (!bot || bot->fd != fd) return nullptr;
    return bot;
  }

  // After the handlers ran for a bot whose socket was `fd` (-1: it had none).
  void rearm(Bot& bot, int fd) {
    if (bot.fd != fd || bot.dead) dropFd(fd, &bot);
    if (!bot.dead) arm(bot);
  }

  // Free the bots that ended. Their names and reasons stay in the fleet's registry.
  void reap() {
    bool any = false;
    for (auto& b : bots) {
      if (!b || b->dead) {
        any = true;
        break;
      }
    }
    if (!any) return;
    for (Bot*& slot : byFd) {
      if (slot && slot->dead) slot = nullptr;
    }
    bots.erase(std::remove_if(bots.begin(), bots.end(),
                              [](const std::unique_ptr<Bot>& b) { return !b || b->dead; }),
               bots.end());
  }

  void noteTick(int64_t us) {
    int64_t prev = tickMaxUs.load(std::memory_order_relaxed);
    while (us > prev && !tickMaxUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
  }

  void drain() {
    std::vector<Cmd> batch;
    {
      std::lock_guard<std::mutex> lock(mu);
      batch.swap(inbox);
    }
    for (auto& cmd : batch) {
      switch (cmd.kind) {
        case Cmd::Kind::StopAll:
          for (auto& b : bots) {
            if (b && !b->dead) {
              int had = b->fd;
              b->stop("stopped");
              dropFd(had, b.get());
            }
          }
          break;
        case Cmd::Kind::StopOne:
          for (auto& b : bots) {
            if (b && b->name() == cmd.name && !b->dead) {
              int had = b->fd;
              b->stop("stopped");
              dropFd(had, b.get());
            }
          }
          break;
        case Cmd::Kind::Add: {
          auto bot = std::make_unique<Bot>(std::move(cmd.spec), &shared, std::move(cmd.view));
          if (bot->start()) arm(*bot);
          bots.push_back(std::move(bot));
          break;
        }
        case Cmd::Kind::Bounds:
          shared.box = cmd.box;
          break;
        case Cmd::Kind::Speed:
          shared.speedOn = cmd.on;
          shared.bps = cmd.a;
          break;
        case Cmd::Kind::Altitude:
          shared.altitudeOn = cmd.on;
          shared.altitude = cmd.a;
          break;
        case Cmd::Kind::GridSet:
          shared.grid = cmd.grid;
          shared.gridGen++;
          break;
        case Cmd::Kind::Rally:
          if (!cmd.seqs) break;
          for (auto& b : bots) {
            if (!b || b->dead) continue;
            if (std::binary_search(cmd.seqs->begin(), cmd.seqs->end(), b->seq())) {
              b->setRally(cmd.a, cmd.b, cmd.c);
            }
          }
          break;
        case Cmd::Kind::Unrally:
          for (auto& b : bots) {
            if (b && !b->dead) b->clearRally();
          }
          break;
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
          rearm(*b, before);
        }
        reap();
        auto spent = std::chrono::steady_clock::now() - now;
        noteTick(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(spent).count()));
        next += std::chrono::milliseconds(50);
        if (next < now) next = now + std::chrono::milliseconds(50);
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
          bot->onSocketError();
        } else {
          if (ready[i].out) bot->onWritable();
          if (!bot->dead && bot->fd == fd && ready[i].in) bot->onReadable();
        }
        rearm(*bot, fd);
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
          bot->onSocketError();
        } else {
          if (evs[i].events & EPOLLOUT) bot->onWritable();
          if (!bot->dead && bot->fd == fd && (evs[i].events & EPOLLIN)) bot->onReadable();
        }
        rearm(*bot, fd);
      }
#endif
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
  for (auto& w : workers_) w->push(cmdOf(Cmd::Kind::StopAll));
  for (auto& t : threadsPool_) {
    if (t.joinable()) t.join();
  }
  threadsPool_.clear();
  // The workers' wait handles close here, after the threads that used them are gone.
  workers_.clear();
}

void Fleet::pruneLocked() {
  if (views_.size() < pruneAt_) return;
  size_t closed = 0;
  for (auto& v : views_) {
    if (v->phase.load(std::memory_order_acquire) == static_cast<uint8_t>(BotPhase::Closed)) closed++;
  }
  if (closed > kKeepClosed) {
    size_t drop = closed - kKeepClosed;
    std::vector<std::shared_ptr<BotView>> kept;
    kept.reserve(views_.size() - drop);
    for (auto& v : views_) {
      // A view seen closed stays closed, so this drops the oldest `drop` of them.
      if (drop > 0 && v->phase.load(std::memory_order_acquire) == static_cast<uint8_t>(BotPhase::Closed)) {
        drop--;
        closedDropped_++;
        continue;
      }
      kept.push_back(std::move(v));
    }
    views_.swap(kept);
  }
  pruneAt_ = views_.size() + kKeepClosed;
}

void Fleet::add(BotSpec spec) {
  if (workers_.empty()) throw std::runtime_error("fleet is not running");
  auto view = std::make_shared<BotView>();
  view->name = spec.name;
  {
    std::lock_guard<std::mutex> lock(viewsMu_);
    pruneLocked();
    view->seq = nextSeq_++;
    views_.push_back(view);
  }
  Cmd cmd = cmdOf(Cmd::Kind::Add);
  cmd.spec = std::move(spec);
  cmd.view = std::move(view);
  auto i = static_cast<size_t>(next_.fetch_add(1)) % workers_.size();
  workers_[i]->push(std::move(cmd));
}

void Fleet::stopBot(const std::string& name) {
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::StopOne);
    cmd.name = name;
    w->push(std::move(cmd));
  }
}

void Fleet::stopAll() {
  for (auto& w : workers_) w->push(cmdOf(Cmd::Kind::StopAll));
}

void Fleet::setBounds(const WanderBox& box) {
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::Bounds);
    cmd.box = box;
    w->push(std::move(cmd));
  }
}

void Fleet::setSpeed(bool on, double bps) {
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::Speed);
    cmd.on = on;
    cmd.a = bps;
    w->push(std::move(cmd));
  }
}

void Fleet::setAltitude(bool on, double y) {
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::Altitude);
    cmd.on = on;
    cmd.a = y;
    w->push(std::move(cmd));
  }
}

void Fleet::setGrid(const Grid& grid) {
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::GridSet);
    cmd.grid = grid;
    w->push(std::move(cmd));
  }
}

size_t Fleet::rally(double x, double z, double radius, long count) {
  auto picked = std::make_shared<std::vector<uint64_t>>();
  {
    std::lock_guard<std::mutex> lock(viewsMu_);
    // views_ is in add order, so seq ascends and the worker can binary-search the list.
    for (auto& v : views_) {
      if (count >= 0 && picked->size() >= static_cast<size_t>(count)) break;
      if (v->phase.load(std::memory_order_acquire) == static_cast<uint8_t>(BotPhase::Play)) {
        picked->push_back(v->seq);
      }
    }
  }
  size_t n = picked->size();
  std::shared_ptr<const std::vector<uint64_t>> seqs = std::move(picked);
  for (auto& w : workers_) {
    Cmd cmd = cmdOf(Cmd::Kind::Rally);
    cmd.a = x;
    cmd.b = z;
    cmd.c = radius;
    cmd.seqs = seqs;
    w->push(std::move(cmd));
  }
  return n;
}

void Fleet::unrally() {
  for (auto& w : workers_) w->push(cmdOf(Cmd::Kind::Unrally));
}

FleetStats Fleet::stats() {
  FleetStats s;
  {
    std::lock_guard<std::mutex> lock(viewsMu_);
    for (auto& v : views_) {
      switch (static_cast<BotPhase>(v->phase.load(std::memory_order_acquire))) {
        case BotPhase::Connecting:
          s.connecting++;
          break;
        case BotPhase::Login:
          s.login++;
          break;
        case BotPhase::Config:
          s.config++;
          break;
        case BotPhase::Play:
          s.play++;
          if (v->placed.load(std::memory_order_relaxed)) s.placed++;
          break;
        case BotPhase::Closed:
          s.closed++;
          break;
      }
    }
    s.closed += closedDropped_;
    s.total = views_.size() + closedDropped_;
  }
  Counters& c = counters();
  s.transfers = c.transfers.load(std::memory_order_relaxed);
  s.rejoins = c.rejoins.load(std::memory_order_relaxed);
  s.deaths = c.deaths.load(std::memory_order_relaxed);
  s.respawns = c.respawns.load(std::memory_order_relaxed);
  s.crossings = c.crossings.load(std::memory_order_relaxed);
  int64_t us = 0;
  for (auto& w : workers_) us = std::max(us, w->tickMaxUs.exchange(0, std::memory_order_relaxed));
  s.tickMsMax = static_cast<uint64_t>((us + 500) / 1000);
  return s;
}

std::string Fleet::list(const std::string& phase, size_t cap) {
  bool wantPlaced = phase == "placed";
  BotPhase want = BotPhase::Play;
  if (phase == "connecting") want = BotPhase::Connecting;
  else if (phase == "login") want = BotPhase::Login;
  else if (phase == "config") want = BotPhase::Config;
  else if (phase == "play" || wantPlaced) want = BotPhase::Play;
  else if (phase == "closed") want = BotPhase::Closed;
  else throw std::runtime_error("unknown phase " + phase);

  // Room for the "ok " in front and the ",..." and newline behind.
  const std::string more = "...";
  std::string out = "ok";
  bool first = true;
  bool truncated = false;
  std::lock_guard<std::mutex> lock(viewsMu_);
  for (auto& v : views_) {
    if (static_cast<BotPhase>(v->phase.load(std::memory_order_acquire)) != want) continue;
    if (wantPlaced && !v->placed.load(std::memory_order_relaxed)) continue;
    std::string item = v->name;
    if (want == BotPhase::Closed) {
      // Safe to read: the worker wrote the reason before it published Closed.
      std::string reason = v->reason;
      for (char& ch : reason) {
        if (ch == ',') ch = ';';
        else if (ch == '\n' || ch == '\r') ch = ' ';
      }
      item += ':';
      item += reason;
    }
    // One byte for the space or comma before the item, and keep room for ",...\n".
    if (out.size() + 1 + item.size() + 1 + more.size() + 1 > cap) {
      truncated = true;
      break;
    }
    out += first ? ' ' : ',';
    out += item;
    first = false;
  }
  if (truncated) {
    out += first ? ' ' : ',';
    out += more;
  }
  return out;
}

int Fleet::started() const {
  std::lock_guard<std::mutex> lock(viewsMu_);
  return static_cast<int>(views_.size() + closedDropped_);
}

int Fleet::finished() const {
  std::lock_guard<std::mutex> lock(viewsMu_);
  size_t n = closedDropped_;
  for (auto& v : views_) {
    if (v->phase.load(std::memory_order_acquire) == static_cast<uint8_t>(BotPhase::Closed)) n++;
  }
  return static_cast<int>(n);
}

}  // namespace ckmc
