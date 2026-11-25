// End-to-end tests of the fleet load client against the in-process stand-in
// (src/fleet/FakeRealtime.hpp). No network.
//
// Each test runs the whole client: the API calls, the sockets, the worker
// threads, the carries, the crossings and the summary. The stand-in keeps its
// own count of what it was sent, so the client's numbers are checked against a
// second set of books.

#include "fleet/DryRun.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace {

using namespace lt::fleet;
using json = nlohmann::json;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
    std::fflush(stdout);
}

uint64_t u(const json& j) { return j.get<uint64_t>(); }

FleetConfig base() {
    FleetConfig cfg;
    cfg.servers = 9;
    cfg.players = 180;
    cfg.crossRate = 30;
    cfg.dryRunS = 2.0;
    cfg.signinRate = 5000;
    cfg.sessionSettleMs = 0;
    cfg.heartbeatS = 0.5;
    cfg.threads = 2;
    cfg.apiThreads = 4;
    cfg.drainS = 3;
    cfg.statsIntervalS = 3600;
    return cfg;
}

void dumpOnFailure(const FakeRunResult& r, int failuresBefore) {
    if (g_failures == failuresBefore) return;
    json brief = r.summary;
    if (brief.contains("bytes")) brief["bytes"].erase("per_session");
    std::fprintf(stderr, "summary of the failing run:\n%s\n", brief.dump(1).c_str());
}

