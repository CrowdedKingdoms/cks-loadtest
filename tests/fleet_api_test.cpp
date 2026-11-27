// Unit tests for the Application API calls of the fleet load client
// (src/fleet/FleetApi.hpp), its roster file and its options. No network: a
// scripted transport records what the client would send and plays the answers.

#include "fleet/FleetApi.hpp"
#include "fleet/FleetConfig.hpp"
#include "fleet/Roster.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace lt;
using namespace lt::fleet;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

// Not real credentials: placeholders with the right shape.
const std::string kOrgBearer = "cko_unit_test_placeholder";
const std::string kSessionKey(64, 'F');
const std::string kPlayerKey(64, 'P');
const std::string kPlayerKey2(64, 'Q');

struct Sent {
    std::string query;
    nlohmann::json variables;
    std::string bearer;
};

/// Plays a script: each entry either answers or throws.
class ScriptedTransport final : public ApiTransport {
public:
    using Step = std::function<nlohmann::json()>;
    std::vector<Sent>* sent;
    std::deque<Step> script;
    std::vector<std::string> refreshBearers;
    int refreshHttpCalls = 1;
    std::deque<std::function<PlayerToken()>> refreshScript;

    explicit ScriptedTransport(std::vector<Sent>* log) : sent(log) {}

    nlohmann::json request(const std::string& query, const nlohmann::json& variables,
                           const std::string& bearer) override {
        sent->push_back(Sent{query, variables, bearer});
        if (script.empty()) throw GraphQLError("the script has no more answers", "", false);
        Step step = std::move(script.front());
        script.pop_front();
        return step();
    }
    PlayerToken refreshAppToken(const std::string& playerToken, int& httpCalls) override {
        refreshBearers.push_back(playerToken);
        httpCalls = refreshHttpCalls;
        if (refreshScript.empty()) throw GraphQLError("no refresh scripted", "", false);
        auto step = std::move(refreshScript.front());
        refreshScript.pop_front();
        return step();
    }
};

struct Rig {
    std::vector<Sent> sent;
    ApiCounters counters;
    std::vector<int> sleeps;
    ScriptedTransport* transport = nullptr;
    std::unique_ptr<FleetApi> api;
    Rig() {
        auto t = std::make_unique<ScriptedTransport>(&sent);
        transport = t.get();
        api = std::make_unique<FleetApi>(std::move(t), kOrgBearer, 7, counters, nullptr,
                                         [this](int ms) { sleeps.push_back(ms); });
    }
    uint64_t calls(ApiKind k, ApiCause c) const {
        return counters.calls[static_cast<size_t>(k)][static_cast<size_t>(c)].load();
    }
};

nlohmann::json sessionAnswer(const char* field) {
    return {{field,
             {{"token", kSessionKey},
              {"gameTokenId", "7000000001"},
              {"appId", "7"},
              {"expiresAt", "2026-11-26T20:00:00.000Z"},
              {"maxPlayers", 256},
              {"server", {{"ip4", "203.0.113.9"}, {"clientPort", 7777}}}}}};
}

void testOpen() {
    Rig rig;
    rig.transport->script.push_back([] { return sessionAnswer("openFleetSession"); });
    const FleetSessionInfo s = rig.api->openFleetSession(ApiCause::Setup);

    const Sent& q = rig.sent.at(0);
    check(q.query.find("mutation($i: OpenFleetSessionInput!)") != std::string::npos &&
              q.query.find("openFleetSession(input: $i)") != std::string::npos &&
              q.query.find("token gameTokenId appId expiresAt maxPlayers server { ip4 clientPort }") !=
                  std::string::npos,
          "open: the mutation and its selection are the contract's");
    check(q.variables == nlohmann::json({{"i", {{"appId", "7"}}}}),
          "open: the input is { appId } with the BigInt as a decimal string");
    check(q.bearer == kOrgBearer, "open: the bearer is the org token");
    check(s.token == kSessionKey && s.gameTokenId == 7000000001LL && s.appId == 7 &&
              s.maxPlayers == 256 && s.server.ip4 == "203.0.113.9" && s.server.port == 7777,
          "open: the answer is read, the gameTokenId from a decimal string");
    check(s.expiresAtMs == 1795723200000LL, "open: expiresAt is read as epoch milliseconds");
    check(rig.calls(ApiKind::OpenFleetSession, ApiCause::Setup) == 1 && rig.counters.total() == 1,
          "open: counted once, as openFleetSession for setup");

    Rig bad;
    bad.transport->script.push_back([] {
        auto a = sessionAnswer("openFleetSession");
        a["openFleetSession"]["token"] = "too-short";
        return a;
    });
    bool threw = false;
    try {
        bad.api->openFleetSession(ApiCause::Setup);
    } catch (const GraphQLError&) {
        threw = true;
    }
    check(threw && bad.counters.failures[static_cast<size_t>(ApiKind::OpenFleetSession)] == 1,
          "open: a token that is not 64 characters is an error, counted as a failure");
}

