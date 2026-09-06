#include "Config.hpp"
#include "ControlServer.hpp"
#include "GraphQLClient.hpp"
#include "Harness.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
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

size_t writeCb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

std::string httpGet(const std::string& url, const std::string& token) {
    std::string body;
    CURL* c = curl_easy_init();
    curl_slist* h = nullptr;
    if (!token.empty()) {
        std::string a = "Authorization: Bearer " + token;
        h = curl_slist_append(h, a.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return body;
}

} // namespace

int main() {
    lt::GraphQLClient::globalInit();
    lt::Config cfg;
    cfg.email = "bots@example.invalid";
    cfg.password = "password1";
    cfg.managementApiUrl = "http://127.0.0.1:9"; // never contacted: clients=0
    cfg.appId = 1;
    cfg.clients = 0;
    cfg.indexLimit = 50;
    cfg.indexBase = 1000;
    cfg.instanceId = "http-test";
    cfg.controlBind = "127.0.0.1:19109";
    cfg.controlToken = "test-token";
    cfg.threads = 1;
    cfg.updateHz = 10;

    std::atomic<bool> stop{false};
    lt::Harness harness(cfg);
    check(harness.start(stop), "harness starts with zero clients");

    lt::ControlServer http(harness, cfg.parsedBind(), cfg.controlToken);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto health = httpGet("http://127.0.0.1:19109/health", "");
    check(health.find("\"ok\"") != std::string::npos, "/health needs no token");

    auto denied = httpGet("http://127.0.0.1:19109/v1/status", "");
    check(denied.find("unauthorized") != std::string::npos, "/v1/status without token");

    auto st = httpGet("http://127.0.0.1:19109/v1/status", "test-token");
    check(st.find("\"used\":0") != std::string::npos || st.find("\"used\": 0") != std::string::npos,
          "status used=0");
    check(st.find("http-test") != std::string::npos, "status instance id");
    check(st.find("1000") != std::string::npos, "status index_base");

    auto stats = httpGet("http://127.0.0.1:19109/v1/stats", "test-token");
    check(stats.find("lifetime") != std::string::npos, "stats has lifetime");
    check(stats.find("window") != std::string::npos, "stats has window");

    stop.store(true);
    harness.requestShutdown();
    http.stop();
    harness.stop();
    lt::GraphQLClient::globalCleanup();

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all ok\n");
    return 0;
}
