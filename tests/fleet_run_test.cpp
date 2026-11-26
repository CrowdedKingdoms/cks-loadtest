// End-to-end tests of the fleet load client against the in-process stand-in
// (src/fleet/FakeRealtime.hpp). No network.
//
// Each test runs the whole client: the API calls, the sockets, the worker
// threads, the carries, the crossings and the summary. The stand-in keeps its
// own count of what it was sent, so the client's numbers are checked against a
// second set of books.

#include "fleet/DryRun.hpp"
#include "fleet/Modes.hpp"

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
                            "refused", "traffic", "replication", "own_state", "bytes", "api", "generator",
                            "usage", "proof"}) {
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
    const json& byStatus = c["by_status"];
    check(u(byStatus["0_carried"]) == u(c["carried"]) && u(byStatus["1_no_live_token"]) == 0 &&
              u(byStatus["2_other_organization"]) == 0 && u(byStatus["3_proof_time_or_replay"]) == 0 &&
              u(byStatus["4_session_full"]) == 0 && u(byStatus["other"]) == 0 && byStatus.size() == 6 &&
              u(c["gave_up_after_5s"]) == 0,
          "clean: carries are reported by status under the contract's names, and none was refused");
    check(u(c["resent"]) == 0 && r.fake.at("carriesDropped") == 0 && r.fake.at("carriesReplayed") == 0,
          "clean: no carry had to be sent again, none passed the carry rate, none was a replay");
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
    check(u(rel["by_reason"]["1_carried_by_another_session"]) == r.releasedReason1 &&
              u(rel["by_reason"]["2_sign_in_ended"]) == 0 && u(rel["by_reason"]["5_idle"]) == 0 &&
              u(rel["by_reason"]["other"]) == 0,
          "clean: releases are reported by reason under the contract's names: reason 1 only");
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
    for (const auto& [kind, n] : api["calls_in_measured_phase_by_kind"].items()) {
        (void)kind;
        quiet = quiet && u(n) == 0;
    }
    check(quiet && u(api["calls_in_measured_phase"]) == 0 && u(api["calls_in_measured_phase_other"]) == 0,
          "clean: no API call at all in the measured phase, while the players crossed");
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
    check(u(t["signed_bundles"]) > 0 && u(t["fleet_frames_in_signed_bundles"]) > 0 &&
              u(t["fleet_frames_with_own_hmac"]) == 0 && u(t["fleet_frame_hmac_failures"]) == 0 &&
              r.fake.at("framesInSignedBundle") == u(t["fleet_frames_in_signed_bundles"]),
          "clean: 51 and 52 arrived as members of signed bundles, every one the stand-in sent");
    check(r.fake.at("ownIdRefused") == 0 && r.fake.at("capabilitiesUnderPlayerId") == 0 &&
              r.fake.at("actorRefused") == 0 && r.fake.at("playerHeartbeats") == 0,
          "clean: under a session's own id went only 29, 49, 50 and 26; everything else under a player's");
    check(u(s["sessions"]["bound_control_acknowledged"]) == 9 && r.fake.at("boundAcksSent") == 9,
          "clean: each session's control nonce was acknowledged");
    uint64_t plainState = 0, sessionDeliveries = 0;
    for (const auto& p : s["bytes"]["per_session"]) {
        plainState += u(p["plain_state_notifications"]);
        sessionDeliveries += u(p["deliveries"]) + u(p["plain_communications"]);
    }
    check(plainState == u(t["plain_state_notifications"]) && plainState == u(t["notifications_received"]) &&
              plainState == r.fake.at("notificationsOut") && sessionDeliveries == 0 &&
              u(t["plain_communications"]) == 0,
          "clean: state arrived plain, counted a session; no FLEET_DELIVERY and no plain communication");
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

    // The session-minutes, for the bill.
    uint64_t sent = 0, received = 0, sentDatagrams = 0, receivedDatagrams = 0;
    double carriedSeconds = 0;
    bool shaped = !s["usage"]["per_session_minute"].empty();
    for (const auto& m : s["usage"]["per_session_minute"]) {
        sent += u(m["sent_bytes"]);
        received += u(m["received_bytes"]);
        sentDatagrams += u(m["sent_datagrams"]);
        receivedDatagrams += u(m["received_datagrams"]);
        carriedSeconds += m["carried_player_seconds"].get<double>();
        shaped = shaped && m["minute"].get<std::string>().size() == 20 &&
                 m["game_token_id"].get<std::string>().rfind("70000000", 0) == 0 &&
                 u(m["received_wire_bytes"]) == u(m["received_bytes"]) + 42 * u(m["received_datagrams"]);
    }
    check(shaped, "usage: a row for each session and minute, with the session's game token id");
    check(sent == u(s["bytes"]["tx"]) && received == u(s["bytes"]["rx"]) &&
              sentDatagrams == u(s["bytes"]["tx_datagrams"]) &&
              receivedDatagrams == u(s["bytes"]["rx_datagrams"]),
          "usage: the session-minutes add up to every byte and datagram sent and received");
    const double ran = s["timing"]["total_s"].get<double>();
    check(carriedSeconds > 180 * 1.5 && carriedSeconds < 180 * (ran + 0.5),
          "usage: carried player-seconds are the players carried times the seconds they were");

    const json& proof = s["proof"];
    check(u(proof["sessions_opened"]) == 9 && proof["players_carried"] == 180 &&
              proof["carries_by_status"] == byStatus && proof["releases_by_reason"] == rel["by_reason"] &&
              u(proof["crossings_attempted"]) == u(x["attempted"]) &&
              u(proof["crossings_completed"]) == u(x["completed"]) &&
              proof["crossings_per_s"] == x["achieved_per_s"] && proof["crossing_plan"] == "carry",
          "proof: sessions, players carried, carries by status, releases by reason, crossings");
    check(u(proof["carry_retries"]) == 0 && u(proof["api_calls_in_measured_phase"]) == 0 &&
              u(proof["api_calls_caused_by_crossing"]) == 0 && u(proof["hmac_failures"]) == 0 &&
              u(proof["unauthorized_answers"]) == r.updatesUnauthorized &&
              u(proof["refused"]["total"]) == r.updatesUnauthorized &&
              u(proof["datagrams_sent"]) == sentDatagrams && u(proof["bytes_received"]) == received,
          "proof: carry retries, API calls in the measured phase, HMAC failures, refusals, datagrams, bytes");
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
    check(s["players"]["not_carried_at_end"] == 0 && s["crossings"]["lost"] == 0,
          "faults: everyone was carried at the end, and no crossing lost a player");

    // The two sessions told to move.
    const json& ses = s["sessions"];
    check(u(ses["moves_by_trigger"]["reconnect_command"]) == 1 && u(ses["reconnect_commands"]) == 1,
          "faults: COMMAND_RECONNECT (44, bound to the session's nonce) was verified and moved the session");
    check(u(ses["moves_by_trigger"]["session_released"]) == 1 && u(ses["session_released_by_reason"]["3"]) == 1,
          "faults: COMMAND_SESSION_RELEASED was verified, with its reason");
    check(u(ses["moves_completed"]) == 2 && u(ses["move_failures"]) == 0 &&
              u(ses["came_back_to_another_server"]) == 2 && u(ses["came_back_to_the_same_server"]) == 0,
          "faults: both sessions came back on another server");
    check(u(s["api"]["calls_by_kind"]["refreshFleetSession"]) == 2 && r.apiRefreshSession == 2 &&
              r.apiRefreshSessionWithoutServer == 2,
          "faults: refreshFleetSession once for each, leaving currentServer out so the API names a new one");
    check(u(s["api"]["calls_by_cause"]["session_move"]) == 2, "faults: counted as session moves");
    check(u(s["carries"]["carried_by_cause"]["session_move"]) > 0,
          "faults: a moved session carried its players again");

    // The ended sign-in and the idle release.
    const json& rel = s["released"];
    check(u(rel["by_reason"]["2_sign_in_ended"]) == 1 && u(rel["reason_2_for_a_held_player"]) == 1,
          "faults: FLEET_RELEASED reason 2 is reported");
    check(u(s["players"]["signed_in_again"]) == 1 && u(s["api"]["calls_by_cause"]["sign_in_again"]) >= 1 &&
              u(s["carries"]["carried_by_cause"]["sign_in_again"]) == 1,
          "faults: that player was signed in again and carried again");
    check(u(rel["by_reason"]["5_idle"]) == 1 && u(rel["reason_5_for_a_held_player"]) == 1 &&
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
    check(u(s["api"]["calls_in_measured_phase_by_kind"]["refreshFleetSession"]) == 2 &&
              u(s["api"]["calls_in_measured_phase_by_kind"]["mintManagedPlayerToken"]) >= 1 &&
              u(s["api"]["calls_in_measured_phase_by_cause"]["session_move"]) == 2 &&
              u(s["api"]["calls_in_measured_phase_by_cause"]["sign_in_again"]) >= 1 &&
              u(s["api"]["calls_in_measured_phase_refreshes_due"]) == 0 &&
              u(s["api"]["calls_in_measured_phase_other"]) >= 3,
          "faults: the calls in the measured phase are the two moves and the one sign-in again");
    bool named = false;
    for (const auto& why : s["fail_reasons"]) {
        const std::string text = why.get<std::string>();
        named = named || (text.find("measured phase") != std::string::npos &&
                          text.find("session_move") != std::string::npos &&
                          text.find("sign_in_again") != std::string::npos);
    }
    check(r.exitCode == 1 && s["fail_reasons"].size() == 1 && named,
          "faults: API calls in the measured phase that are not refreshes fail the run, and are named");
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
    check(u(s["carries"]["sent"]) > u(s["carries"]["attempts"]) && u(s["carries"]["resent"]) > 0 &&
              u(s["proof"]["carry_retries"]) == u(s["carries"]["resent"]) +
                                                     u(s["carries"]["started_again_after_giving_up"]),
          "loss: an unanswered carry was sent again with a new time and proof, and the retries are counted");
    check(r.fake.at("carriesReplayed") == 0, "loss: no resent carry was taken for a replay");
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
    check(u(s["released"]["by_reason"]["2_sign_in_ended"]) == 0 && u(s["players"]["signed_in_again"]) == 0,
          "token: the old carry was replaced without a gap (no reason 2, nobody signed in again)");
    check(u(s["refused"]["unauthorized_unexplained"]) == 0,
          "token: no update was refused across the change of token");
    check(u(s["api"]["calls_in_measured_phase_by_kind"]["refreshAppToken"]) > 0 &&
              u(s["api"]["calls_in_measured_phase_refreshes_due"]) == u(s["api"]["calls_in_measured_phase"]) &&
              u(s["api"]["calls_in_measured_phase_other"]) == 0 &&
              u(s["api"]["calls_caused_by_crossing"]) == 0 && u(s["crossings"]["completed"]) > 0,
          "token: the only API calls in the measured phase are refreshes that fell due, and the run passes");
    dumpOnFailure(r, before);
}

const json* typeRow(const json& traffic, int type) {
    for (const auto& row : traffic["by_type_and_arrival"])
        if (row["type"] == type) return &row;
    return nullptr;
}

void testAroundATokenRefresh() {
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 4.5;
    cfg.tokenRefreshLeadS = 2;
    cfg.crossRate = 90;
    {
        // The realtime side answers a carry a quarter of a second late. Meanwhile
        // the session goes on sending under the old token id, which the stand-in
        // (like the dev tier) refuses once it has accepted the carry with the new
        // one; and some players cross away before the answer comes.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.playerTokenTtlMs = 4000;
        opt.carryResultDelayMs = 250;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        const json& refused = s["refused"];
        check(r.exitCode == 0 && s["players"]["not_carried_at_end"] == 0 &&
                  u(s["players"]["token_refreshes"]) >= 180,
              "refresh: with slow carry results the run passes and every token was refreshed");
        check(u(refused["unauthorized_during_token_refresh"]) > 0 &&
                  u(refused["unauthorized_during_token_refresh"]) ==
                      u(refused["unauthorized_during_token_refresh_before_the_new_carry_result"]) +
                          u(refused["unauthorized_during_token_refresh_after_the_new_carry_result"]),
              "refresh: an update under the old token id, refused while the new carry is unanswered, "
              "is classed as a refusal during a token refresh");
        check(u(refused["unauthorized_unexplained"]) == 0 && u(refused["unauthorized_session_messages"]) == 0 &&
                  u(refused["unauthorized"]) ==
                      u(refused["unauthorized_during_token_refresh"]) + u(refused["unauthorized_after_handoff"]) +
                          u(refused["unauthorized_after_the_carry_ended"]) +
                          u(refused["unauthorized_first_contact"]) + u(refused["unauthorized_window_reload"]),
              "refresh: every refusal has a class, and none is unexplained");
        const json& events = s["events"];
        bool described = events.contains("unauthorized_during_token_refresh");
        if (described) {
            const json& e = events["unauthorized_during_token_refresh"]["events"][0];
            described = e["message_type"] == 128 && e.contains("player") &&
                        e.contains("carry_with_a_new_token_unanswered_when_sent") &&
                        e.contains("token_refresh_in_flight_when_sent") &&
                        e.contains("sent_ms_after_the_last_carry_result_here") &&
                        e["named_a_token_id_replaced_since"] == true;
        }
        check(described, "refresh: each such refusal says the message type, the player, the time since "
                         "the last carry result and whether a refresh or a new carry was pending");
        // A player that crossed away while its carry with the new token was
        // unanswered: the old session's FLEET_RELEASED names the new token id.
        const json& old = s["released"]["old_session"];
        check(u(old["named_the_token_id_of_a_carry_left_unanswered"]) > 0 &&
                  u(old["never_arrived_within_5s"]) == 0 && u(old["received"]) == u(old["expected"]),
              "refresh: a FLEET_RELEASED that names the token id of a carry the player left unanswered "
              "is paired with its crossing, and none is counted as missing");
        check(u(s["carries"]["token_carry_sent_after_the_refresh_answer"]["samples"]) >= 180,
              "refresh: the time from the refresh's answer to the carry with the new token is measured");
        dumpOnFailure(r, before);
    }
    {
        // The realtime side learns of a refreshed token 300 ms after the API
        // answered: a carry that names it sooner is answered status 1.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.playerTokenTtlMs = 4000;
        opt.newTokenLagMs = 300;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& c = r.summary["carries"];
        check(u(c["by_status"]["1_no_live_token"]) >= 180 &&
                  u(c["retried_after_refresh"]) == u(c["by_status"]["1_no_live_token"]) &&
                  u(c["retried_after_refresh_after_a_wait"]) >= 180 &&
                  u(c["status_1_not_explained_by_a_refresh"]) == 0,
              "refresh: status 1 for a token stored moments ago is waited out and the carry sent again");
        check(r.exitCode == 0 && r.summary["players"]["not_carried_at_end"] == 0 &&
                  u(r.summary["players"]["signed_in_again"]) == 0 &&
                  u(r.summary["released"]["by_reason"]["2_sign_in_ended"]) == 0 &&
                  u(r.summary["api"]["calls_in_measured_phase_other"]) == 0,
              "refresh: nobody signs in again, the old carry stands meanwhile, and the run passes");
        bool described = r.summary["events"].contains("carry_status_1");
        if (described) {
            const json& e = r.summary["events"]["carry_status_1"]["events"][0];
            described = e["client_did"] == "waited_and_carried_again" &&
                        e.contains("carry_sent_ms_after_the_newest_token_was_stored") &&
                        e["named_the_newest_token_when_sent"] == true;
        }
        check(described, "refresh: each status 1 says how long after the refresh's answer the carry was "
                         "sent and what the client did next");
        dumpOnFailure(r, before);
    }
    {
        // The API replaces the token 150 ms before its answer arrives. A crossing
        // in that time carries with the token the client still has, which the
        // realtime side no longer takes for a new carry: status 1.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.playerTokenTtlMs = 4000;
        opt.refreshAnswerDelayMs = 150;
        FleetConfig busy = cfg;
        busy.apiThreads = 32;
        busy.crossRate = 120;
        const FakeRunResult r = runAgainstFake(busy, opt, stop, nullptr);
        const json& c = r.summary["carries"];
        check(u(c["by_status"]["1_no_live_token"]) > 0 &&
                  u(c["retried_after_refresh"]) == u(c["by_status"]["1_no_live_token"]) &&
                  u(c["status_1_not_explained_by_a_refresh"]) == 0,
              "refresh: a carry that named the token a refresh was replacing is sent again with the new one");
        check(r.exitCode == 0 && r.summary["players"]["not_carried_at_end"] == 0 &&
                  u(r.summary["players"]["signed_in_again"]) == 0 && r.summary["crossings"]["lost"] == 0,
              "refresh: nobody signs in again and no crossing is lost");
        dumpOnFailure(r, before);
    }
}

void testWhoSentTheState() {
    std::atomic<bool> stop{false};
    {
        // State is held 260 ms before it goes out, longer than the 200 ms between
        // a player's updates, so the old session's last update for a player
        // reaches the new session after its carry result.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.stateDelayMs = 260;
        FleetConfig cfg = base();
        cfg.dryRunS = 3.0;
        cfg.crossRate = 90;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& own = r.summary["own_state"];
        check(r.exitCode == 0 && u(own["sent_by_the_receiving_session"]["total"]) == 0 &&
                  u(own["received"]) == 0,
              "handoff: no session received an update it sent, and the run passes");
        check(u(own["sent_by_another_session_before_the_handoff"]["total"]) > 0 &&
                  own["sent_by_another_session_before_the_handoff"]["fails_a_run"] == false,
              "handoff: an update another session sent for a player the receiver carries now is "
              "counted apart and does not fail the run");
        const json& lead = own["sent_by_another_session_before_the_handoff"]["sent_before_the_receivers_carry_result"];
        check(u(lead["samples"]) == u(own["sent_by_another_session_before_the_handoff"]["total"]) &&
                  lead["max_ms"].get<double>() > 100 && lead["max_ms"].get<double>() < 320 &&
                  lead.contains("p99_ms"),
              "handoff: how long before the receiver's carry result it was sent is reported (max, p99)");
        check(u(own["sent_by_another_session_after_the_receivers_carry_result"]) == 0,
              "handoff: no other session sent for a player after the receiver's carry result");
        dumpOnFailure(r, before);
    }
    {
        // The defect seen on the dev tier: now and then one more copy of an update
        // arrives alone in a datagram with no HMAC, at the session that sent it or
        // at a session far out of range.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.strayUnsignedEvery = 40;
        FleetConfig cfg = base();
        cfg.dryRunS = 2.0;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        const json& mine = s["own_state"]["sent_by_the_receiving_session"];
        check(r.exitCode == 1 && u(mine["total"]) > 0 && u(mine["total"]) == u(s["own_state"]["received"]) &&
                  u(mine["total"]) == r.fake.at("strayUnsignedToSender"),
              "stray: state that came back to the session that sent it fails the run");
        check(u(mine["with_no_hmac_outside_a_signed_bundle"]) == u(mine["total"]) &&
                  u(mine["while_it_still_carried_the_player"]) + u(mine["after_the_player_had_left_it"]) ==
                      u(mine["total"]),
              "stray: each one is counted with how it arrived and whether the session still carried the player");
        const json& t = s["traffic"];
        const json* row = typeRow(t, 130);
        check(u(t["unsigned_notifications"]) == r.fake.at("strayUnsigned") &&
                  u(t["unsigned_notifications"]) > u(mine["total"]) && row &&
                  u((*row)["alone_in_a_datagram"]) == u(t["unsigned_notifications"]) &&
                  u((*row)["outside_signed_bundles_no_hmac"]) == u(t["unsigned_notifications"]) &&
                  u((*row)["in_signed_bundles"]) > 0 && t["unsigned_notifications_by_type"][0]["type"] == 130,
              "stray: every notification with no HMAC is counted by type and by how it arrived");
        const json& were = t["unsigned_notifications_were"];
        check(u(were["an_update_the_receiving_session_sent"]) == u(mine["total"]) &&
                  u(were["an_update_the_receiving_session_sent"]) +
                          u(were["an_update_another_session_of_this_run_sent"]) ==
                      u(t["unsigned_notifications"]) &&
                  u(were["from_beyond_the_range_of_the_receivers_region"]) > 0 &&
                  u(t["updates_from_beyond_the_range_of_the_receivers_region"]) ==
                      u(were["from_beyond_the_range_of_the_receivers_region"]),
              "stray: the summary says who sent each, and how many came from out of range");
        bool said = false;
        for (const auto& why : s["fail_reasons"])
            said = said || why.get<std::string>().find("came back to the session that sent them") !=
                               std::string::npos;
        bool described = s["events"].contains("notifications_outside_a_signed_bundle_without_a_verified_hmac");
        if (described) {
            const json& e = s["events"]["notifications_outside_a_signed_bundle_without_a_verified_hmac"]["events"][0];
            described = e["own_hmac"] == "none" && e["bytes"] == 165 && e.contains("sent_by_session") &&
                        e.contains("chunks_from_the_receiving_sessions_region") &&
                        e.contains("the_session_carried_that_player_before");
        }
        check(said && described && s["events"].contains("own_state_sent_by_the_receiving_session"),
              "stray: the failure says which session sent what came back, and the events describe each");
        dumpOnFailure(r, before);
    }
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
        check(u(s["own_state"]["sent_by_the_receiving_session"]["while_it_still_carried_the_player"]) > 0 &&
                  u(s["own_state"]["sent_by_the_receiving_session"]["total"]) == u(s["own_state"]["received"]),
              "own state: each is an update the receiving session itself sent");
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

void testFrameForms() {
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 1.2;
    {
        // The session does not advertise BUNDLE_SIGNED: every 51 and 52 is followed
        // by its own HMAC, as a member of a MESSAGE_BUNDLE.
        const int before = g_failures;
        FleetConfig plain = cfg;
        plain.signedBundles = false;
        const FakeRunResult r = runAgainstFake(plain, FakeRealtime::Options{}, stop, nullptr);
        const json& s = r.summary;
        const json& t = s["traffic"];
        check(r.exitCode == 0 && u(s["crossings"]["completed"]) > 0 && s["crossings"]["lost"] == 0,
              "frames: without BUNDLE_SIGNED the run passes");
        check(u(t["signed_bundles"]) == 0 && u(t["plain_bundles"]) > 0 &&
                  u(t["fleet_frames_in_signed_bundles"]) == 0 &&
                  u(t["fleet_frames_with_own_hmac"]) == r.fake.at("framesWithOwnHmac") &&
                  u(t["fleet_frames_with_own_hmac"]) >= 180 && u(t["fleet_frame_hmac_failures"]) == 0,
              "frames: every 51 and 52 came with its own HMAC, in a MESSAGE_BUNDLE, and verified");
        check(u(t["hmac_failures"]) == 0 && u(t["unsigned_notifications"]) == 0 &&
                  u(s["replication"]["latency"]["samples"]) > 0,
              "frames: each notification's own HMAC verified with the fleet token");
        const json* updates = typeRow(t, 130);
        const json* results = typeRow(t, 51);
        check(updates && results && u((*updates)["in_plain_bundles"]) > 0 &&
                  u((*updates)["in_signed_bundles"]) == 0 &&
                  u((*updates)["outside_signed_bundles_own_hmac_verified"]) ==
                      u((*updates)["in_plain_bundles"]) + u((*updates)["alone_in_a_datagram"]) &&
                  u((*updates)["outside_signed_bundles_no_hmac"]) == 0 &&
                  u((*results)["outside_signed_bundles_own_hmac_verified"]) >= 180 &&
                  u(t["plain_bundle_members"]) > 0 && !t["plain_bundle_members_by_type"].empty(),
              "frames: by type, what arrived in plain bundles and whether each member's own HMAC verified");
        bool members = s["events"].contains("plain_bundles");
        if (members) {
            const json& first = s["events"]["plain_bundles"]["events"][0];
            members = !first["members"].empty() && first["members"][0]["own_hmac"] == "verified" &&
                      first["members"][0].contains("type") && first["members"][0].contains("bytes");
        }
        check(members, "frames: a plain bundle's members are listed with their own HMAC's result");
        dumpOnFailure(r, before);
    }
    {
        // The same, each frame alone in its datagram.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.ignoreCapabilities = true;
        opt.framesAlone = true;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& t = r.summary["traffic"];
        check(r.exitCode == 0 && u(t["fleet_frames_with_own_hmac"]) >= 180 &&
                  u(t["fleet_frame_hmac_failures"]) == 0 && r.summary["crossings"]["lost"] == 0,
              "frames: a 51 or 52 alone in its datagram, with its own HMAC, is read");
        dumpOnFailure(r, before);
    }
    {
        // Every fifth frame's HMAC is wrong: it is dropped and counted, and the
        // carry it answered is sent again.
        const int before = g_failures;
        FakeRealtime::Options opt;
        opt.ignoreCapabilities = true;
        opt.corruptFrameHmacEvery = 5;
        FleetConfig quick = cfg;
        quick.carryResendMs = 60;
        const FakeRunResult r = runAgainstFake(quick, opt, stop, nullptr);
        const json& s = r.summary;
        check(u(s["traffic"]["fleet_frame_hmac_failures"]) > 10 &&
                  u(s["proof"]["hmac_failures"]) == u(s["traffic"]["fleet_frame_hmac_failures"]),
              "frames: a frame whose HMAC fails is counted and reported");
        check(r.exitCode == 0 && s["players"]["not_carried_at_end"] == 0 && u(s["carries"]["resent"]) > 0,
              "frames: a result dropped for its HMAC is asked for again, and everyone ends carried");
        dumpOnFailure(r, before);
    }
}

void testCarryRate() {
    std::atomic<bool> stop{false};
    // One session that may carry 8 of its 60 players: the other 52 are refused
    // (status 4) and asked for again and again, far more than 64 carries a second
    // if nothing held them back.
    FleetConfig cfg = base();
    cfg.servers = 1;
    cfg.players = 60;
    cfg.dryRunS = 1.5;
    cfg.carryGiveUpMs = 800;
    cfg.carryRetryMs = 100;
    cfg.carryResendMs = 50;
    cfg.drainS = 1;
    FakeRealtime::Options opt;
    opt.maxPlayers = 8;
    {
        // Paced at the default 60 a second: the server drops nothing.
        const int before = g_failures;
        const FakeRunResult r = runAgainstFake(cfg, opt, stop, nullptr);
        const json& s = r.summary;
        check(r.fake.at("carriesDropped") == 0 && u(s["carries"]["waited_for_the_carry_rate"]) > 0,
              "carry rate: paced under 64 a second, no FLEET_CARRY is dropped by the server");
        const double perSecond = static_cast<double>(u(s["carries"]["sent"])) / s["timing"]["total_s"].get<double>();
        check(perSecond < 64.0 + 32.0 / s["timing"]["total_s"].get<double>(),
              "carry rate: the session sent fewer than 64 carries a second, its burst aside");
        check(u(s["carries"]["by_status"]["4_session_full"]) > 0 && s["players"]["carried_at_end"] == 8 &&
                  r.exitCode == 1,
              "carry rate: status 4 is reported, the session carries its 8, and the run does not pass");
        dumpOnFailure(r, before);
    }
    {
        // Unpaced: the server drops carries unanswered, and the client sends them again.
        const int before = g_failures;
        FleetConfig fast = cfg;
        fast.carryRate = 5000;
        fast.carryBurst = 5000;
        const FakeRunResult r = runAgainstFake(fast, opt, stop, nullptr);
        const json& s = r.summary;
        check(r.fake.at("carriesDropped") > 0 && u(s["carries"]["resent"]) > 0,
              "carry rate: past 64 a second the server drops carries unanswered, and they are sent again");
        check(s["players"]["carried_at_end"] == 8, "carry rate: the 8 it may carry are carried all the same");
        dumpOnFailure(r, before);
    }
}

void testReleaseFirstPlan() {
    const int before = g_failures;
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.releaseFirst = true;
    const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, nullptr);
    const json& s = r.summary;
    const uint64_t crossed = u(s["crossings"]["completed"]);
    check(r.exitCode == 0 && crossed > 20 && s["crossings"]["lost"] == 0 &&
              s["proof"]["crossing_plan"] == "release-first",
          "release first: the plan passes, and the summary names it");
    check(u(s["traffic"]["releases_sent"]) == crossed + 180 && r.releaseRequests == crossed + 180,
          "release first: the old session sent FLEET_RELEASE at each crossing");
    check(u(s["released"]["old_session"]["expected"]) == 0 &&
              u(s["released"]["by_reason"]["1_carried_by_another_session"]) ==
                  u(s["released"]["reason_1_after_the_sessions_own_release"]) &&
              u(s["released"]["reason_1_for_a_held_player"]) == 0,
          "release first: no FLEET_RELEASED is due, and one that overtook the release is counted apart");
    check(u(s["api"]["calls_in_measured_phase"]) == 0 && u(s["api"]["calls_caused_by_crossing"]) == 0,
          "release first: still no API call at a crossing");
    dumpOnFailure(r, before);
}

void testComingBack() {
    std::atomic<bool> stop{false};
    {
        // The session's token stops being accepted. The refresh names the server
        // the session is on, the API authorizes it there again, and the carries
        // stand: capabilities only, and no new carry.
        const int before = g_failures;
        bool done = false;
        const FakeRunResult r = runAgainstFake(
            base(), FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!done && t > 0.4) {
                    done = true;
                    fake.deauthorize(3);
                }
            });
        const json& s = r.summary;
        const json& ses = s["sessions"];
        check(done && u(ses["moves_by_trigger"]["error_streak"]) == 1 && u(ses["moves_completed"]) == 1,
              "coming back: a session whose token is refused asks the API to authorize it again");
        check(r.apiRefreshSession == 1 && r.apiRefreshSessionWithoutServer == 0,
              "coming back: that refresh names the server the session is on");
        check(u(ses["came_back_to_the_same_server"]) == 1 && u(ses["came_back_to_another_server"]) == 0 &&
                  u(s["carries"]["attempts_by_cause"]["session_move"]) == 0 && r.capabilities >= 10,
              "coming back: on the same server it sends its capabilities again and no FLEET_CARRY");
        check(s["players"]["not_carried_at_end"] == 0 && s["crossings"]["lost"] == 0 &&
                  u(s["refused"]["errors_by_code"][0]["code"]) == 5,
              "coming back: the carries stood, and the refusals are reported by code");
        dumpOnFailure(r, before);
    }
    {
        // A 22 that verifies, sent to a session whose control is bound to a nonce
        // the server acknowledged: a replay, dropped.
        const int before = g_failures;
        bool done = false;
        const FakeRunResult r = runAgainstFake(
            base(), FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!done && t > 0.4) {
                    done = true;
                    fake.sendLegacyReconnect(2);
                }
            });
        const json& ses = r.summary["sessions"];
        check(done && u(ses["legacy_move_commands_dropped"]) == 1 && u(ses["moves"]) == 0 &&
                  r.exitCode == 0 && r.apiRefreshSession == 0,
              "coming back: a COMMAND_RECONNECT (22) after the bound control was acknowledged is dropped");
        dumpOnFailure(r, before);
    }
    {
        // Without bound control the same 22 is the server's word: the session
        // asks for a new server and carries every player again.
        const int before = g_failures;
        bool done = false;
        FleetConfig cfg = base();
        cfg.boundControl = false;
        const FakeRunResult r = runAgainstFake(
            cfg, FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!done && t > 0.4) {
                    done = true;
                    fake.tellSessionToMove(2, false, 0);
                }
            });
        const json& s = r.summary;
        check(done && u(s["sessions"]["moves_by_trigger"]["reconnect_command"]) == 1 &&
                  u(s["sessions"]["came_back_to_another_server"]) == 1 &&
                  u(s["sessions"]["bound_control_acknowledged"]) == 0 &&
                  r.apiRefreshSessionWithoutServer == 1,
              "coming back: an unbound session takes COMMAND_RECONNECT (22) and asks for a new server");
        check(u(s["carries"]["carried_by_cause"]["session_move"]) > 0 &&
                  s["players"]["not_carried_at_end"] == 0,
              "coming back: on another server it carries every player again");
        dumpOnFailure(r, before);
    }
}