void testMintAndRetries() {
    Rig rig;
    // PLATFORM_BUSY naming its wait, then RATE_LIMITED naming none, then the answer.
    rig.transport->script.push_back([]() -> nlohmann::json {
        throw GraphQLError("The platform is busy", "PLATFORM_BUSY", false, 250);
    });
    rig.transport->script.push_back([]() -> nlohmann::json {
        throw GraphQLError("Too many sign-ins", "RATE_LIMITED", false);
    });
    rig.transport->script.push_back([] {
        return nlohmann::json{{"mintManagedPlayerToken",
                               {{"token", kPlayerKey},
                                {"gameTokenId", "9000000002"},
                                {"expiresAt", "2026-11-25T20:30:00.000Z"}}}};
    });
    const PlayerToken t = rig.api->mintManagedPlayerToken(12, ApiCause::Setup);

    check(rig.sent.size() == 3, "mint: retried until it was answered");
    const Sent& q = rig.sent.at(0);
    check(q.query.find("mutation($i: MintManagedPlayerTokenInput!)") != std::string::npos &&
              q.query.find("mintManagedPlayerToken(input: $i) { token gameTokenId expiresAt }") !=
                  std::string::npos,
          "mint: the mutation and its selection are the contract's");
    check(q.variables == nlohmann::json({{"i",
                                          {{"appId", "7"},
                                           {"externalId", "fleet-load-12"},
                                           {"gamertag", "fleet-load-12"},
                                           {"ageYears", 30},
                                           {"country", "US"}}}}),
          "mint: { appId, externalId fleet-load-<n>, gamertag, ageYears 30, country US }");
    check(q.bearer == kOrgBearer && rig.sent.at(2).variables == q.variables,
          "mint: the org token, and the same input on each retry");
    check(t.token == kPlayerKey && t.gameTokenId == 9000000002LL && t.expiresAtMs == 1795638600000LL,
          "mint: the player's token, id and expiry are read");
    check(rig.sleeps == std::vector<int>({250, 1500}),
          "mint: waits extensions.retryAfterMs, and 1500 ms when the error names none");
    const size_t k = static_cast<size_t>(ApiKind::MintManagedPlayerToken);
    check(rig.counters.platformBusy[k] == 1 && rig.counters.rateLimited[k] == 1,
          "mint: PLATFORM_BUSY and RATE_LIMITED are each counted");
    check(rig.calls(ApiKind::MintManagedPlayerToken, ApiCause::Setup) == 3 &&
              rig.counters.failures[k] == 0,
          "mint: every attempt is a counted API call, and none is a failure");
    check(playerExternalId(0) == "fleet-load-0" && playerExternalId(2999) == "fleet-load-2999",
          "mint: externalId is fleet-load-<n>");

    // An error that is neither is not retried.
    Rig refused;
    refused.transport->script.push_back([]() -> nlohmann::json {
        throw GraphQLError("Fleet sessions are not switched on", "FORBIDDEN", false);
    });
    std::string code;
    try {
        refused.api->mintManagedPlayerToken(1, ApiCause::Setup);
    } catch (const GraphQLError& e) {
        code = e.code();
    }
    check(code == "FORBIDDEN" && refused.sent.size() == 1 && refused.sleeps.empty() &&
              refused.counters.failures[k] == 1,
          "mint: another error is reported at once, with its code");

    // A transport failure is tried three times.
    Rig down;
    for (int i = 0; i < 3; ++i) {
        down.transport->script.push_back([]() -> nlohmann::json {
            throw GraphQLError("HTTP request failed: could not connect", "", true);
        });
    }
    bool threw = false;
    try {
        down.api->mintManagedPlayerToken(1, ApiCause::Setup);
    } catch (const GraphQLError& e) {
        threw = e.isTransport();
    }
    check(threw && down.sent.size() == 3 &&
              down.calls(ApiKind::MintManagedPlayerToken, ApiCause::Setup) == 3,
          "mint: a transport failure is tried three times, each counted");

    // It gives up on a platform that stays busy.
    Rig stuck;
    for (int i = 0; i <= FleetApi::MAX_BUSY_RETRIES; ++i) {
        stuck.transport->script.push_back([]() -> nlohmann::json {
            throw GraphQLError("busy", "PLATFORM_BUSY", false, 1);
        });
    }
    threw = false;
    try {
        stuck.api->mintManagedPlayerToken(1, ApiCause::Setup);
    } catch (const GraphQLError& e) {
        threw = e.code() == "PLATFORM_BUSY";
    }
    check(threw && stuck.counters.platformBusy[k] == FleetApi::MAX_BUSY_RETRIES + 1u,
          "mint: a platform that stays busy is given up on, with every answer counted");
}