void testCleanRun() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    const FakeRunResult r = runAgainstFake(base(), FakeRealtime::Options{}, stop, nullptr);
    const json& s = r.summary;

    check(r.exitCode == 0 && s["pass"] == true && s["fail_reasons"].empty(), "clean: the run passes");
    check(s["mode"] == "dry-run" && s["tool"] == "ck-fleet-load", "clean: the summary names the tool and the mode");
    for (const char* key : {"config", "timing", "sessions", "players", "carries", "crossings", "released",
                            "refused", "traffic", "replication", "own_state", "bytes", "api", "generator"}) {
        check(s.contains(key), "clean: the summary has every section");
    }

    // Sessions and players.
    check(u(s["sessions"]["opened"]) == 9 && u(s["sessions"]["closed"]) == 9 && r.openSessionsAtEnd == 0,
          "clean: 9 sessions opened, and every one closed at the end");
    check(u(s["players"]["signed_in"]) == 180 && u(s["players"]["joined"]) == 180 &&
              s["players"]["carried_at_end"] == 180 && s["players"]["not_carried_at_end"] == 0,
          "clean: 180 players signed in once, joined, and were carried at the end");

    // Carries.
    const json& c = s["carries"];
    check(u(c["carried"]) == u(c["attempts"]) && u(c["carried_by_cause"]["join"]) == 180,
          "clean: every carry attempted ended carried");
    check(u(c["refused"]["1_no_live_token"]) + u(c["refused"]["2_other_organization_or_app"]) +
                  u(c["refused"]["3_proof_or_time"]) + u(c["refused"]["4_session_full"]) +
                  u(c["refused"]["other"]) == 0 &&
              u(c["gave_up_after_5s"]) == 0,
          "clean: no carry was refused or given up");
    check(u(c["latency"]["samples"]) == u(c["carried"]) && c["latency"]["p99_ms"].get<double>() >= 0,
          "clean: carry latency has one sample a carry");
    check(r.carriesAccepted == u(c["carried"]) && r.carriesRefused == 0,
          "clean: the stand-in accepted exactly the carries the client counts carried");

    // Crossings.
    const json& x = s["crossings"];
    check(u(x["completed"]) > 20 && x["lost"] == 0 && x["unaccounted"] == 0,
          "clean: players crossed, none was lost, none is unaccounted for");
    check(u(x["attempted"]) == u(x["completed"]) + u(x["superseded"]),
          "clean: attempted is completed plus superseded");
    check(u(c["carried_by_cause"]["crossing"]) == u(x["completed"]),
          "clean: each completed crossing is one carry by the new session");
    check(x["achieved_per_s"].get<double>() > 10 && x["target_per_s"] == 30.0,
          "clean: the achieved crossing rate is reported beside the target");
    check(u(x["latency"]["samples"]) == u(x["completed"]), "clean: crossing latency has one sample a crossing");

    // The old session's FLEET_RELEASED.
    const json& rel = s["released"];
    check(u(rel["old_session"]["expected"]) == u(x["completed"]) &&
              u(rel["old_session"]["received"]) == u(rel["old_session"]["expected"]) &&
              u(rel["old_session"]["never_arrived_within_5s"]) == 0 &&
              rel["old_session"]["pending_at_end"] == 0,
          "clean: every crossing's old session received FLEET_RELEASED reason 1");
    check(u(rel["by_reason"]["1"]) == r.releasedReason1 && rel["by_reason"].size() == 1,
          "clean: reason 1 only, as many as the stand-in sent");
    check(u(rel["old_session"]["delay_after_new_result"]["samples"]) == u(rel["old_session"]["received"]),
          "clean: the delay after the new session's result has one sample a release");
    check(u(rel["reason_1_for_a_held_player"]) == 0 && u(rel["for_a_player_not_held"]) == 0,
          "clean: no release for a player a session still holds");

    // The API: the client's count against the stand-in's own.
    const json& api = s["api"];
    check(u(api["calls_by_kind"]["openFleetSession"]) == r.apiOpen && r.apiOpen == 9,
          "clean: openFleetSession once for each server");
    check(u(api["calls_by_kind"]["mintManagedPlayerToken"]) == r.apiMint && r.apiMint == 180,
          "clean: mintManagedPlayerToken once for each player");
    check(u(api["calls_by_kind"]["closeFleetSession"]) == r.apiClose && r.apiClose == 9,
          "clean: closeFleetSession once for each session");
    check(u(api["calls_by_kind"]["refreshFleetSession"]) == 0 && u(api["calls_by_kind"]["refreshAppToken"]) == 0 &&
              r.apiRefreshSession == 0 && r.apiRefreshAppToken == 0,
          "clean: no refresh was needed");
    check(u(api["calls"]) == r.apiCalls, "clean: the client counted every call the API answered");
    check(u(api["calls_by_cause"]["setup"]) == 189 && u(api["calls_by_cause"]["teardown"]) == 9,
          "clean: every call was setup or teardown");
    bool quiet = true;
    for (const auto& [kind, n] : api["calls_in_measured_window_by_kind"].items()) {
        (void)kind;
        quiet = quiet && u(n) == 0;
    }
    check(quiet, "clean: no API call at all while the players crossed");
    check(u(api["calls_caused_by_crossing"]) == 0 && u(x["api_calls"]) == 0,
          "clean: the number of API calls made because of a crossing is zero");

    // Traffic.
    const json& t = s["traffic"];
    check(u(t["updates_sent"]) > 1000 && r.updatesAccepted + r.updatesUnauthorized == u(t["updates_sent"]),
          "clean: the stand-in received every update the client counts sent");
    check(t["updates_per_s"].get<double>() > 0.9 * 900 && t["updates_per_s"].get<double>() < 1.1 * 900,
          "clean: 180 players at 5 Hz are 900 updates a second");
    check(u(t["hmac_failures"]) == 0 && u(t["malformed"]) == 0 && r.badHmac == 0,
          "clean: every datagram verified, in both directions");
    check(u(t["signed_bundles"]) > 0 && u(t["fleet_frames_outside_signed_bundle"]) == 0,
          "clean: frames arrived in signed bundles");
    check(u(t["releases_sent"]) == 180 && r.releaseRequests == 180,
          "clean: FLEET_RELEASE for each player when the players left");
    check(u(t["capabilities_sent"]) == r.capabilities && r.capabilities >= 9 && r.heartbeats >= 9 &&
              u(t["heartbeats_sent"]) == r.heartbeats,
          "clean: capabilities and heartbeats for each session");
    bool sawActorUpdates = false;
    for (const auto& e : t["received_by_type"]) {
        if (e["type"] == 130) sawActorUpdates = e["name"] == "ACTOR_UPDATE_NOTIFICATION" && u(e["n"]) > 0;
    }
    check(sawActorUpdates, "clean: notifications are counted by type");

    // Refusals: none that is not explained.
    const json& ref = s["refused"];
    check(u(ref["unauthorized_unexplained"]) == 0 && u(ref["unauthorized_session_messages"]) == 0 &&
              u(ref["unauthorized"]) == r.updatesUnauthorized &&
              u(ref["player_messages_refused"]) == u(ref["unauthorized_after_handoff"]),
          "clean: no refusal beyond an old session's last update crossing the new carry");

    // Replication latency and own state.
    check(u(s["replication"]["latency"]["samples"]) > 0 && u(s["replication"]["marked_updates_received"]) > 0 &&
              s["replication"]["sample_players"] == 100,
          "clean: replication latency was measured on the sampled players' updates");
    check(u(s["own_state"]["received"]) == 0 && r.ownStateOut == 0 && s["own_state"]["capability_set"] == false,
          "clean: no session was sent its own players' state");

    // Bytes and the generator.
    uint64_t tx = 0, rx = 0;
    for (const auto& p : s["bytes"]["per_session"]) {
        tx += u(p["tx_bytes"]);
        rx += u(p["rx_bytes"]);
    }
    check(s["bytes"]["per_session"].size() == 9 && tx == u(s["bytes"]["tx"]) && rx == u(s["bytes"]["rx"]) &&
              tx > 0 && rx > 0,
          "clean: bytes sent and received per session add up to the totals");
    check(u(s["generator"]["send_lag"]["samples"]) == u(t["updates_sent"]),
          "clean: the generator's own send lag is recorded for every update");
    dumpOnFailure(r, before);
}