void testDeliveriesAndPlainCommunications() {
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.dryRunS = 1.2;
    {
        const int before = g_failures;
        bool done = false;
        const FakeRunResult r = runAgainstFake(
            cfg, FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!done && t > 0.4) {
                    done = true;
                    fake.injectText(1, /*insideDelivery*/ true);
                }
            });
        const json& d = r.summary["traffic"]["deliveries"];
        int sessionsWithOne = 0;
        uint64_t listed = 0, plain = 0;
        for (const auto& p : r.summary["bytes"]["per_session"]) {
            if (u(p["deliveries"]) == 1) ++sessionsWithOne;
            listed += u(p["delivery_players_listed"]);
            plain += u(p["plain_communications"]);
        }
        check(done && r.exitCode == 0 && u(d["frames"]) == 1 && u(d["players_listed"]) == 1 &&
                  u(d["by_type"]["137"]) == 1 && u(d["listing_a_player_not_carried"]) == 0,
              "deliveries: a FLEET_DELIVERY is counted, with the players it lists and what it wraps");
        check(sessionsWithOne == 1 && listed == 1 && plain == 0 &&
                  u(r.summary["traffic"]["plain_communications"]) == 0,
              "deliveries: counted for the session that received it; a text in a 53 is no defect");
        dumpOnFailure(r, before);
    }
    {
        const int before = g_failures;
        bool done = false;
        const FakeRunResult r = runAgainstFake(
            cfg, FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!done && t > 0.4) {
                    done = true;
                    fake.injectText(1, /*insideDelivery*/ false);
                }
            });
        const json& s = r.summary;
        bool said = false;
        for (const auto& why : s["fail_reasons"])
            said = said || why.get<std::string>().find("outside a FLEET_DELIVERY") != std::string::npos;
        int sessionsWithOne = 0;
        for (const auto& p : s["bytes"]["per_session"]) {
            if (u(p["plain_communications"]) == 1) ++sessionsWithOne;
        }
        check(done && u(s["traffic"]["plain_communications"]) == 1 && sessionsWithOne == 1 &&
                  u(s["proof"]["plain_communications"]) == 1,
              "plain communication: a text that arrives outside a FLEET_DELIVERY is counted");
        check(r.exitCode == 1 && said, "plain communication: it fails the run, and the failure says why");
        dumpOnFailure(r, before);
    }
}