void testCloseAndRefreshSession() {
    Rig rig;
    rig.transport->script.push_back([] { return nlohmann::json{{"closeFleetSession", true}}; });
    check(rig.api->closeFleetSession(7000000001LL, ApiCause::Teardown), "close: answers true");
    const Sent& c = rig.sent.at(0);
    check(c.query.find("mutation($i: CloseFleetSessionInput!) { closeFleetSession(input: $i) }") !=
              std::string::npos,
          "close: the mutation is the contract's");
    check(c.variables == nlohmann::json({{"i", {{"appId", "7"}, {"gameTokenId", "7000000001"}}}}) &&
              c.bearer == kOrgBearer,
          "close: { appId, gameTokenId } as decimal strings, with the org token");
    check(rig.calls(ApiKind::CloseFleetSession, ApiCause::Teardown) == 1,
          "close: counted as closeFleetSession for teardown");

    rig.transport->script.push_back([] {
        auto a = sessionAnswer("refreshFleetSession");
        a["refreshFleetSession"].erase("appId");
        a["refreshFleetSession"].erase("maxPlayers");
        a["refreshFleetSession"]["server"] = {{"ip4", "203.0.113.20"}, {"clientPort", 7780}};
        return a;
    });
    const FleetSessionInfo s = rig.api->refreshFleetSession(
        7000000001LL, ServerAddr{"203.0.113.9", 7777}, ApiCause::MoveErrorStreak);
    const Sent& r = rig.sent.at(1);
    check(r.query.find("mutation($i: RefreshFleetSessionInput!)") != std::string::npos &&
              r.query.find("refreshFleetSession(input: $i) { token gameTokenId expiresAt server { ip4 "
                           "clientPort } }") != std::string::npos,
          "refresh session: the mutation and its selection are the contract's");
    check(r.variables == nlohmann::json({{"i",
                                          {{"appId", "7"},
                                           {"gameTokenId", "7000000001"},
                                           {"currentServer",
                                            {{"ip4", "203.0.113.9"}, {"clientPort", 7777}}}}}}),
          "refresh session: { appId, gameTokenId, currentServer: { ip4, clientPort } }");
    check(r.variables["i"]["currentServer"]["clientPort"].is_number_integer(),
          "refresh session: clientPort is an Int");
    check(s.server.ip4 == "203.0.113.20" && s.server.port == 7780 && s.token == kSessionKey &&
              s.gameTokenId == 7000000001LL,
          "refresh session: the server to reconnect to, with the same token and id");
    check(rig.calls(ApiKind::RefreshFleetSession, ApiCause::MoveErrorStreak) == 1,
          "refresh session: counted as refreshFleetSession for the errors that made the session ask");

    // After a realtime server told the session to move, currentServer is left
    // out, and the API names a new server.
    rig.transport->script.push_back([] { return sessionAnswer("refreshFleetSession"); });
    rig.api->refreshFleetSession(7000000001LL, ServerAddr{}, ApiCause::MoveReconnectCommand);
    check(rig.sent.at(2).variables ==
              nlohmann::json({{"i", {{"appId", "7"}, {"gameTokenId", "7000000001"}}}}),
          "refresh session: told to move, the input leaves currentServer out");
    check(rig.calls(ApiKind::RefreshFleetSession, ApiCause::MoveReconnectCommand) == 1,
          "refresh session: counted apart, as a move a realtime server asked for");
    check(rig.sent.at(0).bearer == kOrgBearer && rig.sent.at(1).bearer == kOrgBearer &&
              rig.sent.at(2).bearer == kOrgBearer,
          "refresh session: one org token opens, refreshes and closes");
}

