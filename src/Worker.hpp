#pragma once

#include "Config.hpp"
#include "Provisioner.hpp"
#include "SimClient.hpp"
#include "Stats.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace lt {

/// Worker -> control thread: this client needs GraphQL work done on its
/// behalf (the worker never blocks on HTTP).
struct ControlRequest {
    enum class Kind : uint8_t {
        REASSIGN,  // COMMAND_RECONNECT / session lost: new serverWithLeastClients
        REFRESH,   // token near expiry or rejected: refreshAppToken + reassign
    };
    int workerIndex = 0;
    int clientIndex = 0; // index within the worker's shard
    Kind kind = Kind::REASSIGN;
    ClientCredentials creds; // snapshot to operate on
};

/// Control thread -> worker: updated credentials/assignment to apply.
struct ClientUpdate {
    int clientIndex = 0;
    bool failed = false; // control gave up; suspend the client permanently
    ClientCredentials creds;
};

/// Mutex + condvar work queue shared by all workers and the control thread.
class ControlQueue {
public:
    void push(ControlRequest req) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(req));
        }
        cv_.notify_one();
    }

    /// Blocks until a request arrives or shutdown; returns false on shutdown.
    bool pop(ControlRequest& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    bool isShutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ControlRequest> queue_;
    bool shutdown_ = false;
};

/// One worker thread owning a shard of simulated clients: an epoll loop that
/// drains inbound datagrams and sends actor updates at the configured rate.
class Worker {
public:
    Worker(int index, const Config& config, Stats& stats, ControlQueue& control);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Queue a provisioned client. Safe before or after start(): the worker
    /// thread appends to its vector, so epoll indices stay stable.
    /// `activateAt` implements the ramp schedule for this add.
    void addClient(ClientCredentials creds,
                   std::chrono::steady_clock::time_point activateAt);

    void start();
    void stop(); // signal + join

    /// Thread-safe: deliver refreshed credentials from the control thread.
    void postUpdate(ClientUpdate update);

    int index() const { return index_; }

    /// Clients currently on this worker (including WAITING/SUSPENDED).
    /// Approximate across threads; exact on the worker thread.
    int clientCount() const { return clientCount_.load(std::memory_order_relaxed); }

private:
    void run();
    void applyPendingUpdates();
    void applyPendingClients();
    void activateDueClients(std::chrono::steady_clock::time_point now, double nowSec);
    void drainSocket(int clientIndex);
    void handleDatagram(SimClient& client, const uint8_t* data, size_t len);
    void sendDueUpdates(double nowSec);
    bool openSocket(SimClient& client);
    void closeSocket(SimClient& client);
    void suspendClient(SimClient& client, ControlRequest::Kind kind);
    int slotOf(const SimClient& client) const;

    int index_;
    const Config& config_;
    Stats& stats_;
    ControlQueue& control_;

    std::vector<SimClient> clients_;
    int epollFd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> clientCount_{0};

    std::mutex inboxMutex_;
    std::vector<ClientUpdate> inbox_;

    struct PendingAdd {
        ClientCredentials creds;
        std::chrono::steady_clock::time_point activateAt{};
    };
    std::mutex pendingMutex_;
    std::vector<PendingAdd> pending_;
};

} // namespace lt