void testOrgTokenOwnsItsSessions() {
    FakeRealtime fake(4242, FakeRealtime::Options{});
    ApiCounters counters;
    FleetApi opener(fake.transport(), FakeRealtime::ORG_TOKEN, 4242, counters);
    FleetApi other(fake.transport(), FakeRealtime::OTHER_ORG_TOKEN, 4242, counters);
    const FleetSessionInfo session = opener.openFleetSession(ApiCause::Setup);
    std::string code;
    try {
        other.refreshFleetSession(session.gameTokenId, session.server, ApiCause::SessionKeep);
    } catch (const lt::GraphQLError& e) {
        code = e.code();
    }
    check(code == "NOT_FOUND", "org token: another org token's refresh is answered NOT_FOUND");
    check(!other.closeFleetSession(session.gameTokenId, ApiCause::Teardown) && fake.openSessions() == 1,
          "org token: another org token's close is answered false, and the session stays");
    const FleetSessionInfo kept =
        opener.refreshFleetSession(session.gameTokenId, session.server, ApiCause::SessionKeep);
    check(kept.token == session.token && kept.gameTokenId == session.gameTokenId &&
              kept.server == session.server,
          "org token: the token that opened it refreshes it, on the same server");
    const FleetSessionInfo moved =
        opener.refreshFleetSession(session.gameTokenId, ServerAddr{}, ApiCause::SessionMove);
    check(!(moved.server == session.server), "org token: with no current server named, the answer names a new one");
    check(opener.closeFleetSession(session.gameTokenId, ApiCause::Teardown) && fake.openSessions() == 0,
          "org token: the token that opened it closes it");
    check(fake.counters().apiRefusedOtherOrgToken == 2, "org token: both refusals were for the other token");
}

