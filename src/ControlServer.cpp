#include "ControlServer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace lt {
namespace {

bool bearerOk(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true; // loopback-only binds may omit a token
    auto auth = req.get_header_value("Authorization");
    const std::string want = "Bearer " + token;
    return auth == want;
}

void unauthorized(httplib::Response& res) {
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
}

void jsonRes(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

} // namespace

ControlServer::ControlServer(Harness& harness, ControlBind bind, std::string token)
    : harness_(harness), bind_(std::move(bind)), token_(std::move(token)) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
    if (bind_.disabled) return true;
    auto* svr = new httplib::Server();
    server_ = svr;

    svr->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"ok":true})", "application/json");
    });

    auto auth = [this](const httplib::Request& req, httplib::Response& res) {
        if (!bearerOk(req, token_)) {
            unauthorized(res);
            return false;
        }
        return true;
    };

    svr->Get("/v1/status", [this, auth](const httplib::Request& req, httplib::Response& res) {
        if (!auth(req, res)) return;
        jsonRes(res, 200, harness_.statusJson());
    });

    svr->Get("/v1/stats", [this, auth](const httplib::Request& req, httplib::Response& res) {
        if (!auth(req, res)) return;
        jsonRes(res, 200, harness_.statsJson());
    });

    svr->Post("/v1/clients/add",
              [this, auth](const httplib::Request& req, httplib::Response& res) {
                  if (!auth(req, res)) return;
                  int count = 0;
                  try {
                      auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                      count = body.value("count", 0);
                  } catch (const std::exception& e) {
                      jsonRes(res, 400, {{"error", e.what()}});
                      return;
                  }
                  std::string err = harness_.requestAdd(count);
                  if (!err.empty()) {
                      jsonRes(res, 409, {{"error", err}});
                      return;
                  }
                  jsonRes(res, 202,
                          {{"accepted", true},
                           {"count", count},
                           {"used", harness_.used()},
                           {"limit", harness_.config().indexLimit}});
              });

    svr->Post("/v1/rung/open",
              [this, auth](const httplib::Request& req, httplib::Response& res) {
                  if (!auth(req, res)) return;
                  std::string id;
                  try {
                      auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                      id = body.value("id", std::string());
                  } catch (const std::exception& e) {
                      jsonRes(res, 400, {{"error", e.what()}});
                      return;
                  }
                  std::string err = harness_.openRung(id);
                  if (!err.empty()) {
                      jsonRes(res, 409, {{"error", err}});
                      return;
                  }
                  jsonRes(res, 200, {{"ok", true}, {"id", id}});
              });

    svr->Post("/v1/rung/close",
              [this, auth](const httplib::Request& req, httplib::Response& res) {
                  if (!auth(req, res)) return;
                  jsonRes(res, 200, harness_.closeRung());
              });

    svr->Post("/v1/shutdown",
              [this, auth](const httplib::Request& req, httplib::Response& res) {
                  if (!auth(req, res)) return;
                  harness_.requestShutdown();
                  jsonRes(res, 200, {{"ok", true}});
              });

    running_.store(true);
    thread_ = std::thread([this] { serve(); });
    // Give listen a moment to fail-fast on EADDRINUSE by polling is_running.
    for (int i = 0; i < 50; ++i) {
        if (svr->is_running()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // listen() may still be binding; treat as started. Failures log in serve().
    return true;
}

void ControlServer::serve() {
    auto* svr = static_cast<httplib::Server*>(server_);
    std::printf("[control] HTTP listening on %s:%d%s\n", bind_.host.c_str(),
                bind_.port, token_.empty() ? " (no token; loopback)" : " (bearer)");
    if (!svr->listen(bind_.host, bind_.port)) {
        std::fprintf(stderr, "[control] listen failed on %s:%d\n", bind_.host.c_str(),
                     bind_.port);
        running_.store(false);
    }
}

void ControlServer::stop() {
    if (auto* svr = static_cast<httplib::Server*>(server_)) {
        svr->stop();
    }
    if (thread_.joinable()) thread_.join();
    if (auto* svr = static_cast<httplib::Server*>(server_)) {
        delete svr;
        server_ = nullptr;
    }
    running_.store(false);
}

} // namespace lt