void testDeleteAndBlock() {
    Rig rig;
    rig.transport->script.push_back([] { return nlohmann::json{{"deleteManagedPlayer", true}}; });
    rig.transport->script.push_back([] { return nlohmann::json{{"deleteManagedPlayer", false}}; });
    check(rig.api->deleteManagedPlayer(77, "fleet-load-12", ApiCause::Cleanup),
          "delete: true when the player was deleted");
    check(!rig.api->deleteManagedPlayer(77, "fleet-load-13", ApiCause::Cleanup),
          "delete: false when there was no such player");
    const Sent& d = rig.sent.at(0);
    check(d.query.find("mutation($o: BigInt!, $e: String!) { deleteManagedPlayer(orgId: $o, "
                       "externalId: $e) }") != std::string::npos,
          "delete: deleteManagedPlayer(orgId, externalId)");
    check(d.variables == nlohmann::json({{"o", "77"}, {"e", "fleet-load-12"}}) && d.bearer == kOrgBearer,
          "delete: the organization's id as a decimal string, the externalId, the org token");
    check(rig.calls(ApiKind::DeleteManagedPlayer, ApiCause::Cleanup) == 2,
          "delete: counted as deleteManagedPlayer for cleanup");

    rig.transport->script.push_back(
        [] { return nlohmann::json{{"blockPlayer", {{"__typename", "PlayerBlock"}}}}; });
    rig.api->blockPlayer(kPlayerKey, 5001, ApiCause::RulesCheck);
    const Sent& b = rig.sent.at(2);
    check(b.query.find("blockPlayer(appId: $a, userId: $u)") != std::string::npos &&
              b.variables == nlohmann::json({{"a", "7"}, {"u", "5001"}}),
          "block: blockPlayer(appId, userId) with both as decimal strings");
    check(b.bearer == kPlayerKey, "block: the bearer is the blocking player's own app token");

    // A sign-in for the rule check: any externalId, any age.
    rig.transport->script.push_back([] {
        return nlohmann::json{{"mintManagedPlayerToken",
                               {{"token", kPlayerKey},
                                {"gameTokenId", "9000000002"},
                                {"expiresAt", "2026-11-25T20:30:00.000Z"}}}};
    });
    rig.api->mintManagedPlayerToken("fleet-rules-minor", 16, ApiCause::RulesCheck);
    check(rig.sent.at(3).variables["i"]["externalId"] == "fleet-rules-minor" &&
              rig.sent.at(3).variables["i"]["ageYears"] == 16,
          "mint: a named player with its age");
}

