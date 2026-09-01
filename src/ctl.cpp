#include "Stats.hpp"

#include <curl/curl.h>
#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct HttpResult {
    int status = 0;
    std::string body;
    std::string error;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResult http(const std::string& method, const std::string& url,
                const std::string& token, const std::string& body) {
    HttpResult out;
    CURL* c = curl_easy_init();
    if (!c) {
        out.error = "curl_easy_init failed";
        return out;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!token.empty()) {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    } else {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        out.status = static_cast<int>(code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return out;
}

std::string joinUrl(std::string base, const char* path) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

std::vector<std::string> loadHosts(const std::string& path,
                                   const std::vector<std::string>& extra) {
    std::vector<std::string> hosts = extra;
    if (!path.empty()) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "error: cannot open hosts file '%s'\n", path.c_str());
            std::exit(2);
        }
        std::string line;
        while (std::getline(f, line)) {
            auto b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            auto e = line.find_last_not_of(" \t\r\n");
            std::string s = line.substr(b, e - b + 1);
            if (s.empty() || s[0] == '#') continue;
            hosts.push_back(s);
        }
    }
    return hosts;
}

struct FanIn {
    std::string host;
    HttpResult res;
};

std::vector<FanIn> fan(const std::vector<std::string>& hosts, const std::string& token,
                       const std::string& method, const char* path,
                       const std::string& body) {
    std::vector<FanIn> out(hosts.size());
    std::vector<std::thread> ts;
    for (size_t i = 0; i < hosts.size(); ++i) {
        ts.emplace_back([&, i] {
            out[i].host = hosts[i];
            out[i].res = http(method, joinUrl(hosts[i], path), token, body);
        });
    }
    for (auto& t : ts) t.join();
    return out;
}

int failIfAny(const std::vector<FanIn>& rows, int minOk, int maxOk) {
    int rc = 0;
    for (const auto& r : rows) {
        if (!r.res.error.empty()) {
            std::fprintf(stderr, "%s: %s\n", r.host.c_str(), r.res.error.c_str());
            rc = 1;
            continue;
        }
        if (r.res.status < minOk || r.res.status > maxOk) {
            std::fprintf(stderr, "%s: HTTP %d %s\n", r.host.c_str(), r.res.status,
                         r.res.body.c_str());
            rc = 1;
        }
    }
    return rc;
}

void printBodies(const std::vector<FanIn>& rows) {
    for (const auto& r : rows) {
        std::printf("=== %s ===\n%s\n", r.host.c_str(), r.res.body.c_str());
    }
}

std::string envToken(const std::string& cli) {
    if (!cli.empty()) return cli;
    if (const char* t = std::getenv("LT_CONTROL_TOKEN")) return t;
    return "";
}

} // namespace

