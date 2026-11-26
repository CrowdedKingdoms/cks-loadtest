#include "fleet/FleetConfig.hpp"

#include <cxxopts.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace lt::fleet {

FleetConfig FleetConfig::load(int argc, char** argv) {
    FleetConfig c;
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0) c.threads = static_cast<int>(std::min<unsigned>(4, hw));

    cxxopts::Options opts(
        "ck-fleet-load",
        "Fleet session load client: S game servers, one fleet session each, carrying P players "
        "who wander and cross between servers.\nThe org token is read from the environment "
        "variable CK_ORG_TOKEN.");
    // clang-format off
    opts.add_options()
        ("api", "Application API GraphQL endpoint, e.g. https://api.example.com/graphql", cxxopts::value<std::string>())
        ("app", "App id: an app of the organization the org token belongs to", cxxopts::value<int64_t>())
        ("servers", "Simulated game servers, one fleet session each", cxxopts::value<int>())
        ("players", "Players carried in total", cxxopts::value<int>())
        ("duration-s", "Measured seconds, counted from when every player has joined (0 = until Ctrl-C)", cxxopts::value<int>())
        ("update-hz", "Actor updates a second for each carried player", cxxopts::value<double>())
        ("cross-rate", "Target crossings a second over all players (0 keeps players inside their region)", cxxopts::value<double>())
        ("signin-rate", "mintManagedPlayerToken calls a second", cxxopts::value<double>())
        ("region-chunks", "Side of a server's region in chunks", cxxopts::value<int>())
        ("seed", "Seed for placement and walks", cxxopts::value<uint64_t>())
        ("own-state", "Set the FLEET_OWN_STATE capability bit", cxxopts::value<bool>())
        ("roster-file", "Save minted player tokens here and reuse them in the next run (mode 0600)", cxxopts::value<std::string>())
        ("dry-run", "No network: an in-process stand-in answers the API and the realtime wire", cxxopts::value<bool>())
        ("dry-run-s", "Seconds the world runs in a dry run", cxxopts::value<double>())
        ("self-test", "Run the built-in checks and exit", cxxopts::value<bool>())
        ("delete-players", "Delete the players this client made (fleet-load-0 to --players - 1, and the rule check's) and exit; needs --org", cxxopts::value<bool>())
        ("org", "The organization's id, for --delete-players", cxxopts::value<int64_t>())
        ("rules-check", "Run the per-player rule checks on carried players and exit", cxxopts::value<bool>())
        ("rules-idle-timeout-s", "The app's idle timeout as the operator set it, for the idle check (0 skips it)", cxxopts::value<double>())
        ("rules-step-s", "Seconds a rule check waits for what it expects", cxxopts::value<double>())
        ("crossing-plan", "carry: the destination session carries and the old one is released (default). release-first: the old session sends FLEET_RELEASE first", cxxopts::value<std::string>())
        ("carry-rate", "FLEET_CARRY a second a session, resends included (the server drops past 64)", cxxopts::value<double>())
        ("carry-burst", "FLEET_CARRY a session may send at once", cxxopts::value<int>())
        ("carry-resend-ms", "Send an unanswered FLEET_CARRY again after this long, with a new time and proof", cxxopts::value<int>())
        ("signed-bundles", "Advertise BUNDLE_SIGNED (--signed-bundles=false reads every frame with its own HMAC)", cxxopts::value<bool>()->default_value("true"))
        ("bound-control", "Advertise BOUND_CONTROL with a nonce, so move commands are 44 and 45", cxxopts::value<bool>()->default_value("true"))
        ("distance", "Replication distance of an actor update, in chunks", cxxopts::value<int>())
        ("decay", "Decay rate 0=none 1=exponential 2..5=linear", cxxopts::value<int>())
        ("chunk-size-units", "Chunk edge in position units", cxxopts::value<double>())
        ("latency-sample", "Players whose updates carry a send time for the replication latency", cxxopts::value<int>())
        ("heartbeat-s", "Seconds between a session's own heartbeats (0 = none)", cxxopts::value<double>())
        ("caps-interval-s", "Seconds between CLIENT_CAPABILITIES", cxxopts::value<int>())
        ("session-settle-ms", "Wait after the API authorizes a session before its first datagram", cxxopts::value<int>())
        ("rx-silent-s", "Move a session that has received nothing for this long (0 = never)", cxxopts::value<int>())
        ("socket-rcvbuf-bytes", "SO_RCVBUF of a session's socket (0 = kernel default)", cxxopts::value<int>())
        ("threads", "Realtime worker threads", cxxopts::value<int>())
        ("api-threads", "Application API threads", cxxopts::value<int>())
        ("stats-interval-s", "Seconds between progress lines on stderr", cxxopts::value<int>())
        ("token-refresh-lead-s", "Refresh a player's app token this many seconds before it expires", cxxopts::value<int>())
        ("drain-s", "Seconds to wait at the end for open carries and releases", cxxopts::value<int>())
        ("summary-file", "Also write the summary JSON to this file", cxxopts::value<std::string>())
        ("tls-insecure", "Skip TLS certificate verification (a local API only)", cxxopts::value<bool>())
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

    auto str = [&](const char* k, std::string& v) { if (cli.count(k)) v = cli[k].as<std::string>(); };
    auto num = [&](const char* k, int& v) { if (cli.count(k)) v = cli[k].as<int>(); };
    auto dbl = [&](const char* k, double& v) { if (cli.count(k)) v = cli[k].as<double>(); };
    auto flag = [&](const char* k, bool& v) { if (cli.count(k)) v = cli[k].as<bool>(); };

    str("api", c.api);
    if (cli.count("app")) c.appId = cli["app"].as<int64_t>();
    num("servers", c.servers);
    num("players", c.players);
    num("duration-s", c.durationS);
    dbl("update-hz", c.updateHz);
    dbl("cross-rate", c.crossRate);
    dbl("signin-rate", c.signinRate);
    num("region-chunks", c.regionChunks);
    if (cli.count("seed")) c.seed = cli["seed"].as<uint64_t>();
    flag("own-state", c.ownState);
    str("roster-file", c.rosterFile);
    flag("dry-run", c.dryRun);
    dbl("dry-run-s", c.dryRunS);
    flag("self-test", c.selfTest);
    flag("delete-players", c.deletePlayers);
    if (cli.count("org")) c.orgId = cli["org"].as<int64_t>();
    flag("rules-check", c.rulesCheck);
    dbl("rules-idle-timeout-s", c.rulesIdleTimeoutS);
    dbl("rules-step-s", c.rulesStepS);
    c.rulesStepGiven = cli.count("rules-step-s") > 0;
    if (cli.count("crossing-plan")) {
        const std::string plan = cli["crossing-plan"].as<std::string>();
        if (plan == "release-first") {
            c.releaseFirst = true;
        } else if (plan != "carry") {
            std::fprintf(stderr, "error: --crossing-plan is carry or release-first\n");
            std::exit(2);
        }
    }
    dbl("carry-rate", c.carryRate);
    num("carry-burst", c.carryBurst);
    num("carry-resend-ms", c.carryResendMs);
    c.signedBundles = cli["signed-bundles"].as<bool>();
    c.boundControl = cli["bound-control"].as<bool>();
    num("distance", c.distance);
    num("decay", c.decay);
    dbl("chunk-size-units", c.chunkSizeUnits);
    num("latency-sample", c.latencySample);
    dbl("heartbeat-s", c.heartbeatS);
    num("caps-interval-s", c.capsIntervalS);
    num("session-settle-ms", c.sessionSettleMs);
    num("rx-silent-s", c.rxSilentS);
    num("socket-rcvbuf-bytes", c.socketRcvbufBytes);
    num("threads", c.threads);
    num("api-threads", c.apiThreads);
    num("stats-interval-s", c.statsIntervalS);
    num("token-refresh-lead-s", c.tokenRefreshLeadS);
    num("drain-s", c.drainS);
    str("summary-file", c.summaryFile);
    flag("tls-insecure", c.tlsInsecure);

    c.signinRateGiven = cli.count("signin-rate") > 0;
    c.sessionSettleGiven = cli.count("session-settle-ms") > 0;
    c.rulesIdleGiven = cli.count("rules-idle-timeout-s") > 0;

    // The credential comes from the environment only, so it is never in argv
    // (which ps shows) and never in a shell history line.
    if (const char* t = std::getenv("CK_ORG_TOKEN")) c.orgToken = t;
    return c;
}