void testRefreshAppToken() {
    Rig rig;
    rig.transport->refreshHttpCalls = 2; // the helper retried once below GraphQL
    rig.transport->refreshScript.push_back(
        [] { return PlayerToken{kPlayerKey2, 9000000077LL, 1795640400000LL}; });
    const PlayerToken t = rig.api->refreshAppToken(kPlayerKey, ApiCause::TokenRefresh);
    check(rig.transport->refreshBearers == std::vector<std::string>({kPlayerKey}),
          "refresh token: the bearer is the player's own app token");
    check(rig.sent.empty(), "refresh token: nothing is sent with the org token");
    check(t.token == kPlayerKey2 && t.gameTokenId == 9000000077LL,
          "refresh token: the new token and its new gameTokenId are read");
    check(rig.calls(ApiKind::RefreshAppToken, ApiCause::TokenRefresh) == 2,
          "refresh token: every HTTP request it took is counted");
    check(rig.counters.byCause(ApiCause::Crossing) == 0,
          "refresh token: counted apart from crossings");

    // PLATFORM_BUSY is retried here as everywhere.
    Rig busy;
    busy.transport->refreshScript.push_back(
        []() -> PlayerToken { throw GraphQLError("busy", "PLATFORM_BUSY", false, 40); });
    busy.transport->refreshScript.push_back(
        [] { return PlayerToken{kPlayerKey2, 9000000078LL, 1}; });
    busy.api->refreshAppToken(kPlayerKey, ApiCause::TokenRefresh);
    check(busy.sleeps == std::vector<int>({40}) &&
              busy.counters.platformBusy[static_cast<size_t>(ApiKind::RefreshAppToken)] == 1,
          "refresh token: PLATFORM_BUSY waits retryAfterMs and tries again");
}

void testCountingAndCrossingGuard() {
    Rig rig;
    for (int i = 0; i < 2; ++i)
        rig.transport->script.push_back([] { return sessionAnswer("openFleetSession"); });
    rig.transport->script.push_back([] { return nlohmann::json{{"closeFleetSession", true}}; });
    rig.transport->script.push_back([] { return sessionAnswer("openFleetSession"); });
    rig.api->openFleetSession(ApiCause::Setup);
    rig.api->openFleetSession(ApiCause::Setup);
    rig.api->closeFleetSession(1, ApiCause::Teardown);
    check(rig.counters.byKind(ApiKind::OpenFleetSession) == 2 &&
              rig.counters.byKind(ApiKind::CloseFleetSession) == 1 &&
              rig.counters.byCause(ApiCause::Setup) == 2 &&
              rig.counters.byCause(ApiCause::Teardown) == 1 && rig.counters.total() == 3,
          "counting: by kind and by cause add up to every call");
    check(rig.counters.byCause(ApiCause::Crossing) == 0 &&
              rig.counters.requestsFromCrossingPath == 0,
          "counting: nothing is counted against a crossing");

    // Calls made while the measured window is open are counted apart as well.
    rig.counters.windowOpen = true;
    rig.api->openFleetSession(ApiCause::SessionKeep);
    rig.counters.windowOpen = false;
    check(rig.counters.duringWindow[static_cast<size_t>(ApiKind::OpenFleetSession)] == 1,
          "counting: a call inside the measured window is marked");

    // The guard: a call made from the code that handles a crossing is counted as
    // one, whatever cause it claims, so the run fails.
    check(!crossingScopeFlag(), "guard: off by default");
    Rig guarded;
    guarded.transport->script.push_back([] { return sessionAnswer("openFleetSession"); });
    {
        CrossingScope scope;
        check(crossingScopeFlag(), "guard: set inside the scope");
        guarded.api->openFleetSession(ApiCause::Setup);
    }
    check(!crossingScopeFlag(), "guard: cleared after the scope");
    check(guarded.counters.byCause(ApiCause::Crossing) == 1 &&
              guarded.counters.byCause(ApiCause::Setup) == 0 &&
              guarded.counters.requestsFromCrossingPath == 1,
          "guard: an API call from a crossing is counted as caused by a crossing");
}