int main(int argc, char** argv) {
    cxxopts::Options opts(
        "cks-loadtest-ctl",
        "Drive a fleet of cks-loadtest generators over their HTTP control ports.\n"
        "Commands: status, stats, add, wait, rung-open, rung-close, shutdown, aggregate");
    opts.add_options()
        ("hosts", "File of base URLs, one http://host:9109 per line",
         cxxopts::value<std::string>())
        ("host", "Generator base URL (repeatable)",
         cxxopts::value<std::vector<std::string>>())
        ("token", "Bearer token (else LT_CONTROL_TOKEN)", cxxopts::value<std::string>())
        ("count", "clients to add per host", cxxopts::value<int>()->default_value("0"))
        ("id", "rung id", cxxopts::value<std::string>())
        ("out", "write merged JSON here", cxxopts::value<std::string>())
        ("in", "JSON file to merge (aggregate; repeatable)",
         cxxopts::value<std::vector<std::string>>())
        ("timeout-sec", "wait timeout", cxxopts::value<int>()->default_value("120"))
        ("active-delta", "wait until used AND live clients grew by this many per host",
         cxxopts::value<int>()->default_value("0"))
        ("stable-sec", "sleep this many seconds after idle (measurement window)",
         cxxopts::value<int>()->default_value("0"))
        ("command", "status|stats|add|wait|rung-open|rung-close|shutdown|aggregate",
         cxxopts::value<std::string>())
        ("h,help", "Show help");
    opts.parse_positional({"command"});
    opts.positional_help("command");

    cxxopts::ParseResult cli;
    try {
        cli = opts.parse(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n%s\n", e.what(), opts.help().c_str());
        return 2;
    }
    if (cli.count("help") || !cli.count("command")) {
        std::printf("%s\n", opts.help().c_str());
        return cli.count("help") ? 0 : 2;
    }

    const std::string cmd = cli["command"].as<std::string>();
    const std::string token = envToken(cli.count("token") ? cli["token"].as<std::string>() : "");
    std::vector<std::string> extra;
    if (cli.count("host")) extra = cli["host"].as<std::vector<std::string>>();
    std::string hostsFile = cli.count("hosts") ? cli["hosts"].as<std::string>() : "";
    auto hosts = loadHosts(hostsFile, extra);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto needHosts = [&]() {
        if (hosts.empty()) {
            std::fprintf(stderr, "error: pass --hosts FILE and/or --host URL\n");
            std::exit(2);
        }
    };

    int rc = 0;
    if (cmd == "status") {
        needHosts();
        auto rows = fan(hosts, token, "GET", "/v1/status", "");
        rc = failIfAny(rows, 200, 200);
        printBodies(rows);
    } else if (cmd == "stats") {
        needHosts();
        auto rows = fan(hosts, token, "GET", "/v1/stats", "");
        rc = failIfAny(rows, 200, 200);
        std::vector<nlohmann::json> snaps;
        for (const auto& r : rows) {
            if (r.res.status == 200) {
                try {
                    snaps.push_back(nlohmann::json::parse(r.res.body));
                } catch (...) {
                }
            }
        }
        auto fleet = lt::mergeFleetStats(snaps);
        if (cli.count("out")) {
            std::ofstream f(cli["out"].as<std::string>());
            f << fleet.dump(2) << '\n';
        } else {
            std::printf("%s\n", fleet.dump(2).c_str());
        }
    } else if (cmd == "add") {
        needHosts();
        int count = cli["count"].as<int>();
        if (count < 1) {
            std::fprintf(stderr, "error: --count N is required\n");
            curl_global_cleanup();
            return 2;
        }
        nlohmann::json body = {{"count", count}};
        auto rows = fan(hosts, token, "POST", "/v1/clients/add", body.dump());
        rc = failIfAny(rows, 202, 202);
        printBodies(rows);
    } else if (cmd == "wait") {
        needHosts();
        int timeout = cli["timeout-sec"].as<int>();
        int delta = cli["active-delta"].as<int>();
        int stable = cli["stable-sec"].as<int>();
        std::vector<int> baseUsed(hosts.size(), 0);
        std::vector<int> baseLive(hosts.size(), 0);
        if (delta > 0) {
            auto rows = fan(hosts, token, "GET", "/v1/status", "");
            if (failIfAny(rows, 200, 200)) {
                curl_global_cleanup();
                return 1;
            }
            for (size_t i = 0; i < rows.size(); ++i) {
                try {
                    auto st = nlohmann::json::parse(rows[i].res.body);
                    baseUsed[i] = st.value("used", 0);
                    baseLive[i] = st.value("active", 0) + st.value("suspended", 0);
                } catch (...) {
                }
            }
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        for (;;) {
            auto rows = fan(hosts, token, "GET", "/v1/status", "");
            if (failIfAny(rows, 200, 200)) {
                rc = 1;
                break;
            }
            bool idle = true;
            bool grown = true;
            for (size_t i = 0; i < rows.size(); ++i) {
                auto st = nlohmann::json::parse(rows[i].res.body);
                if (st.value("busy", false)) idle = false;
                int live = st.value("active", 0) + st.value("suspended", 0);
                if (delta > 0 && (st.value("used", 0) < baseUsed[i] + delta ||
                                  live < baseLive[i] + delta))
                    grown = false;
                std::string err = st.value("add_error", std::string());
                if (!err.empty()) {
                    std::fprintf(stderr, "%s: add_error: %s\n", rows[i].host.c_str(),
                                 err.c_str());
                    rc = 1;
                }
            }
            if (rc) break;
            if (idle && grown) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "error: wait timed out after %ds\n", timeout);
                rc = 3;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (rc == 0 && stable > 0) {
            std::printf("[wait] idle; sleeping %ds for a stable measurement window\n",
                        stable);
            std::this_thread::sleep_for(std::chrono::seconds(stable));
        }
    } else if (cmd == "rung-open") {
        needHosts();
        std::string id = cli.count("id") ? cli["id"].as<std::string>() : "";
        if (id.empty()) {
            std::fprintf(stderr, "error: --id is required\n");
            curl_global_cleanup();
            return 2;
        }
        nlohmann::json body = {{"id", id}};
        auto rows = fan(hosts, token, "POST", "/v1/rung/open", body.dump());
        rc = failIfAny(rows, 200, 200);
        printBodies(rows);
    } else if (cmd == "rung-close") {
        needHosts();
        auto rows = fan(hosts, token, "POST", "/v1/rung/close", "{}");
        rc = failIfAny(rows, 200, 200);
        std::vector<nlohmann::json> snaps;
        for (const auto& r : rows) {
            if (r.res.status != 200) continue;
            try {
                auto j = nlohmann::json::parse(r.res.body);
                j["host"] = r.host;
                snaps.push_back(std::move(j));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s: bad JSON: %s\n", r.host.c_str(), e.what());
                rc = 1;
            }
        }
        auto fleet = lt::mergeFleetStats(snaps);
        fleet["per_host"] = snaps;
        std::string dumped = fleet.dump(2);
        if (cli.count("out")) {
            std::ofstream f(cli["out"].as<std::string>());
            f << dumped << '\n';
            std::printf("wrote %s\n", cli["out"].as<std::string>().c_str());
        } else {
            std::printf("%s\n", dumped.c_str());
        }
        if (fleet.contains("warning")) {
            std::fprintf(stderr, "warning: %s\n",
                         fleet["warning"].get<std::string>().c_str());
            if (rc == 0) rc = 1;
        }
    } else if (cmd == "shutdown") {
        needHosts();
        auto rows = fan(hosts, token, "POST", "/v1/shutdown", "{}");
        rc = failIfAny(rows, 200, 200);
    } else if (cmd == "aggregate") {
        if (!cli.count("in")) {
            std::fprintf(stderr, "error: aggregate needs one or more --in JSON files\n");
            curl_global_cleanup();
            return 2;
        }
        std::vector<nlohmann::json> snaps;
        for (const auto& path : cli["in"].as<std::vector<std::string>>()) {
            std::ifstream f(path);
            if (!f) {
                std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
                curl_global_cleanup();
                return 1;
            }
            nlohmann::json j;
            f >> j;
            snaps.push_back(std::move(j));
        }
        auto fleet = lt::mergeFleetStats(snaps);
        std::string dumped = fleet.dump(2);
        if (cli.count("out")) {
            std::ofstream f(cli["out"].as<std::string>());
            f << dumped << '\n';
        } else {
            std::printf("%s\n", dumped.c_str());
        }
    } else {
        std::fprintf(stderr, "error: unknown command '%s'\n%s\n", cmd.c_str(),
                     opts.help().c_str());
        rc = 2;
    }

    curl_global_cleanup();
    return rc;
}
