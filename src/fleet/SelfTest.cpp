#include "fleet/CrossRate.hpp"
#include "fleet/DryRun.hpp"
#include "fleet/FleetWire.hpp"
#include "fleet/Grid.hpp"
#include "fleet/Modes.hpp"

#include <cstdio>
#include <cstring>
#include <string>

/// `ck-fleet-load --self-test`: checks a build can run before it is pointed at
/// a tier. Known answers for the wire formats (computed outside this program),
/// the grid and the controller, then three short runs against the in-process
/// stand-in: a clean one, one with faults injected, and one where the stand-in
/// breaks a rule the client must catch.
namespace lt::fleet {

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
    std::fflush(stdout);
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

/// A fixture with the shape of a token: a 15-character part four times, then 4
/// more, 64 octets in all. Built here so that no token-shaped literal sits in
/// the source for a secret scanner to report. tests/fleet_vectors.hpp holds the
/// same two strings in full.
std::string fixture(const char* part15, const char* tail4) {
    std::string s;
    for (int i = 0; i < 4; ++i) s += part15;
    return s + tail4;
}
const std::string kFleetFixture = fixture("fleet0123456789", "abcd");
const std::string kPlayerFixture = fixture("player012345678", "wxyz");
constexpr int64_t APP = 7;
constexpr int64_t FLEET_ID = 7000000001LL;
constexpr int64_t PLAYER_ID = 9000000002LL;
constexpr int64_t WHEN_MS = 1795000000123LL;

// Computed with Python's hmac and struct modules from the contract's text.
const char* PROOF_HEX = "98ddf5f217ddacdf9bb4145b0572eddbecd4c951f4867d3fbb404db2a0caa715";
const char* CARRY_HEX =
    "3107000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "000000000000000000000000000000000000000000000000021a7118020000007b5e56eea101000098ddf5f2"
    "17ddacdf9bb4145b0572eddbecd4c951f4867d3fbb404db2a0caa7154f1f8158ce46cbb97998dc051c526b06"
    "7844f46c8a6e5952206d1c74ed8abdd801863ba10100000009";

void wireChecks() {
    const auto* fleet = reinterpret_cast<const uint8_t*>(kFleetFixture.data());
    const auto* player = reinterpret_cast<const uint8_t*>(kPlayerFixture.data());
    check(kFleetFixture.size() == 64 && kPlayerFixture.size() == 64,
          "wire: the two fixture tokens are 64 octets");

    uint8_t proof[hmac::TAG_SIZE];
    check(carryProof(player, FLEET_ID, PLAYER_ID, WHEN_MS, proof) &&
              toHex(proof, sizeof(proof)) == PROOF_HEX,
          "wire: the carry proof matches the known answer");

    uint8_t carry[FLEET_CARRY_SIZE];
    const size_t n = buildCarry(carry, APP, FLEET_ID, PLAYER_ID, WHEN_MS, player, 9, fleet);
    check(n == 157 && toHex(carry, n) == CARRY_HEX, "wire: FLEET_CARRY matches the known answer");

    // A carried player's update: the tail names the player, the HMAC is the fleet token's.
    uint8_t update[wire::ACTOR_UPDATE_MAX_SIZE];
    wire::initActorUpdateTemplateFmt(update, wire::PoseFormat::UE5,
                                     "0123456789ABCDEF0123456789ABCDEF", APP, PLAYER_ID, 8,
                                     wire::DecayRate::EXPONENTIAL);
    wire::ActorPose pose;
    pose.pitch = markerCode(3, 0x2EF, true, 2999);
    pose.yaw = 123456789.0;
    const bool built =
        wire::finalizeActorUpdateFmt(update, wire::PoseFormat::UE5, 1, -2, 0, pose, 4, fleet);
    const wire::InboundView view{update, wire::ACTOR_UPDATE_SIZE};
    check(built && wire::readI64(update + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET) == PLAYER_ID &&
              wire::verifyNotification(view, fleet) && !wire::verifyNotification(view, player),
          "wire: a carried player's update names the player and is signed with the fleet token");
    const auto payload = notificationPayload(view);
    const auto marker = payload ? readMarker(payload->data, payload->len) : std::nullopt;
    check(marker && marker->session == 3 && marker->runTag == 0x2EF && marker->sampled &&
              marker->player == 2999 && marker->sendUs == 123456789,
          "wire: the marker in the update's payload reads back (sending session, player, time)");

    // The contract's golden proof (fleet id 9001, player id 4242, app 7).
    // The contract's two fixture strings, each 64 octets: a 12- or 13-character
    // head, then the digits, the lower case and as much of the upper case as fits.
    const std::string alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const std::string goldenFleet = "fleet-" + std::string("token-") + alphabet.substr(0, 52);
    const std::string goldenPlayer = "player-" + std::string("token-") + alphabet.substr(0, 51);
    uint8_t golden[hmac::TAG_SIZE];
    check(goldenFleet.size() == 64 && goldenPlayer.size() == 64 &&
              carryProof(reinterpret_cast<const uint8_t*>(goldenPlayer.data()), 9001, 4242,
                         1795000000000LL, golden) &&
              toHex(golden, sizeof(golden)) ==
                  "5602abb9300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a",
          "wire: the contract's golden carry proof");

    // The three server frames, in both forms a session must read.
    const uint8_t result[CARRY_RESULT_SIZE] = {FLEET_CARRY_RESULT, 2, 0x1a, 0x71, 0x18, 2, 0, 0, 0,
                                               0x39, 0x30, 0, 0, 0, 0, 0, 0, 4};
    const auto r = parseCarryResult(wire::InboundView{result, sizeof(result)});
    check(r && r->playerGameTokenId == PLAYER_ID && r->userId == 12345 && r->status == 4,
          "wire: FLEET_CARRY_RESULT parses (18 bytes)");
    const uint8_t released[RELEASED_SIZE] = {FLEET_RELEASED, 2, 0x1a, 0x71, 0x18, 2, 0, 0, 0, 1};
    const auto rel = parseReleased(wire::InboundView{released, sizeof(released)});
    check(rel && rel->playerGameTokenId == PLAYER_ID && rel->reason == 1,
          "wire: FLEET_RELEASED parses (10 bytes)");
    uint8_t delivery[2 + 16 + 4] = {FLEET_DELIVERY, 2};
    wire::writeI64(delivery + 2, PLAYER_ID);
    wire::writeI64(delivery + 10, PLAYER_ID + 1);
    const uint8_t text[4] = {137, 1, 2, 3};
    std::memcpy(delivery + 18, text, sizeof(text));
    const auto d = parseDelivery(wire::InboundView{delivery, sizeof(delivery)});
    check(d && d->count == 2 && d->player(1) == PLAYER_ID + 1 && d->notification.type() == 137 &&
              d->notification.len == 4,
          "wire: FLEET_DELIVERY parses");

    // Outside a signed bundle a frame is followed by its own HMAC.
    uint8_t withHmac[CARRY_RESULT_SIZE + hmac::TAG_SIZE];
    std::memcpy(withHmac, result, sizeof(result));
    hmac::spatialSign(withHmac, sizeof(result), fleet, withHmac + sizeof(result));
    FleetFrame f = readFleetFrame(wire::InboundView{withHmac, sizeof(withHmac)}, false, fleet);
    check(sizeof(withHmac) == 50 && f.auth == FrameAuth::OwnHmac && f.frame.len == CARRY_RESULT_SIZE &&
              parseCarryResult(f.frame),
          "wire: a 51 with its own HMAC is 50 bytes and verifies with the fleet token");
    withHmac[20] ^= 1;
    check(readFleetFrame(wire::InboundView{withHmac, sizeof(withHmac)}, false, fleet).auth ==
                  FrameAuth::Failed &&
              readFleetFrame(wire::InboundView{result, sizeof(result)}, false, fleet).auth ==
                  FrameAuth::Failed,
          "wire: a frame whose HMAC is wrong or missing is refused");
    check(readFleetFrame(wire::InboundView{result, sizeof(result)}, true, fleet).auth ==
              FrameAuth::SignedBundle,
          "wire: inside a verified signed bundle a frame has no HMAC of its own");
    check(std::string(carryStatusName(3)) == "3_proof_time_or_replay" &&
              std::string(releaseReasonName(5)) == "5_idle",
          "wire: statuses and reasons have the contract's names");
}

void worldChecks() {
    const Grid g = Grid::forServers(100, 16);
    check(g.cols == 10 && g.rows == 10 && g.neighbours(0).size() == 3 &&
              g.neighbours(55).size() == 8,
          "world: 100 servers are a 10 x 10 grid");
    RegionTracker t;
    t.update(g, 15.5, 8.0);
    int crossings = 0;
    for (double x = 15.5; x < 17.0; x += 0.05) {
        if (t.update(g, x, 8.0) >= 0) ++crossings;
    }
    check(crossings == 1 && t.region == 1, "world: a step over a border is one crossing");
    const double v = speedForCrossRate(g, 3000, 75.0);
    check(v > 0.2 && v < 0.6 && std::fabs(expectedCrossRate(g, 3000, v) - 75.0) < 1e-6,
          "world: the speed for 75 crossings a second among 3,000 players");
}

void runChecks() {
    std::atomic<bool> stop{false};
    FleetConfig cfg;
    cfg.servers = 9;
    cfg.players = 180;
    cfg.crossRate = 30;
    cfg.dryRunS = 2.0;
    cfg.signinRate = 5000;
    cfg.sessionSettleMs = 0;
    cfg.threads = 2;
    cfg.apiThreads = 4;
    cfg.drainS = 3;

    {
        const FakeRunResult r = runAgainstFake(cfg, FakeRealtime::Options{}, stop, nullptr);
        const auto& s = r.summary;
        check(r.exitCode == 0, "run: a clean run against the stand-in passes");
        check(s["crossings"]["completed"].get<uint64_t>() > 10 &&
                  s["crossings"]["lost"].get<int>() == 0,
              "run: players crossed and none was lost");
        check(s["api"]["calls_caused_by_crossing"].get<uint64_t>() == 0 &&
                  s["api"]["calls_in_measured_phase"].get<uint64_t>() == 0 &&
                  s["api"]["calls"].get<uint64_t>() == r.apiCalls && r.apiCalls == 9u + 180u + 9u,
              "run: the API saw only the setup and the teardown, and nothing while players crossed");
        check(s["own_state"]["received"].get<uint64_t>() == 0 && r.badHmac == 0 &&
                  s["traffic"]["plain_communications"].get<uint64_t>() == 0,
              "run: no own state came back, and every datagram verified");
        check(r.fake.at("ownIdRefused") == 0 && r.fake.at("carriesDropped") == 0,
              "run: nothing but 29, 49, 50 and 26 under a session's own id, and no carry past the rate");
        check(!s["usage"]["per_session_minute"].empty() &&
                  s["proof"]["sessions_opened"].get<uint64_t>() == 9,
              "run: the summary has the session-minutes and the proof's numbers");
    }
    {
        // Lost datagrams each way, a busy API at sign-in, and frames that each
        // carry their own HMAC because the session's capabilities were not taken.
        FakeRealtime::Options rough;
        rough.mintBusyEvery = 7;
        rough.dropToClient = 0.03;
        rough.dropToServer = 0.03;
        rough.ignoreCapabilities = true;
        FleetConfig quick = cfg;
        quick.carryResendMs = 80;
        const FakeRunResult r = runAgainstFake(quick, rough, stop, nullptr);
        const auto& s = r.summary;
        check(r.exitCode == 0 && s["crossings"]["lost"].get<int>() == 0,
              "rough: with datagrams lost each way, nobody is lost");
        check(s["carries"]["resent"].get<uint64_t>() > 0 &&
                  s["players"]["platform_busy"].get<uint64_t>() > 0,
              "rough: an unanswered carry was sent again, and PLATFORM_BUSY was waited out");
        check(s["traffic"]["fleet_frames_with_own_hmac"].get<uint64_t>() > 0 &&
                  s["traffic"]["fleet_frame_hmac_failures"].get<uint64_t>() == 0,
              "rough: frames with their own HMAC were read and verified");
    }
    {
        // A session told to move and a sign-in that ends: both are handled, and
        // both are API calls in the measured phase. The move is a refresh a
        // realtime server asked for and is counted apart; the sign-in again is
        // what fails the run.
        bool moved = false, ended = false;
        const FakeRunResult r = runAgainstFake(
            cfg, FakeRealtime::Options{}, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!moved && t > 0.4) {
                    moved = true;
                    fake.tellSessionToMove(4, false, 0);
                }
                if (!ended && t > 1.2) {
                    ended = true;
                    for (int n = 0; n < 180; ++n) {
                        if (fake.carrierOf(n) >= 0) {
                            fake.endSignIn(n);
                            break;
                        }
                    }
                }
            });
        const auto& s = r.summary;
        check(s["sessions"]["came_back_to_another_server"].get<uint64_t>() == 1 &&
                  r.apiRefreshSession == 1 && r.apiRefreshSessionWithoutServer == 1 &&
                  s["carries"]["carried_by_cause"]["session_move"].get<uint64_t>() > 0,
              "faults: a session told to move asked the API for a new server and carried again");
        check(s["players"]["signed_in_again"].get<uint64_t>() == 1 &&
                  s["players"]["not_carried_at_end"].get<int>() == 0,
              "faults: an ended sign-in was signed in again, and everyone was carried at the end");
        const uint64_t signInsAgain =
            s["api"]["calls_in_measured_phase_by_cause"]["sign_in_again"].get<uint64_t>();
        check(s["api"]["calls_in_measured_phase_refreshes_asked_by_a_server"].get<uint64_t>() == 1 &&
                  s["api"]["calls_in_measured_phase_refreshes_due"].get<uint64_t>() == 0 &&
                  s["api"]["calls_in_measured_phase_other"].get<uint64_t>() == signInsAgain &&
                  signInsAgain >= 1,
              "faults: the move is counted as a refresh a realtime server asked for, apart from the sign-in again");
        check(r.exitCode == 1 && s["fail_reasons"].size() == 1 &&
                  s["fail_reasons"][0].get<std::string>().find("sign_in_again") != std::string::npos &&
                  s["fail_reasons"][0].get<std::string>().find("session_move") == std::string::npos &&
                  s["api"]["calls_caused_by_crossing"].get<uint64_t>() == 0,
              "faults: the sign-in again fails the run, the move does not, and none was for a crossing");
    }
    {
        FakeRealtime::Options broken;
        broken.ownStateBug = true;
        const FakeRunResult r = runAgainstFake(cfg, broken, stop, nullptr);
        const auto& own = r.summary["own_state"];
        check(r.exitCode == 1 && own["received"].get<uint64_t>() > 0 &&
                  own["sent_by_the_receiving_session"]["total"] == own["received"],
              "detect: an update that came back to the session that sent it fails the run");
    }
    {
        // A copy with no HMAC, alone in a datagram, at its sender or at a session
        // out of range: counted by type, by arrival and by who sent it.
        FakeRealtime::Options stray;
        stray.strayUnsignedEvery = 40;
        const FakeRunResult r = runAgainstFake(cfg, stray, stop, nullptr);
        const auto& t = r.summary["traffic"];
        check(t["unsigned_notifications"].get<uint64_t>() == r.fake.at("strayUnsigned") &&
                  t["unsigned_notifications"].get<uint64_t>() > 0 &&
                  t["unsigned_notifications_were"]["an_update_the_receiving_session_sent"]
                          .get<uint64_t>() == r.fake.at("strayUnsignedToSender"),
              "detect: a notification with no HMAC outside a signed bundle is counted, with who sent it");
    }
    {
        // A token refresh whose new token the realtime side learns late: status 1
        // is waited out and the carry sent again.
        FakeRealtime::Options late;
        late.playerTokenTtlMs = 4000;
        late.newTokenLagMs = 300;
        FleetConfig refresh = cfg;
        refresh.dryRunS = 3.0;
        refresh.tokenRefreshLeadS = 2;
        const FakeRunResult r = runAgainstFake(refresh, late, stop, nullptr);
        const auto& c = r.summary["carries"];
        check(r.exitCode == 0 && c["retried_after_refresh"].get<uint64_t>() > 0 &&
                  c["retried_after_refresh"] == c["by_status"]["1_no_live_token"] &&
                  r.summary["players"]["signed_in_again"].get<uint64_t>() == 0,
              "refresh: status 1 after a token refresh is sent again, and nobody signs in again");
    }
    {
        // The per-player rule checks, against the stand-in.
        FakeRealtime::Options options;
        options.idleTimeoutMs = 2500;
        FakeRealtime fake(4242, options);
        fake.start();
        FleetConfig rules = configForFake(cfg, fake);
        rules.rulesCheck = true;
        rules.rulesIdleTimeoutS = 2.5;
        rules.rulesStepS = 0.4;
        nlohmann::json summary;
        const int code = runRulesCheck(rules, fakeDeps(fake), stop, summary, nullptr);
        fake.stop();
        int passed = 0, failed = 0;
        for (const auto& c : summary["checks"]) (c["result"] == "PASS" ? passed : failed)++;
        check(code == 0 && passed >= 10 && failed == 0,
              "rules: every per-player rule check passes against the stand-in");
    }
}

} // namespace

int runSelfTest() {
    g_failures = 0;
    wireChecks();
    worldChecks();
    runChecks();
    if (g_failures) {
        std::printf("%d self-test check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all self-test checks passed\n");
    return 0;
}

} // namespace lt::fleet