void testMeasuredPhaseClasses() {
    // What made a session go back to the API decides what its refreshFleetSession
    // is counted as: a realtime server's word, or the session's own conclusion.
    check(moveCause(MoveTrigger::ReconnectCommand) == ApiCause::MoveReconnectCommand &&
              moveCause(MoveTrigger::SessionReleased) == ApiCause::MoveSessionReleased &&
              moveCause(MoveTrigger::RxSilence) == ApiCause::MoveRxSilence &&
              moveCause(MoveTrigger::SendErrors) == ApiCause::MoveSendErrors &&
              moveCause(MoveTrigger::ErrorStreak) == ApiCause::MoveErrorStreak,
          "classes: each trigger of a move has its own cause");
    int due = 0, asked = 0, other = 0;
    for (int c = 0; c < ApiCounters::C; ++c) {
        switch (apiCallClass(static_cast<ApiCause>(c))) {
            case ApiCallClass::RefreshDue: ++due; break;
            case ApiCallClass::AskedByServer: ++asked; break;
            case ApiCallClass::Other: ++other; break;
        }
    }
    check(due == 2 && apiCallClass(ApiCause::TokenRefresh) == ApiCallClass::RefreshDue &&
              apiCallClass(ApiCause::SessionKeep) == ApiCallClass::RefreshDue,
          "classes: a token refresh and a session keep are the refreshes that fall due, and nothing else is");
    check(asked == 3 && apiCallClass(ApiCause::MoveReconnectCommand) == ApiCallClass::AskedByServer &&
              apiCallClass(ApiCause::MoveSessionReleased) == ApiCallClass::AskedByServer &&
              apiCallClass(ApiCause::MoveRxSilence) == ApiCallClass::AskedByServer,
          "classes: a reconnect command, a release and silence are a realtime server asking, and nothing else is");
    check(other == ApiCounters::C - 5 && apiCallClass(ApiCause::MoveSendErrors) == ApiCallClass::Other &&
              apiCallClass(ApiCause::MoveErrorStreak) == ApiCallClass::Other &&
              apiCallClass(ApiCause::SignInAgain) == ApiCallClass::Other &&
              apiCallClass(ApiCause::Crossing) == ApiCallClass::Other &&
              apiCallClass(ApiCause::Setup) == ApiCallClass::Other,
          "classes: every other cause, a session's own move included, is anything else");

    // Counted while the measured window is open, each class apart.
    ApiCounters counters;
    counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveReconnectCommand); // before the window
    counters.windowOpen = true;
    for (int i = 0; i < 24; ++i) counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveReconnectCommand);
    counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveSessionReleased);
    counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveRxSilence);
    for (int i = 0; i < 7; ++i) counters.count(ApiKind::RefreshAppToken, ApiCause::TokenRefresh);
    counters.count(ApiKind::RefreshFleetSession, ApiCause::SessionKeep);
    MeasuredPhaseCalls m = measuredPhaseCalls(counters);
    check(m.total == 34 && m.refreshesDue == 8 && m.askedByServer == 26 && m.other == 0,
          "classes: a drain's refreshes are counted apart from the due ones, and nothing is left over");
    counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveErrorStreak);
    counters.count(ApiKind::RefreshFleetSession, ApiCause::MoveSendErrors);
    counters.count(ApiKind::MintManagedPlayerToken, ApiCause::SignInAgain);
    counters.windowOpen = false;
    counters.count(ApiKind::CloseFleetSession, ApiCause::Teardown); // after the window
    m = measuredPhaseCalls(counters);
    check(m.total == 37 && m.refreshesDue == 8 && m.askedByServer == 26 && m.other == 3 &&
              m.total == m.refreshesDue + m.askedByServer + m.other,
          "classes: a session's own move and a sign-in again are the rest, and the three add up");
    check(std::string(apiCauseName(ApiCause::MoveReconnectCommand)) == "session_move_reconnect_command" &&
              std::string(apiCauseName(ApiCause::MoveSessionReleased)) == "session_move_session_released" &&
              std::string(apiCauseName(ApiCause::MoveRxSilence)) == "session_move_rx_silence" &&
              std::string(apiCauseName(ApiCause::MoveSendErrors)) == "session_move_send_errors" &&
              std::string(apiCauseName(ApiCause::MoveErrorStreak)) == "session_move_error_streak",
          "classes: the summary names each cause after its trigger");
}

