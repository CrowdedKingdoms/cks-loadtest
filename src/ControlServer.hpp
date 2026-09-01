#pragma once

#include "Harness.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace lt {

/// HTTP/1.1 JSON control port. Bind loopback by default; a non-loopback bind
/// requires a bearer token (enforced in Config::validate).
class ControlServer {
public:
    ControlServer(Harness& harness, ControlBind bind, std::string token);
    ~ControlServer();

    bool start();
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    void serve();

    Harness& harness_;
    ControlBind bind_;
    std::string token_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    // httplib::Server* stored opaquely so the header stays small.
    void* server_ = nullptr;
};

} // namespace lt