void testFaults() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.crossRate = 6;
    cfg.dryRunS = 3.0;
    FakeRealtime::Options opt;
    opt.mintBusyEvery = 7;
    opt.mintRateLimitedEvery = 11;
    int step = 0;
    int ended = -1, idled = -1;
    const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr, [&](double t, FakeRealtime& fake) {
        if (step == 0 && t > 0.3) {
            ++step;
            fake.tellSessionToMove(4, false, 0); // COMMAND_RECONNECT
        } else if (step == 1 && t > 0.5) {
            ++step;
            fake.tellSessionToMove(2, true, 3); // COMMAND_SESSION_RELEASED, overload
        } else if (step == 2 && t > 1.4) {
            ++step;
            for (int n = 0; n < 180 && ended < 0; ++n) {
                if (fake.carrierOf(n) >= 0) ended = n;
            }
            fake.endSignIn(ended); // FLEET_RELEASED reason 2
        } else if (step == 3 && t > 1.7) {
            ++step;
            for (int n = 0; n < 180 && idled < 0; ++n) {
                if (n != ended && fake.carrierOf(n) >= 0) idled = n;
            }
            fake.idleRelease(idled); // FLEET_RELEASED reason 5
        }
    });
    const json& s = r.summary;
    check(step == 4 && ended >= 0 && idled >= 0, "faults: all four were injected");
    check(r.exitCode == 0 && s["players"]["not_carried_at_end"] == 0, "faults: the run passes, everyone carried at the end");

    // The two sessions told to move.
    const json& ses = s["sessions"];
    check(u(ses["moves_by_trigger"]["reconnect_command"]) == 1 && u(ses["reconnect_commands"]) == 1,
          "faults: COMMAND_RECONNECT was verified and moved the session");
    check(u(ses["moves_by_trigger"]["session_released"]) == 1 && u(ses["session_released_by_reason"]["3"]) == 1,
          "faults: COMMAND_SESSION_RELEASED was verified, with its reason");
    check(u(ses["moves_completed"]) == 2 && u(ses["move_failures"]) == 0, "faults: both sessions reconnected");
    check(u(s["api"]["calls_by_kind"]["refreshFleetSession"]) == 2 && r.apiRefreshSession == 2 &&
              r.apiRefreshSessionWithoutServer == 0,
          "faults: refreshFleetSession once for each, naming the current server");
    check(u(s["api"]["calls_by_cause"]["session_move"]) == 2, "faults: counted as session moves");
    check(u(s["carries"]["carried_by_cause"]["session_move"]) > 0,
          "faults: a moved session carried its players again");

    // The ended sign-in and the idle release.
    const json& rel = s["released"];
    check(u(rel["by_reason"]["2"]) == 1 && u(rel["reason_2_for_a_held_player"]) == 1,
          "faults: FLEET_RELEASED reason 2 is reported");
    check(u(s["players"]["signed_in_again"]) == 1 && u(s["api"]["calls_by_cause"]["sign_in_again"]) >= 1 &&
              u(s["carries"]["carried_by_cause"]["sign_in_again"]) == 1,
          "faults: that player was signed in again and carried again");
    check(u(rel["by_reason"]["5"]) == 1 && u(rel["reason_5_for_a_held_player"]) == 1 &&
              u(rel["carried_again"]) >= 1 && u(s["carries"]["carried_by_cause"]["recarry"]) >= 1,
          "faults: FLEET_RELEASED reason 5 is reported, and that player was carried again");

    // PLATFORM_BUSY and RATE_LIMITED at sign-in.
    check(u(s["players"]["platform_busy"]) >= 10 && u(s["players"]["rate_limited"]) >= 10,
          "faults: PLATFORM_BUSY and RATE_LIMITED are each counted");
    check(u(s["api"]["calls_by_kind"]["mintManagedPlayerToken"]) == r.apiMint &&
              r.apiMint == 180 + u(s["players"]["platform_busy"]) + u(s["players"]["rate_limited"]) +
                               u(s["players"]["signed_in_again"]),
          "faults: every retried sign-in is a counted API call");
    check(u(s["players"]["signed_in"]) == 180, "faults: every player was signed in all the same");

    check(u(s["api"]["calls"]) == r.apiCalls, "faults: the client counted every call the API answered");
    check(u(s["api"]["calls_caused_by_crossing"]) == 0 && u(s["crossings"]["completed"]) > 0,
          "faults: still no API call because of a crossing");
    check(u(s["api"]["calls_in_measured_window_by_kind"]["refreshFleetSession"]) == 2 &&
              u(s["api"]["calls_in_measured_window_by_kind"]["mintManagedPlayerToken"]) >= 1,
          "faults: the calls inside the window are the two moves and the one sign-in again");
    dumpOnFailure(r, before);
}

