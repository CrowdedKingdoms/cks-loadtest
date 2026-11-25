#include "fleet/CrossRate.hpp"
#include "fleet/DryRun.hpp"
#include "fleet/FleetWire.hpp"
#include "fleet/Grid.hpp"

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

// 64 octets each.
const char* FLEET_TOKEN = "fleet0123456789fleet0123456789fleet0123456789fleet0123456789abcd";
const char* PLAYER_TOKEN = "player012345678player012345678player012345678player012345678wxyz";
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
    const auto* fleet = reinterpret_cast<const uint8_t*>(FLEET_TOKEN);
    const auto* player = reinterpret_cast<const uint8_t*>(PLAYER_TOKEN);

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
    pose.pitch = markerCode(3, 0xBEEF, true);
    pose.yaw = 123456789.0;
    const bool built =
        wire::finalizeActorUpdateFmt(update, wire::PoseFormat::UE5, 1, -2, 0, pose, 4, fleet);
    const wire::InboundView view{update, wire::ACTOR_UPDATE_SIZE};
    check(built && wire::readI64(update + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET) == PLAYER_ID &&
              wire::verifyNotification(view, fleet) && !wire::verifyNotification(view, player),
          "wire: a carried player's update names the player and is signed with the fleet token");
    const auto payload = notificationPayload(view);
    const auto marker = payload ? readMarker(payload->data, payload->len) : std::nullopt;
    check(marker && marker->session == 3 && marker->runTag == 0xBEEF && marker->sampled &&
              marker->sendUs == 123456789,
          "wire: the marker in the update's payload reads back");

    const uint8_t result[18] = {FLEET_CARRY_RESULT, 2, 0x1a, 0x71, 0x18, 2, 0, 0, 0,
                                0x39, 0x30, 0, 0, 0, 0, 0, 0, 4};
    const auto r = parseCarryResult(wire::InboundView{result, sizeof(result)}, APP);
    check(r && r->playerGameTokenId == PLAYER_ID && r->userId == 12345 && r->status == 4,
          "wire: FLEET_CARRY_RESULT parses");
    const uint8_t released[10] = {FLEET_RELEASED, 2, 0x1a, 0x71, 0x18, 2, 0, 0, 0, 1};
    const auto rel = parseReleased(wire::InboundView{released, sizeof(released)}, APP);
    check(rel && rel->playerGameTokenId == PLAYER_ID && rel->reason == 1,
          "wire: FLEET_RELEASED parses");
    uint8_t delivery[2 + 16 + 4] = {FLEET_DELIVERY, 2};
    wire::writeI64(delivery + 2, PLAYER_ID);
    wire::writeI64(delivery + 10, PLAYER_ID + 1);
    const uint8_t text[4] = {137, 1, 2, 3};
    std::memcpy(delivery + 18, text, sizeof(text));
    const auto d = parseDelivery(wire::InboundView{delivery, sizeof(delivery)}, APP);
    check(d && d->count == 2 && d->player(1) == PLAYER_ID + 1 && d->notification.type() == 137 &&
              d->notification.len == 4,
          "wire: FLEET_DELIVERY parses");
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
                  s["api"]["calls"].get<uint64_t>() == r.apiCalls &&
                  r.apiCalls == 9u + 180u + 9u,
              "run: the API saw only the setup and the teardown, and no call for a crossing");
        check(s["own_state"]["received"].get<uint64_t>() == 0 && r.badHmac == 0,
              "run: no own state came back, and every datagram verified");
    }
    {
        FakeRealtime::Options faults;
        faults.mintBusyEvery = 7;
        faults.dropToClient = 0.02;
        faults.dropToServer = 0.02;
        bool moved = false, ended = false;
        const FakeRunResult r = runAgainstFake(
            cfg, faults, stop, nullptr, [&](double t, FakeRealtime& fake) {
                if (!moved && t > 0.4) {
                    moved = true;
                    fake.tellSessionToMove(4, false, 0);
                }
                if (!ended && t > 0.8) {
                    ended = true;
                    fake.endSignIn(3);
                }
            });
        const auto& s = r.summary;
        check(r.exitCode == 0, "faults: the run still passes");
        check(s["sessions"]["moves_completed"].get<uint64_t>() >= 1 &&
                  r.apiRefreshSession >= 1 && r.apiRefreshSessionWithoutServer == 0,
              "faults: a session told to move asked the API and reconnected");
        check(s["players"]["signed_in_again"].get<uint64_t>() >= 1 &&
                  s["players"]["platform_busy"].get<uint64_t>() >= 1,
              "faults: an ended sign-in was signed in again, and PLATFORM_BUSY was retried");
        check(s["api"]["calls_caused_by_crossing"].get<uint64_t>() == 0,
              "faults: still no API call for a crossing");
    }
    {
        FakeRealtime::Options broken;
        broken.ownStateBug = true;
        const FakeRunResult r = runAgainstFake(cfg, broken, stop, nullptr);
        check(r.exitCode == 1 && r.summary["own_state"]["received"].get<uint64_t>() > 0,
              "detect: own state sent back unasked fails the run");
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