void testOrgTokenNeverLeaks() {
    Rig rig;
    // An error that (wrongly) echoes the Authorization header.
    rig.transport->script.push_back([]() -> nlohmann::json {
        throw GraphQLError("bad header: Bearer " + kOrgBearer + " rejected", "UNAUTHENTICATED", false);
    });
    std::string what;
    try {
        rig.api->openFleetSession(ApiCause::Setup);
    } catch (const GraphQLError& e) {
        what = e.what();
    }
    check(!what.empty() && what.find(kOrgBearer) == std::string::npos &&
              what.find("[org token]") != std::string::npos,
          "secrecy: an error that echoes the org token is redacted");
    check(rig.api->redact("x " + kOrgBearer + " y " + kOrgBearer) == "x [org token] y [org token]",
          "secrecy: every occurrence is replaced");

    // The token is read from the environment, never from the command line.
    setenv("CK_ORG_TOKEN", kOrgBearer.c_str(), 1);
    const char* argv[] = {"ck-fleet-load", "--api", "https://api.example.invalid/graphql", "--app", "7",
                          "--servers", "4", "--players", "40"};
    FleetConfig cfg = FleetConfig::load(9, const_cast<char**>(argv));
    check(cfg.orgToken == kOrgBearer && cfg.validate().empty(),
          "options: the org token comes from CK_ORG_TOKEN");
    check(cfg.apiOrigin() == "https://api.example.invalid", "options: the API origin drops /graphql");
    unsetenv("CK_ORG_TOKEN");
    cfg = FleetConfig::load(9, const_cast<char**>(argv));
    const std::string problem = cfg.validate();
    check(cfg.orgToken.empty() && problem.find("CK_ORG_TOKEN") != std::string::npos,
          "options: a live run without CK_ORG_TOKEN is refused, and the message names the variable");
    cfg.dryRun = true;
    check(cfg.validate().empty(), "options: a dry run needs no token, API or app");

    FleetConfig defaults;
    check(defaults.servers == 100 && defaults.players == 3000 && defaults.durationS == 1800 &&
              defaults.updateHz == 5.0 && defaults.crossRate == 75.0 && defaults.signinRate == 20.0 &&
              defaults.latencySample == 100 && !defaults.ownState,
          "options: the defaults are the proof's (100 servers, 3,000 players, 30 minutes, 5 Hz, 75/s, 20/s)");
    check(defaults.carryResendMs == 250 && defaults.carryGiveUpMs == 5000 && defaults.releaseWaitMs == 5000,
          "options: resend a carry every 250 ms, give up after 5 s, wait 5 s for a release");
    check(!defaults.releaseFirst && defaults.carryRate == 60.0 && defaults.signedBundles &&
              defaults.boundControl,
          "options: the destination carries at a crossing, 60 carries a second a session at most");
    const char* planArgv[] = {"ck-fleet-load", "--dry-run", "--crossing-plan", "release-first",
                              "--signed-bundles=false", "--bound-control=false", "--carry-rate", "40"};
    FleetConfig plan = FleetConfig::load(8, const_cast<char**>(planArgv));
    check(plan.releaseFirst && !plan.signedBundles && !plan.boundControl && plan.carryRate == 40.0 &&
              plan.validate().empty(),
          "options: --crossing-plan release-first, --signed-bundles=false, --bound-control=false");
    plan.carryRate = 65;
    check(!plan.validate().empty(), "options: a carry rate above the server's 64 a second is refused");
    setenv("CK_ORG_TOKEN", kOrgBearer.c_str(), 1);
    const char* deleteArgv[] = {"ck-fleet-load", "--api", "https://api.example.invalid", "--app", "7",
                                "--delete-players"};
    FleetConfig del = FleetConfig::load(6, const_cast<char**>(deleteArgv));
    check(del.deletePlayers && del.validate().find("--org") != std::string::npos,
          "options: --delete-players needs --org");
    unsetenv("CK_ORG_TOKEN");
    FleetConfig wrong;
    wrong.dryRun = true;
    wrong.servers = 0;
    check(!wrong.validate().empty(), "options: no servers is refused");
    wrong.servers = 4;
    wrong.updateHz = 0;
    check(!wrong.validate().empty(), "options: an update rate of 0 is refused");
}