void testLoss() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.carryResendMs = 60; // the contract's 250 ms, shortened so a short run sees resends
    FakeRealtime::Options opt;
    opt.dropToClient = 0.05;
    opt.dropToServer = 0.05;
    const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
    const json& s = r.summary;
    check(r.exitCode == 0 && s["players"]["not_carried_at_end"] == 0 && s["crossings"]["lost"] == 0,
          "loss: with 5% of datagrams lost each way, nobody is lost");
    check(u(s["carries"]["sent"]) > u(s["carries"]["attempts"]),
          "loss: a carry whose datagram or result was lost was sent again");
    check(u(s["carries"]["carried"]) + u(s["carries"]["abandoned_player_moved_on"]) +
                  u(s["carries"]["gave_up_after_5s"]) >= u(s["carries"]["attempts"]),
          "loss: every carry ended carried, or the player moved on");
    check(u(s["traffic"]["hmac_failures"]) == 0 && u(s["own_state"]["received"]) == 0,
          "loss: what did arrive verified");
    check(u(s["api"]["calls_caused_by_crossing"]) == 0, "loss: no API call because of a crossing");
    dumpOnFailure(r, before);
}

void testTokenRefresh() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 4.5;
    cfg.tokenRefreshLeadS = 2;
    FakeRealtime::Options opt;
    opt.playerTokenTtlMs = 4000; // a player's token lasts 4 s here, so it is refreshed in the run
    const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
    const json& s = r.summary;
    check(r.exitCode == 0 && s["players"]["not_carried_at_end"] == 0, "token: the run passes");
    check(u(s["players"]["token_refreshes"]) >= 180 && u(s["players"]["token_refresh_failures"]) == 0,
          "token: every player's app token was refreshed before it expired");
    check(u(s["api"]["calls_by_kind"]["refreshAppToken"]) == r.apiRefreshAppToken &&
              r.apiRefreshAppToken == u(s["players"]["token_refreshes"]),
          "token: refreshAppToken once a refresh, as the API counted");
    check(u(s["api"]["calls_by_cause"]["token_refresh"]) == r.apiRefreshAppToken &&
              u(s["api"]["calls_by_cause"]["crossing"]) == 0,
          "token: those calls are counted as token refreshes, apart from crossings");
    check(u(s["carries"]["carried_by_cause"]["token_refresh"]) >= 180,
          "token: each player was carried again with the new token");
    check(!s["released"]["by_reason"].contains("2") && u(s["players"]["signed_in_again"]) == 0,
          "token: the old carry was replaced without a gap (no reason 2, nobody signed in again)");
    check(u(s["refused"]["unauthorized_unexplained"]) == 0,
          "token: no update was refused across the change of token");
    check(u(s["api"]["calls_in_measured_window_by_kind"]["refreshAppToken"]) > 0 &&
              u(s["api"]["calls_caused_by_crossing"]) == 0 && u(s["crossings"]["completed"]) > 0,
          "token: API calls happened during the window, and none was for a crossing");
    dumpOnFailure(r, before);
}