void testDeletePlayers() {
    const int before = g_failures;
    char dirTemplate[] = "/tmp/fleet-delete-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    if (!dir) {
        check(false, "delete: a scratch directory");
        return;
    }
    const std::string roster = std::string(dir) + "/players-roster.json";
    std::atomic<bool> stop{false};
    FleetConfig cfg = base();
    cfg.servers = 4;
    cfg.players = 60;
    cfg.dryRunS = 0.8;
    cfg.rosterFile = roster;
    FakeRealtime fake(4242, FakeRealtime::Options{});
    fake.start();
    const FakeRunResult run = runAgainstFake(fake, cfg, stop, nullptr);
    check(run.exitCode == 0 && fake.players() == 60, "delete: a run leaves its 60 players on the tier");

    cfg.deletePlayers = true;
    json summary;
    int code = runDeletePlayers(configForFake(cfg, fake), fakeDeps(fake), stop, summary, nullptr);
    check(code == 0 && summary["pass"] == true && u(summary["deleted"]) == 60 &&
              u(summary["not_found"]) == rulesPlayerIds().size() && u(summary["failed"]) == 0,
          "delete: --delete-players deletes fleet-load-0 to fleet-load-59, and finds no rule-check players");
    check(fake.players() == 0 && fake.counters().apiDelete == 60 + rulesPlayerIds().size() &&
              u(summary["api"]["deleteManagedPlayer"]) == 60 + rulesPlayerIds().size(),
          "delete: the tier holds none of them afterwards, and each deletion was one API call");
    struct stat st {};
    check(summary["roster_file_removed"] == true && stat(roster.c_str(), &st) != 0,
          "delete: the roster of their tokens is removed with them");

    code = runDeletePlayers(configForFake(cfg, fake), fakeDeps(fake), stop, summary, nullptr);
    check(code == 0 && u(summary["deleted"]) == 0 && u(summary["not_found"]) == 66,
          "delete: a second time there is nothing to delete, and that is not a failure");

    // A rerun signs the same external ids in again, as new players.
    cfg.deletePlayers = false;
    cfg.rosterFile.clear();
    const FakeRunResult again = runAgainstFake(fake, cfg, stop, nullptr);
    check(again.exitCode == 0 && fake.players() == 60 && again.apiMint == 60,
          "delete: a later run signs the same external ids in again");
    fake.stop();
    rmdir(dir);
    dumpOnFailure(again, before);
}