void testGraphQLErrorCarriesRetryAfter() {
    const GraphQLError plain("x", "FORBIDDEN", false);
    const GraphQLError busy("x", "PLATFORM_BUSY", false, 1234);
    check(plain.retryAfterMs() == -1 && busy.retryAfterMs() == 1234 && busy.code() == "PLATFORM_BUSY",
          "GraphQLError: carries extensions.retryAfterMs, -1 when there is none");
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testRoster() {
    char dirTemplate[] = "/tmp/fleet-roster-test-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    check(dir != nullptr, "roster: a scratch directory");
    if (!dir) return;
    const std::string path = std::string(dir) + "/players-roster.json";
    const std::string api = "https://api.example.invalid";

    Roster none;
    std::string error;
    check(loadRoster(path, api, 7, none, error) == RosterLoad::Missing && none.players.empty(),
          "roster: a file that does not exist yet is an empty roster to fill");

    Roster r;
    r.api = api + "/";
    r.appId = 7;
    r.players[0] = PlayerToken{kPlayerKey, 9000000002LL, 1795638600000LL};
    r.players[41] = PlayerToken{kPlayerKey2, 9000000043LL, 1795638700000LL};
    // A file left behind with wider permissions is narrowed.
    {
        std::ofstream stale(path);
        stale << "{}";
    }
    chmod(path.c_str(), 0644);
    check(saveRoster(path, r, error), "roster: saves");
    struct stat st {};
    check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "roster: the file's mode is 0600");
    check(access((path + ".tmp." + std::to_string(getpid())).c_str(), F_OK) != 0,
          "roster: no temporary file is left behind");

    Roster back;
    check(loadRoster(path, api, 7, back, error) == RosterLoad::Loaded && back.players.size() == 2,
          "roster: loads what was saved");
    check(back.players[0].token == kPlayerKey && back.players[0].gameTokenId == 9000000002LL &&
              back.players[0].expiresAtMs == 1795638600000LL &&
              back.players[41].token == kPlayerKey2 && back.players[41].gameTokenId == 9000000043LL,
          "roster: tokens, ids and expiries survive the round trip");
    const std::string body = slurp(path);
    check(body.find("\"fleet-players\"") != std::string::npos &&
              body.find("\"fleet-load-41\"") != std::string::npos,
          "roster: the file names its kind and each player's externalId");

    Roster other;
    check(loadRoster(path, "https://other.example.invalid", 7, other, error) == RosterLoad::Refused &&
              error.find("minted against") != std::string::npos,
          "roster: one minted against another API is refused");
    check(loadRoster(path, api, 8, other, error) == RosterLoad::Refused &&
              error.find("app") != std::string::npos,
          "roster: one minted for another app is refused");
    check(error.find(kPlayerKey) == std::string::npos, "roster: a refusal does not print a token");

    {
        std::ofstream broken(path);
        broken << "{ \"kind\": \"fleet-players\", \"token\": \"" << kPlayerKey << "\", ";
    }
    check(loadRoster(path, api, 7, other, error) == RosterLoad::Refused &&
              error.find(kPlayerKey) == std::string::npos,
          "roster: a file that is not JSON is refused without quoting it");
    {
        std::ofstream foreign(path);
        foreign << "{ \"origin\": \"x\", \"sessions\": [] }";
    }
    check(loadRoster(path, api, 7, other, error) == RosterLoad::Refused,
          "roster: the per-player harness's roster is not taken for this one");

    unlink(path.c_str());
    rmdir(dir);
}

} // namespace

int main() {
    testOpen();
    testMintAndRetries();
    testCloseAndRefreshSession();
    testDeleteAndBlock();
    testRefreshAppToken();
    testCountingAndCrossingGuard();
    testMeasuredPhaseClasses();
    testOrgTokenNeverLeaks();
    testGraphQLErrorCarriesRetryAfter();
    testRoster();

    if (g_failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all fleet API tests passed\n");
    return 0;
}
