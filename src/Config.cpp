#include "Config.hpp"

#include <cxxopts.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace lt {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// KEY=VALUE file, '#' comments, blank lines ignored. Values may be quoted.
std::unordered_map<std::string, std::string> parseKvFile(const std::string& path) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "error: cannot open config file '%s'\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        kv[key] = val;
    }
    return kv;
}

/// Layered lookup: env var wins over config-file entry.
struct Layers {
    const std::unordered_map<std::string, std::string>& file;

    std::string get(const char* name, const std::string& fallback) const {
        if (const char* v = std::getenv(name)) return v;
        auto it = file.find(name);
        if (it != file.end()) return it->second;
        return fallback;
    }
    int getInt(const char* name, int fallback) const {
        std::string v = get(name, "");
        return v.empty() ? fallback : std::stoi(v);
    }
    int64_t getI64(const char* name, int64_t fallback) const {
        std::string v = get(name, "");
        return v.empty() ? fallback : std::stoll(v);
    }
    double getDouble(const char* name, double fallback) const {
        std::string v = get(name, "");
        return v.empty() ? fallback : std::stod(v);
    }
    bool getBool(const char* name, bool fallback) const {
        std::string v = get(name, "");
        if (v.empty()) return fallback;
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
};

} // namespace

std::string Config::derivedEmail(int index) const {
    size_t at = email.find('@');
    std::string local = at == std::string::npos ? email : email.substr(0, at);
    std::string domain = at == std::string::npos ? "" : email.substr(at + 1);

    char idx[16];
    std::snprintf(idx, sizeof(idx), "%04d", index);

    std::string out = emailPattern;
    auto replaceAll = [&out](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("{local}", local);
    replaceAll("{domain}", domain);
    replaceAll("{index}", idx);
    return out;
}

std::string Config::validate() const {
    if (email.empty()) return "missing LT_EMAIL / --email";
    if (email.find('@') == std::string::npos) return "LT_EMAIL must be a full email address";
    if (needsPassword()) {
        if (password.empty()) {
            return rosterFile.empty()
                       ? "missing LT_PASSWORD / --password"
                       : "missing LT_PASSWORD / --password: the roster may not "
                         "cover every client and the rest must sign in. Set "
                         "LT_ROSTER_REQUIRED=1 to refuse a short roster instead.";
        }
        if (password.size() < 8) return "LT_PASSWORD must be at least 8 characters (API minimum)";
    }
    if (managementApiUrl.empty()) return "missing LT_MANAGEMENT_API_URL / --management-api-url";
    if (appId <= 0) return "missing/invalid LT_APP_ID / --app-id";
    if (clients < 1) return "LT_CLIENTS must be >= 1";
    if (threads < 1) return "LT_THREADS must be >= 1";
    if (updateHz < 1 || updateHz > 1000) return "LT_UPDATE_HZ must be in [1, 1000]";
    if (distance < 0 || distance > 255) return "LT_DISTANCE must be in [0, 255]";
    if (permWindowRadiusChunks < 0)
        return "LT_PERMISSION_WINDOW_RADIUS_CHUNKS must be >= 0";
    if (permReloadGraceMs < 0) return "LT_PERMISSION_RELOAD_GRACE_MS must be >= 0";
    if (decay < 0 || decay > 5) return "LT_DECAY must be in [0, 5]";
    if (provisionConcurrency < 1) return "LT_PROVISION_CONCURRENCY must be >= 1";
    if (rampBatchSize < 1) return "LT_RAMP_BATCH_SIZE must be >= 1";
    return "";
}

Config Config::load(int argc, char** argv) {
    cxxopts::Options opts(
        "cks-loadtest",
        "UDP load tester for Crowded Kingdoms game servers.\n"
        "All options may also be set via LT_* environment variables or a\n"
        "--config KEY=VALUE file (CLI > env > file).");

    // clang-format off
    opts.add_options()
        ("config", "Config file with KEY=VALUE lines (LT_* names)", cxxopts::value<std::string>())
        ("email", "Base account email (per-client emails are derived from it)", cxxopts::value<std::string>())
        ("password",
         "Password for the base + derived accounts. Prefer LT_PASSWORD: argv is "
         "visible in ps(1).",
         cxxopts::value<std::string>())
        ("management-api-url", "Management API base URL, e.g. https://api.example.com", cxxopts::value<std::string>())
        ("game-api-url", "Game API base URL override (default: from mintAppToken)", cxxopts::value<std::string>())
        ("app-id", "App id to load test", cxxopts::value<int64_t>())
        ("clients", "Number of simulated clients", cxxopts::value<int>())
        ("threads", "Worker threads", cxxopts::value<int>())
        ("update-hz", "Actor updates per second per client", cxxopts::value<int>())
        ("walk-speed", "Walk speed in Unreal units/second", cxxopts::value<double>())
        ("spawn-radius-chunks", "Spawn radius around origin, in chunks", cxxopts::value<int>())
        ("distance", "Replication distance (chunks)", cxxopts::value<int>())
        ("permission-window-radius-chunks",
         "Assumed radius of the server's cached grid-permission box, for "
         "classifying UNAUTHORIZED refusals only",
         cxxopts::value<int>())
        ("decay", "Decay rate 0=none 1=exponential 2..5=linear", cxxopts::value<int>())
        ("ramp-batch-size", "Clients activated per ramp batch", cxxopts::value<int>())
        ("ramp-interval-ms", "Interval between ramp batches (ms)", cxxopts::value<int>())
        ("provision-concurrency", "Parallel GraphQL provisioning requests", cxxopts::value<int>())
        ("duration-sec", "Test duration in seconds (0 = until Ctrl-C)", cxxopts::value<int>())
        ("stats-interval-sec", "Seconds between stats reports", cxxopts::value<int>())
        ("csv-out", "Append per-interval stats to this CSV file", cxxopts::value<std::string>())
        ("email-pattern", "Derived email pattern ({local},{domain},{index})", cxxopts::value<std::string>())
        ("roster", "Pre-minted session roster JSON (a run then signs in nobody)", cxxopts::value<std::string>())
        ("roster-required", "Refuse to run if the roster misses any client", cxxopts::value<bool>())
        ("verify-server-hmac", "Verify HMAC on signed server notifications", cxxopts::value<bool>())
        ("tls-insecure", "Skip TLS certificate verification (dev only)", cxxopts::value<bool>())
        ("session-settle-ms", "Wait after server assignment before UDP (ms)", cxxopts::value<int>())
        ("rx-health-timeout-sec", "Fail if nothing received after this many seconds (0 = off)", cxxopts::value<int>())
        ("token-refresh-lead-sec", "Refresh app tokens this many seconds before expiry", cxxopts::value<int>())
        ("h,help", "Show help");
    // clang-format on

    cxxopts::ParseResult cli;
    try {
        cli = opts.parse(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n\n%s\n", e.what(), opts.help().c_str());
        std::exit(2);
    }
    if (cli.count("help")) {
        std::printf("%s\n", opts.help().c_str());
        std::exit(0);
    }

    std::unordered_map<std::string, std::string> fileKv;
    if (cli.count("config")) fileKv = parseKvFile(cli["config"].as<std::string>());
    Layers layers{fileKv};

    Config c;
    c.email = layers.get("LT_EMAIL", c.email);
    c.password = layers.get("LT_PASSWORD", c.password);
    c.managementApiUrl = layers.get("LT_MANAGEMENT_API_URL", c.managementApiUrl);
    c.gameApiUrl = layers.get("LT_GAME_API_URL", c.gameApiUrl);
    c.appId = layers.getI64("LT_APP_ID", c.appId);
    c.clients = layers.getInt("LT_CLIENTS", c.clients);
    c.threads = layers.getInt("LT_THREADS", c.threads);
    c.updateHz = layers.getInt("LT_UPDATE_HZ", c.updateHz);
    c.walkSpeed = layers.getDouble("LT_WALK_SPEED", c.walkSpeed);
    c.spawnRadiusChunks = layers.getInt("LT_SPAWN_RADIUS_CHUNKS", c.spawnRadiusChunks);
    c.distance = layers.getInt("LT_DISTANCE", c.distance);
    c.permWindowRadiusChunks = layers.getInt("LT_PERMISSION_WINDOW_RADIUS_CHUNKS",
                                             c.permWindowRadiusChunks);
    c.permReloadGraceMs =
        layers.getInt("LT_PERMISSION_RELOAD_GRACE_MS", c.permReloadGraceMs);
    c.decay = layers.getInt("LT_DECAY", c.decay);
    c.rampBatchSize = layers.getInt("LT_RAMP_BATCH_SIZE", c.rampBatchSize);
    c.rampIntervalMs = layers.getInt("LT_RAMP_INTERVAL_MS", c.rampIntervalMs);
    c.provisionConcurrency = layers.getInt("LT_PROVISION_CONCURRENCY", c.provisionConcurrency);
    c.durationSec = layers.getInt("LT_DURATION_SEC", c.durationSec);
    c.statsIntervalSec = layers.getInt("LT_STATS_INTERVAL_SEC", c.statsIntervalSec);
    c.csvOut = layers.get("LT_CSV_OUT", c.csvOut);
    c.emailPattern = layers.get("LT_EMAIL_PATTERN", c.emailPattern);
    c.rosterFile = layers.get("LT_ROSTER_FILE", c.rosterFile);
    c.rosterRequired = layers.getBool("LT_ROSTER_REQUIRED", c.rosterRequired);
    c.verifyServerHmac = layers.getBool("LT_VERIFY_SERVER_HMAC", c.verifyServerHmac);
    c.tlsInsecure = layers.getBool("LT_TLS_INSECURE", c.tlsInsecure);
    c.sessionSettleMs = layers.getInt("LT_SESSION_SETTLE_MS", c.sessionSettleMs);
    c.rxHealthTimeoutSec = layers.getInt("LT_RX_HEALTH_TIMEOUT_SEC", c.rxHealthTimeoutSec);
    c.tokenRefreshLeadSec = layers.getInt("LT_TOKEN_REFRESH_LEAD_SEC", c.tokenRefreshLeadSec);

    // CLI flags override everything.
    auto cliStr = [&](const char* name, std::string& dst) {
        if (cli.count(name)) dst = cli[name].as<std::string>();
    };
    auto cliInt = [&](const char* name, int& dst) {
        if (cli.count(name)) dst = cli[name].as<int>();
    };
    auto cliBool = [&](const char* name, bool& dst) {
        if (cli.count(name)) dst = cli[name].as<bool>();
    };
    cliStr("email", c.email);
    cliStr("password", c.password);
    cliStr("management-api-url", c.managementApiUrl);
    cliStr("game-api-url", c.gameApiUrl);
    if (cli.count("app-id")) c.appId = cli["app-id"].as<int64_t>();
    cliInt("clients", c.clients);
    cliInt("threads", c.threads);
    cliInt("update-hz", c.updateHz);
    if (cli.count("walk-speed")) c.walkSpeed = cli["walk-speed"].as<double>();
    cliInt("spawn-radius-chunks", c.spawnRadiusChunks);
    cliInt("distance", c.distance);
    cliInt("permission-window-radius-chunks", c.permWindowRadiusChunks);
    cliInt("decay", c.decay);
    cliInt("ramp-batch-size", c.rampBatchSize);
    cliInt("ramp-interval-ms", c.rampIntervalMs);
    cliInt("provision-concurrency", c.provisionConcurrency);
    cliInt("duration-sec", c.durationSec);
    cliInt("stats-interval-sec", c.statsIntervalSec);
    cliStr("csv-out", c.csvOut);
    cliStr("email-pattern", c.emailPattern);
    cliStr("roster", c.rosterFile);
    cliBool("roster-required", c.rosterRequired);
    cliBool("verify-server-hmac", c.verifyServerHmac);
    cliBool("tls-insecure", c.tlsInsecure);
    cliInt("session-settle-ms", c.sessionSettleMs);
    cliInt("rx-health-timeout-sec", c.rxHealthTimeoutSec);
    cliInt("token-refresh-lead-sec", c.tokenRefreshLeadSec);

    if (std::string err = c.validate(); !err.empty()) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n", err.c_str());
        std::exit(2);
    }
    if (c.threads > c.clients) c.threads = c.clients;
    return c;
}

} // namespace lt