void testRulesCheck() {
    std::atomic<bool> stop{false};
    auto run = [&](FakeRealtime::Options options, FleetConfig cfg, json& summary) {
        options.idleTimeoutMs = 1500;
        FakeRealtime fake(4242, options);
        fake.start();
        cfg = configForFake(cfg, fake);
        cfg.rulesCheck = true;
        cfg.rulesIdleTimeoutS = 1.5;
        cfg.rulesStepS = 0.4;
        const int code = runRulesCheck(cfg, fakeDeps(fake), stop, summary, nullptr);
        const bool closed = fake.openSessions() == 0;
        fake.stop();
        return std::make_pair(code, closed);
    };
    auto result = [](const json& summary, const std::string& name) {
        for (const auto& c : summary["checks"]) {
            if (c["name"].get<std::string>().find(name) != std::string::npos)
                return c["result"].get<std::string>();
        }
        return std::string("missing");
    };
    {
        json summary;
        const auto [code, closed] = run(FakeRealtime::Options{}, base(), summary);
        check(code == 0 && summary["pass"] == true && closed, "rules: every check passes, and both sessions are closed");
        for (const char* name : {"carry", "the blocked player's text", "the blocking player's text",
                                 "state still passes", "only in FLEET_DELIVERY", "under 18's voice",
                                 "another player's actor", "does not carry", "under its own id",
                                 "largest FLEET_DELIVERY", "idle release"}) {
            check(result(summary, name) == "PASS", "rules: each rule has a check that prints PASS");
        }
        bool observed = true;
        for (const auto& c : summary["checks"]) observed = observed && !c["observed"].get<std::string>().empty();
        check(observed && summary["checks"].size() == 11, "rules: each check says what it observed");
        check(u(summary["api"]["calls_by_kind"]["blockPlayer"]) == 1 &&
                  u(summary["api"]["calls_by_kind"]["mintManagedPlayerToken"]) == 6 &&
                  u(summary["api"]["calls_by_kind"]["openFleetSession"]) == 2 &&
                  u(summary["api"]["calls_by_kind"]["closeFleetSession"]) == 2,
              "rules: two sessions, six players, one block");
    }
    {
        // A session that does not advertise signed bundles reads every frame with its own HMAC.
        json summary;
        FleetConfig cfg = base();
        cfg.signedBundles = false;
        cfg.boundControl = false;
        const auto [code, closed] = run(FakeRealtime::Options{}, cfg, summary);
        check(code == 0 && closed && result(summary, "largest FLEET_DELIVERY") == "PASS",
              "rules: the checks pass without BUNDLE_SIGNED and BOUND_CONTROL as well");
    }
    {
        // The stand-in sends a text plain as well as in its 53: the check catches it.
        json summary;
        FakeRealtime::Options broken;
        broken.plainCommunicationBug = true;
        const auto [code, closed] = run(broken, base(), summary);
        check(code == 1 && closed && result(summary, "only in FLEET_DELIVERY") == "FAIL" &&
                  result(summary, "the blocked player's text") == "PASS",
              "rules: a communication outside a FLEET_DELIVERY prints FAIL, and the run does not pass");
    }
    {
        // With no idle timeout on the app, the idle check fails and says so.
        json summary;
        FakeRealtime::Options options;
        FakeRealtime fake(4242, options); // idle release off
        fake.start();
        FleetConfig cfg = configForFake(base(), fake);
        cfg.rulesIdleTimeoutS = 0.5;
        cfg.rulesStepS = 0.3;
        const int code = runRulesCheck(cfg, fakeDeps(fake), stop, summary, nullptr);
        fake.stop();
        check(code == 1 && result(summary, "idle release") == "FAIL" && result(summary, "carry") == "PASS",
              "rules: when the app's idle timeout is not what was said, the idle check prints FAIL");
    }
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
        check(r.exitCode == 1 && u(s["carries"]["by_status"]["2_other_organization"]) >= 1 &&
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
        check(r.exitCode == 1 && u(s["carries"]["by_status"]["4_session_full"]) > 0 &&
                  s["players"]["not_carried_at_end"].get<int>() > 0,
              "refusal: status 4 is counted, and players left uncarried fail the run");
        check(u(s["carries"]["gave_up_after_5s"]) > 0 && u(s["carries"]["started_again_after_giving_up"]) > 0,
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
        check(r.exitCode == 1 && u(s["carries"]["by_status"]["3_proof_time_or_replay"]) > 0 &&
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
    testAroundATokenRefresh();
    testWhoSentTheState();
    testOwnState();
    testFrameForms();
    testCarryRate();
    testReleaseFirstPlan();
    testComingBack();
    testDeliveriesAndPlainCommunications();
    testOrgTokenOwnsItsSessions();
    testDeletePlayers();
    testRulesCheck();
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