void testOwnState() {
    std::atomic<bool> stop{false};
    {
        // The stand-in breaks the rule: it sends a session its own players' state.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.ownStateBug = true;
        FleetConfig cfg = base();
        cfg.dryRunS = 1.2;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        check(r.exitCode == 1 && s["pass"] == false, "own state: sent back unasked, the run fails");
        check(u(s["own_state"]["received"]) > 0 && u(s["own_state"]["received"]) <= r.ownStateOut,
              "own state: every copy that came back is counted");
        bool said = false;
        for (const auto& why : s["fail_reasons"])
            said = said || why.get<std::string>().find("FLEET_OWN_STATE") != std::string::npos;
        check(said, "own state: the failure says why");
        dumpOnFailure(r, before);
    }
    {
        // With the capability set it is expected, and measured.
        const int before = g_failures;
        FleetConfig cfg = base();
        cfg.dryRunS = 1.2;
        cfg.ownState = true;
        const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, nullptr);
        const json& s = r.summary;
        check(r.exitCode == 0 && s["own_state"]["capability_set"] == true,
              "own state: with --own-state the run passes");
        check(u(s["own_state"]["received"]) > 0 && r.ownStateOut > 0,
              "own state: with --own-state the session's own players' state comes back");
        dumpOnFailure(r, before);
    }
}

void testOtherFramings() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FakeRealtime::Options opt;
    opt.ignoreCapabilities = true;   // plain bundles, each notification signed for the session
    opt.longFormFleetFrames = true;  // 51 and 52 as long-form app payloads
    FleetConfig cfg = base();
    cfg.dryRunS = 1.2;
    const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
    const json& s = r.summary;
    const json& t = s["traffic"];
    check(r.exitCode == 0 && u(s["crossings"]["completed"]) > 0 && s["crossings"]["lost"] == 0,
          "framing: plain bundles and long-form fleet frames are read as well");
    check(u(t["signed_bundles"]) == 0 && u(t["plain_bundles"]) > 0, "framing: no signed bundle was sent");
    check(u(t["fleet_frames_outside_signed_bundle"]) > 0 && u(t["fleet_frames_long_form"]) > 0,
          "framing: the summary says how the fleet frames arrived");
    check(u(t["hmac_failures"]) == 0 && u(t["unsigned_notifications"]) == 0 &&
              u(s["replication"]["latency"]["samples"]) > 0,
          "framing: each notification's own HMAC verified with the fleet token");
    dumpOnFailure(r, before);
}

void testRefusals() {
    std::atomic<bool> stop{false};
    {
        // Another organization's player: status 2, which no retry can help.
        const int before = g_failures;
        FleetConfig cfg = base();
        cfg.dryRunS = 1.0;
        const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, nullptr, nullptr,
                                               [](FakeRealtime& fake) { fake.setForeign(7); });
        const json& s = r.summary;
        check(r.exitCode == 1 && u(s["carries"]["refused"]["2_other_organization_or_app"]) >= 1 &&
                  s["players"]["refused_for_good"] == 1 && s["players"]["carried_at_end"] == 179,
              "refusal: status 2 is counted, the player is not carried, and the run fails");
        dumpOnFailure(r, before);
    }
    {
        // More players in a region than a session may carry: status 4.
        const int before = g_failures;
        FleetConfig cfg = base();
        cfg.servers = 4;
        cfg.players = 100;
        cfg.dryRunS = 1.5;
        cfg.carryGiveUpMs = 400;
        cfg.carryRetryMs = 150;
        cfg.drainS = 1;
        FakeRealtime::Options opt;
        opt.maxPlayers = 10;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        check(r.exitCode == 1 && u(s["carries"]["refused"]["4_session_full"]) > 0 &&
                  s["players"]["not_carried_at_end"].get<int>() > 0,
              "refusal: status 4 is counted, and players left uncarried fail the run");
        check(u(s["carries"]["gave_up_after_5s"]) > 0 && u(s["carries"]["retried"]) > 0,
              "refusal: a carry with no success is given up, counted, and started again");
        check(s["sessions"]["max_players"] == 10 && s["players"]["carried_at_end"].get<int>() <= 40,
              "refusal: no session carries more than it may");
        dumpOnFailure(r, before);
    }
    {
        // This host's clock a minute off the server's: status 3.
        const int before = g_failures;
        FleetConfig cfg = base();
        cfg.dryRunS = 0.5;
        cfg.carryGiveUpMs = 300;
        cfg.carryRetryMs = 100;
        cfg.drainS = 1;
        FakeRealtime::Options opt;
        opt.clockSkewMs = 60000;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        check(r.exitCode == 1 && u(s["carries"]["refused"]["3_proof_or_time"]) > 0 &&
                  s["players"]["carried_at_end"] == 0,
              "refusal: a time more than 30 s off is status 3, and nobody is carried");
        check(u(s["sessions"]["closed"]) == 9 && r.openSessionsAtEnd == 0,
              "refusal: a failed run still closes its sessions");
        dumpOnFailure(r, before);
    }
}

