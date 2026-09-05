#include "Config.hpp"
#include "Stats.hpp"
#include "Worker.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

} // namespace

int main() {
    lt::Config cfg;
    cfg.threads = 1;
    cfg.updateHz = 10;
    cfg.sessionSettleMs = 0;
    lt::Stats stats;
    lt::ControlQueue q;
    lt::Worker w(0, cfg, stats, q);
    w.start();

    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        lt::ClientCredentials c;
        c.index = 1000 + i;
        c.email = "bot@example.invalid";
        c.serverIp4 = "127.0.0.1";
        c.serverPort = 1;
        c.udpReadyAt = now;
        w.addClient(std::move(c), now);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (w.clientCount() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(w.clientCount() == 5, "hot-add five clients after start()");

    // A second batch must keep the first five (indices stay stable).
    for (int i = 0; i < 3; ++i) {
        lt::ClientCredentials c;
        c.index = 2000 + i;
        c.email = "bot@example.invalid";
        c.serverIp4 = "127.0.0.1";
        c.serverPort = 1;
        c.udpReadyAt = std::chrono::steady_clock::now();
        w.addClient(std::move(c), std::chrono::steady_clock::now());
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (w.clientCount() < 8 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(w.clientCount() == 8, "second hot-add appends without dropping the first");

    w.stop();
    q.shutdown();

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all ok\n");
    return 0;
}