std::string FleetConfig::validate() const {
    if (servers < 1) return "--servers must be at least 1";
    if (servers > 65535) return "--servers must be at most 65535";
    if (players < 1) return "--players must be at least 1";
    if (updateHz <= 0 || updateHz > 500) return "--update-hz must be above 0 and at most 500";
    if (crossRate < 0) return "--cross-rate must be 0 or more";
    if (signinRate <= 0) return "--signin-rate must be above 0";
    if (regionChunks < 1) return "--region-chunks must be at least 1";
    if (distance < 0 || distance > 255) return "--distance must be 0 to 255";
    if (decay < 0 || decay > 5) return "--decay must be 0 to 5";
    if (threads < 1) return "--threads must be at least 1";
    if (apiThreads < 1) return "--api-threads must be at least 1";
    if (durationS < 0) return "--duration-s must be 0 or more";
    if (chunkSizeUnits <= 0) return "--chunk-size-units must be above 0";
    if (latencySample < 0) return "--latency-sample must be 0 or more";
    if (carryRate <= 0 || carryRate > 64) return "--carry-rate must be above 0 and at most 64";
    if (carryBurst < 1) return "--carry-burst must be at least 1";
    if (carryResendMs < 20) return "--carry-resend-ms must be at least 20";
    if (rulesIdleTimeoutS < 0) return "--rules-idle-timeout-s must be 0 or more";
    if (rulesStepS <= 0) return "--rules-step-s must be above 0";
    if (deletePlayers && rulesCheck) return "--delete-players and --rules-check are separate modes";
    if (dryRun || selfTest) return "";
    if (api.empty()) return "--api is required (the Application API's GraphQL endpoint)";
    if (appId <= 0) return "--app is required (an app of the organization)";
    if (orgToken.empty())
        return "the environment variable CK_ORG_TOKEN is not set (an org token with the scopes "
               "players:mint and fleet:session; players:manage as well for --delete-players)";
    if (deletePlayers && orgId <= 0)
        return "--delete-players needs --org (the organization's id)";
    return "";
}

std::string FleetConfig::apiOrigin() const {
    std::string url = api;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const std::string tail = "/graphql";
    if (url.size() >= tail.size() &&
        url.compare(url.size() - tail.size(), tail.size(), tail) == 0) {
        url.resize(url.size() - tail.size());
    }
    return url;
}

} // namespace lt::fleet