void testInterrupt() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 30.0;
    const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, nullptr,
                                           [&](double t, FakeRealtime&) {
                                               if (t > 0.5) stop.store(true); // as SIGINT does
                                           });
    const json& s = r.summary;
    check(r.exitCode == 130 && s["interrupted"] == true, "interrupt: the run ends with exit code 130");
    check(u(s["sessions"]["closed"]) == 9 && r.apiClose == 9 && r.openSessionsAtEnd == 0,
          "interrupt: every session is closed all the same");
    check(s["timing"]["measured_s"].get<double>() < 5.0 && u(s["traffic"]["updates_sent"]) > 0,
          "interrupt: the summary covers what ran");
    dumpOnFailure(r, before);
}

void testRosterReuse() {
    const int before = g_failures;
    char dirTemplate[] = "/tmp/fleet-run-roster-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    if (!dir) {
        check(false, "roster: a scratch directory");
        return;
    }
    const std::string path = std::string(dir) + "/players-roster.json";
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 1.0;
    cfg.rosterFile = path;

    FakeRealtime fake(4242, FakeRealtime::Options{});
    fake.start();
    const FakeRunResult first = runAgainstFake(fake, cfg, stop, nullptr);
    struct stat st {};
    check(first.exitCode == 0 && u(first.summary["players"]["signed_in"]) == 180 &&
              u(first.summary["players"]["from_roster"]) == 0,
          "roster: the first run signs every player in");
    check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "roster: and saves their tokens in a file of mode 0600");

    const FakeRunResult second = runAgainstFake(fake, cfg, stop, nullptr);
    const json& s = second.summary;
    check(second.exitCode == 0 && u(s["players"]["from_roster"]) == 180 && u(s["players"]["signed_in"]) == 0,
          "roster: the second run reuses them and signs nobody in");
    check(second.apiMint == 0 && u(s["api"]["calls_by_kind"]["mintManagedPlayerToken"]) == 0 &&
              second.apiOpen == 9 && second.apiClose == 9,
          "roster: so its only API calls open and close the sessions");
    check(s["players"]["carried_at_end"] == 180 && u(s["crossings"]["completed"]) > 0,
          "roster: the reused tokens carry the players");
    check(s.dump().find("players-roster") == std::string::npos,
          "roster: the summary does not point at the file");

    // A roster minted for another app is refused before anything is opened.
    FakeRealtime other(999, FakeRealtime::Options{});
    other.start();
    const FakeRunResult refused = runAgainstFake(other, cfg, stop, nullptr);
    check(refused.exitCode == 2 && refused.apiOpen == 0 && refused.apiCalls == 0,
          "roster: one minted for another app stops the run before any API call");
    other.stop();
    fake.stop();
    unlink(path.c_str());
    rmdir(dir);
    dumpOnFailure(second, before);
}

void testSecretsStayOut() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 0.5;
    char* captured = nullptr;
    size_t capturedLen = 0;
    FILE* log = open_memstream(&captured, &capturedLen);
    cfg.statsIntervalS = 1;
    const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, log);
    std::fclose(log);
    const std::string logged(captured ? captured : "", capturedLen);
    std::free(captured);
    const std::string summary = r.summary.dump();
    check(!logged.empty() && logged.find(FakeRealtime::ORG_TOKEN) == std::string::npos &&
              summary.find(FakeRealtime::ORG_TOKEN) == std::string::npos,
          "secrecy: the org token is in neither the log nor the summary");
    check(summary.find("\"token\"") == std::string::npos && summary.find("org_token") == std::string::npos,
          "secrecy: the summary has no token field at all");
    dumpOnFailure(r, before);
}

} // namespace

int main() {
    testCleanRun();
    testFaults();
    testLoss();
    testTokenRefresh();
    testOwnState();
    testOtherFramings();
    testRefusals();
    testInterrupt();
    testRosterReuse();
    testSecretsStayOut();

    if (g_failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all fleet run tests passed\n");
    return 0;
}
