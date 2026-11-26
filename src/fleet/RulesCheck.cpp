#include "fleet/Modes.hpp"

#include "Wire.hpp"
#include "fleet/FleetApi.hpp"
#include "fleet/FleetStats.hpp"
#include "fleet/FleetWire.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <random>
#include <thread>

/// `--rules-check`: the per-player rules, checked on carried players.
///
/// Two fleet sessions, X and Y, carry six players who stand in one chunk:
///   X carries a, c and idle; Y carries b, d and minor.
/// a has blocked b. minor is 16. idle sends one update and then only heartbeats.
/// Each check sends something and reads what the realtime servers answer and
/// deliver. It is a small synchronous driver of its own, apart from the load
/// run's workers, because a check has to tie one message to its answer.
namespace lt::fleet {

namespace {

constexpr int64_t MS = 1000000;
constexpr int64_t RULES_CHUNK = 5000;      // away from the load run's world around (0, 0)
constexpr uint8_t ERR_INVALID_REQUEST = 15;

/// One thing a session received.
struct Seen {
    enum class Kind { CarryResult, Released, Delivery, Error, Plain, HmacFailure, MoveCommand };
    Kind kind = Kind::Plain;
    int64_t atNs = 0;
    int64_t playerId = 0;          // CarryResult, Released
    int64_t userId = 0;            // CarryResult
    uint8_t code = 0;              // status, reason or error code
    uint8_t seq = 0;               // Error: the sequence it answers
    uint8_t type = 0;              // Delivery: the notification inside; Plain: the message
    std::vector<int64_t> listed;   // Delivery
    std::string payload;           // Delivery, Plain: the notification's app payload
    size_t datagramLen = 0;
    FrameAuth auth = FrameAuth::Failed;
};

struct RSession;

struct RPlayer {
    std::string externalId;
    std::string label;             // "a", "b", ...
    int age = 30;
    PlayerToken tok;
    int64_t userId = 0;
    char uuid[32] = {};
    RSession* home = nullptr;
    int status = -1;               // the last FLEET_CARRY_RESULT status, -1 before any
    bool moving = true;            // sends updates; false: heartbeats only
    int64_t lastCarryMs = 0;
    int64_t lastInputNs = 0;
    double x = 100.0;
    std::string mintError;         // why the sign-in was refused, if it was
};

struct RSession {
    std::string name;
    const FleetConfig* cfg = nullptr;
    FleetSessionInfo info;
    uint8_t token[hmac::TOKEN_OCTETS] = {};
    uint8_t nonce[CONTROL_NONCE_LEN] = {};
    int fd = -1;
    uint8_t seq = 0;
    bool boundAcked = false;
    std::vector<Seen> seen;

    uint8_t sendRaw(const uint8_t* buf, size_t len) {
        const uint8_t used = seq++;
        if (len) (void)send(fd, buf, len, 0);
        return used;
    }

    void capabilities() {
        uint32_t flags = cfg->signedBundles ? wire::CAP_BUNDLE_SIGNED : 0;
        uint8_t buf[CAPABILITIES_BOUND_SIZE];
        sendRaw(buf, buildCapabilities(buf, cfg->appId, info.gameTokenId, flags,
                                       cfg->boundControl ? nonce : nullptr, seq, token));
    }
    void keepalive() {
        uint8_t buf[HEARTBEAT_SIZE];
        sendRaw(buf, buildHeartbeat(buf, cfg->appId, info.gameTokenId, seq, token));
    }
    void carry(RPlayer& p) {
        p.lastCarryMs = std::max(epochMs(), p.lastCarryMs + 1);
        uint8_t buf[FLEET_CARRY_SIZE];
        sendRaw(buf, buildCarry(buf, cfg->appId, info.gameTokenId, p.tok.gameTokenId,
                                p.lastCarryMs,
                                reinterpret_cast<const uint8_t*>(p.tok.token.data()), seq, token));
    }
    void release(const RPlayer& p) {
        uint8_t buf[FLEET_RELEASE_SIZE];
        sendRaw(buf, buildRelease(buf, cfg->appId, info.gameTokenId, p.tok.gameTokenId, seq, token));
    }
    /// A long-form message at the rule check's chunk. `tailId` is the game token
    /// id the tail names; the HMAC is always the fleet token's.
    uint8_t message(uint8_t type, const char* uuid32, const std::string& payload,
                    int64_t tailId) {
        uint8_t buf[MAX_CLIENT_DATAGRAM];
        return sendRaw(buf, buildLongForm(buf, type, cfg->appId, RULES_CHUNK, RULES_CHUNK, 0,
                                          static_cast<uint8_t>(cfg->distance),
                                          static_cast<uint8_t>(cfg->decay), uuid32,
                                          reinterpret_cast<const uint8_t*>(payload.data()),
                                          payload.size(), tailId, seq, token));
    }
    /// An actor update whose state differs from the last, so it is gameplay input.
    uint8_t update(RPlayer& p, const char* uuid32, int64_t tailId) {
        p.x += 1.0;
        std::string state(wire::state::SIZE, '\0');
        state[wire::state::VERSION] = static_cast<char>(wire::state::VERSION_VALUE);
        std::memcpy(&state[wire::state::POSITION_X], &p.x, 8);
        return message(wire::ACTOR_UPDATE_REQUEST_2, uuid32, state, tailId);
    }
    uint8_t heartbeat(const RPlayer& p) {
        uint8_t buf[HEARTBEAT_SIZE];
        return sendRaw(buf, buildPlayerHeartbeat(buf, cfg->appId, RULES_CHUNK, RULES_CHUNK, 0,
                                                 p.uuid, p.tok.gameTokenId, seq, token));
    }

    /// Read everything waiting on the socket into `seen`.
    void pump() {
        uint8_t buf[2048]; // the largest datagram a fleet session is sent is 1,242 bytes
        for (;;) {
            const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) return;
            datagram(buf, static_cast<size_t>(n));
        }
    }

    void note(Seen s) {
        s.atNs = nowNs();
        seen.push_back(std::move(s));
    }

    void datagram(const uint8_t* data, size_t len) {
        const uint8_t first = data[0];
        if (first == CONTROL_BOUND_ACK) {
            if (verifyBoundAck(data, len, token, nonce)) boundAcked = true;
            return;
        }
        if (first == wire::COMMAND_RECONNECT || first == COMMAND_RECONNECT_BOUND ||
            first == COMMAND_SESSION_RELEASED || first == COMMAND_SESSION_RELEASED_BOUND) {
            Seen s;
            s.kind = Seen::Kind::MoveCommand;
            s.type = first;
            note(std::move(s));
            return;
        }
        const bool signedBundle = first == wire::MESSAGE_BUNDLE_SIGNED;
        if (signedBundle && !wire::verifySignedBundle(data, len, token)) {
            Seen s;
            s.kind = Seen::Kind::HmacFailure;
            s.type = first;
            note(std::move(s));
            return;
        }
        wire::forEachMessage(data, len, [&](const wire::InboundView& m) {
            member(m, signedBundle, len);
        });
    }

    void member(const wire::InboundView& m, bool trusted, size_t datagramLen) {
        const uint8_t type = m.type();
        Seen s;
        s.datagramLen = datagramLen;
        if (type == wire::GENERIC_ERROR_MESSAGE && m.len >= 3) {
            s.kind = Seen::Kind::Error;
            s.seq = m.data[1];
            s.code = m.data[2];
            note(std::move(s));
            return;
        }
        if (type == FLEET_CARRY_RESULT || type == FLEET_RELEASED || type == FLEET_DELIVERY) {
            const FleetFrame f = readFleetFrame(m, trusted, token);
            s.auth = f.auth;
            s.type = type;
            if (f.auth == FrameAuth::Failed) {
                s.kind = Seen::Kind::HmacFailure;
                note(std::move(s));
                return;
            }
            if (const auto r = parseCarryResult(f.frame)) {
                s.kind = Seen::Kind::CarryResult;
                s.playerId = r->playerGameTokenId;
                s.userId = r->userId;
                s.code = r->status;
                note(std::move(s));
            } else if (const auto rel = parseReleased(f.frame)) {
                s.kind = Seen::Kind::Released;
                s.playerId = rel->playerGameTokenId;
                s.code = rel->reason;
                note(std::move(s));
            } else if (const auto d = parseDelivery(f.frame)) {
                s.kind = Seen::Kind::Delivery;
                s.type = d->notification.type();
                for (size_t i = 0; i < d->count; ++i) s.listed.push_back(d->player(i));
                if (const auto p = notificationPayload(d->notification))
                    s.payload.assign(reinterpret_cast<const char*>(p->data), p->len);
                note(std::move(s));
            }
            return;
        }
        if ((type & wire::SPATIAL_TYPE_BIT) && !trusted && !wire::verifyNotification(m, token)) {
            s.kind = Seen::Kind::HmacFailure;
            s.type = type;
            note(std::move(s));
            return;
        }
        s.kind = Seen::Kind::Plain;
        s.type = type;
        if (type & wire::SPATIAL_TYPE_BIT) {
            if (const auto p = notificationPayload(m))
                s.payload.assign(reinterpret_cast<const char*>(p->data), p->len);
        }
        note(std::move(s));
    }

    /// The error that answered sequence `seq`, among what was seen from `from` on.
    const Seen* errorFor(uint8_t wanted, size_t from) const {
        for (size_t i = from; i < seen.size(); ++i) {
            if (seen[i].kind == Seen::Kind::Error && seen[i].seq == wanted) return &seen[i];
        }
        return nullptr;
    }
};

struct Check {
    std::string name;
    std::string result; // PASS, FAIL or SKIP
    std::string observed;
};

class Rules {
public:
    Rules(const FleetConfig& cfg, const RunDeps& deps, std::atomic<bool>& stop, FILE* log)
        : cfg_(cfg), deps_(deps), stop_(stop), log_(log),
          api_(deps.makeTransport(), cfg.orgToken, cfg.appId, counters_, &stop) {
        std::random_device entropy;
        rng_.seed((static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^
                  static_cast<uint64_t>(epochMs()));
        tag_ = std::to_string(rng_() % 1000000007ULL);
        const auto& ids = rulesPlayerIds();
        const char* labels[] = {"a", "b", "c", "d", "minor", "idle"};
        for (size_t i = 0; i < ids.size(); ++i) {
            RPlayer p;
            p.externalId = ids[i];
            p.label = labels[i];
            p.age = p.label == "minor" ? 16 : 30;
            p.moving = p.label != "idle";
            // A stable actor UUID for each player, as the load run has.
            char uuid[64];
            std::snprintf(uuid, sizeof(uuid), "ckfleetrules%020llu",
                          static_cast<unsigned long long>(cfg.appId * 100 + static_cast<int64_t>(i)));
            std::memcpy(p.uuid, uuid, sizeof(p.uuid)); // 12 + 20 characters
            players_.push_back(std::move(p));
        }
    }

    int run(nlohmann::json& summary) {
        const bool ready = setup();
        if (ready) {
            checkBlock();
            checkCommunicationsOnlyInDeliveries();
            checkMinorVoice();
            checkAnotherPlayersActor();
            checkNotCarried();
            checkOwnId();
            checkLargeDelivery();
            checkIdle();
        }
        teardown();

        bool pass = ready && !stop_.load();
        nlohmann::json checks = nlohmann::json::array();
        for (const auto& c : checks_) {
            if (c.result == "FAIL") pass = false;
            checks.push_back({{"name", c.name}, {"result", c.result}, {"observed", c.observed}});
        }
        nlohmann::json calls = nlohmann::json::object();
        for (int k = 0; k < ApiCounters::K; ++k)
            calls[apiKindName(static_cast<ApiKind>(k))] = counters_.byKind(static_cast<ApiKind>(k));
        summary = {
            {"tool", "ck-fleet-load"},
            {"mode", deps_.mode == "dry-run" ? "rules-check dry-run" : "rules-check"},
            {"pass", pass},
            {"exit_code", pass ? 0 : (stop_.load() ? 130 : 1)},
            {"interrupted", stop_.load()},
            {"checks", checks},
            {"sessions", {{"x", sessionJson(x_)}, {"y", sessionJson(y_)}}},
            {"api", {{"calls", counters_.total()}, {"calls_by_kind", calls}}},
        };
        return summary["exit_code"].get<int>();
    }

private:
    RPlayer& player(const char* label) {
        for (auto& p : players_) {
            if (p.label == label) return p;
        }
        return players_.front();
    }
    std::string name(int64_t gameTokenId) const {
        for (const auto& p : players_) {
            if (p.tok.gameTokenId == gameTokenId) return p.label;
        }
        return "?" + std::to_string(gameTokenId);
    }
    std::string names(const std::vector<int64_t>& ids) const {
        std::string out;
        for (int64_t id : ids) out += (out.empty() ? "" : ", ") + name(id);
        return out.empty() ? "nobody" : out;
    }
    static nlohmann::json sessionJson(const RSession& s) {
        return {{"game_token_id", std::to_string(s.info.gameTokenId)},
                {"server", s.info.server.ip4 + ":" + std::to_string(s.info.server.port)},
                {"received", s.seen.size()}};
    }

    double step() const { return cfg_.rulesStepS; }

    void report(const std::string& checkName, const char* result, const std::string& observed) {
        checks_.push_back(Check{checkName, result, observed});
        if (log_) {
            std::fprintf(log_, "[rules] %s  %s: %s\n", result, checkName.c_str(), observed.c_str());
            std::fflush(log_);
        }
    }
    void pass(const std::string& n, const std::string& o) { report(n, "PASS", o); }
    void fail(const std::string& n, const std::string& o) { report(n, "FAIL", o); }
    void verdict(const std::string& n, bool ok, const std::string& o) {
        report(n, ok ? "PASS" : "FAIL", o);
    }

    /// Let `seconds` pass: every 200 ms each carried player sends an update (or,
    /// for one that does not move, a heartbeat every 2 s), each session sends its
    /// keepalive every 2 s, and everything received is read.
    void spin(double seconds, const std::function<bool()>& until = nullptr) {
        const int64_t end = nowNs() + static_cast<int64_t>(seconds * 1e9);
        while (nowNs() < end && !stop_.load()) {
            const int64_t now = nowNs();
            if (now >= nextUpdateNs_) {
                nextUpdateNs_ = now + 200 * MS;
                for (auto& p : players_) {
                    if (p.status != CARRY_CARRIED || !p.moving) continue;
                    p.home->update(p, p.uuid, p.tok.gameTokenId);
                    p.lastInputNs = now;
                }
            }
            if (now >= nextSlowNs_) {
                nextSlowNs_ = now + 2000 * MS;
                x_.keepalive();
                y_.keepalive();
                for (auto& p : players_) {
                    // A heartbeat under the PLAYER's id keeps its actor present. It
                    // is not gameplay input.
                    if (p.status == CARRY_CARRIED && !p.moving) p.home->heartbeat(p);
                }
            }
            pollfd fds[2] = {{x_.fd, POLLIN, 0}, {y_.fd, POLLIN, 0}};
            (void)poll(fds, 2, 10);
            x_.pump();
            y_.pump();
            absorb();
            if (until && until()) return;
        }
    }

    /// Take carry results into the players.
    void absorb() {
        for (RSession* s : {&x_, &y_}) {
            for (size_t& i = s == &x_ ? absorbedX_ : absorbedY_; i < s->seen.size(); ++i) {
                const Seen& e = s->seen[i];
                if (e.kind != Seen::Kind::CarryResult) continue;
                for (auto& p : players_) {
                    if (p.home == s && p.tok.gameTokenId == e.playerId) {
                        p.status = e.code;
                        if (e.code == CARRY_CARRIED) p.userId = e.userId;
                    }
                }
            }
        }
    }

    bool setup() {
        // Two sessions, opened by the one org token.
        try {
            x_.info = api_.openFleetSession(ApiCause::RulesCheck);
            y_.info = api_.openFleetSession(ApiCause::RulesCheck);
        } catch (const std::exception& e) {
            fail("setup", std::string("openFleetSession failed: ") + e.what());
            return false;
        }
        int at = 0;
        for (RSession* s : {&x_, &y_}) {
            s->name = at++ == 0 ? "X" : "Y";
            s->cfg = &cfg_;
            std::memcpy(s->token, s->info.token.data(), sizeof(s->token));
            const uint64_t nonce = rng_();
            std::memcpy(s->nonce, &nonce, sizeof(s->nonce));
            std::string error;
            s->fd = deps_.connect(s->info, error);
            if (s->fd < 0) {
                fail("setup", "cannot open a socket to " + s->info.server.ip4 + ": " + error);
                return false;
            }
            opened_.push_back(s->info.gameTokenId);
        }

        // The players sign in once, with the org token.
        for (auto& p : players_) {
            p.home = (p.label == "a" || p.label == "c" || p.label == "idle") ? &x_ : &y_;
            try {
                p.tok = api_.mintManagedPlayerToken(p.externalId, p.age, ApiCause::RulesCheck);
            } catch (const GraphQLError& e) {
                p.mintError = (e.code().empty() ? std::string("error") : e.code()) + ": " + e.what();
            }
        }
        for (const char* needed : {"a", "b", "c", "d"}) {
            if (!player(needed).mintError.empty()) {
                fail("setup", std::string("player ") + needed + " could not be signed in: " +
                                  player(needed).mintError);
                return false;
            }
        }

        // The session's first signed datagram is its capabilities, under its own id.
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.sessionSettleMs));
        x_.capabilities();
        y_.capabilities();
        spin(0.3);

        // Carry every player, sending the carry again (a new time and proof) until
        // it is answered.
        const int64_t give = nowNs() + 6000 * MS;
        while (nowNs() < give && !stop_.load()) {
            bool all = true;
            for (auto& p : players_) {
                if (!p.mintError.empty() || p.status == CARRY_CARRIED) continue;
                all = false;
                p.home->carry(p);
            }
            if (all) break;
            spin(0.3);
        }
        std::string states;
        bool carried = true;
        for (const auto& p : players_) {
            if (!p.mintError.empty()) continue;
            states += (states.empty() ? "" : ", ") + p.label + " " +
                      (p.status < 0 ? std::string("unanswered") : carryStatusName(static_cast<uint8_t>(p.status)));
            carried = carried && p.status == CARRY_CARRIED;
        }
        verdict("carry", carried,
                "FLEET_CARRY_RESULT for each player on its session: " + states +
                    (x_.boundAcked && y_.boundAcked ? "; both sessions' bound control was acknowledged"
                                                    : ""));
        if (!carried) return false;

        // Everyone stands in one chunk and is present: the players that move send
        // updates from now on, and idle sends its one update here.
        RPlayer& idle = player("idle");
        idle.home->update(idle, idle.uuid, idle.tok.gameTokenId);
        idle.lastInputNs = nowNs();
        spin(0.6 * step());

        // a blocks b, with a's own app token, as a player's device would.
        try {
            api_.blockPlayer(player("a").tok.token, player("b").userId, ApiCause::RulesCheck);
            blockSet_ = true;
        } catch (const std::exception& e) {
            blockError_ = e.what();
        }
        spin(std::max(0.8 * step(), cfg_.dryRun ? 0.0 : 1.5)); // a block is enforced within a second
        return true;
    }

    /// The FLEET_DELIVERY frames a session received, from `from` on, whose
    /// notification is of `type` and carries `payload`.
    static std::vector<const Seen*> deliveries(const RSession& s, size_t from, uint8_t type,
                                               const std::string& payload) {
        std::vector<const Seen*> out;
        for (size_t i = from; i < s.seen.size(); ++i) {
            const Seen& e = s.seen[i];
            if (e.kind == Seen::Kind::Delivery && e.type == type && e.payload == payload)
                out.push_back(&e);
        }
        return out;
    }
    static std::vector<int64_t> listedIn(const std::vector<const Seen*>& frames) {
        std::vector<int64_t> all;
        for (const Seen* f : frames) all.insert(all.end(), f->listed.begin(), f->listed.end());
        return all;
    }
    static bool has(const std::vector<int64_t>& ids, int64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    void checkBlock() {
        if (!blockSet_) {
            fail("block", "blockPlayer (a blocks b, with a's own app token) was refused: " + blockError_ +
                              ". Set the block yourself and run again");
            return;
        }
        RPlayer &a = player("a"), &b = player("b"), &c = player("c"), &d = player("d");
        // b speaks beside a and c. c may hear it; a, who blocked b, may not.
        {
            const std::string text = "ck-rules b says " + tag_;
            const size_t from = x_.seen.size(), fromY = y_.seen.size();
            y_.message(wire::CLIENT_TEXT_PACKET_2, b.uuid, text, b.tok.gameTokenId);
            spin(step());
            const auto frames = deliveries(x_, from, CLIENT_TEXT_NOTIFICATION, text);
            const auto listed = listedIn(frames);
            const auto own = listedIn(deliveries(y_, fromY, CLIENT_TEXT_NOTIFICATION, text));
            verdict("block: the blocked player's text",
                    has(listed, c.tok.gameTokenId) && !has(listed, a.tok.gameTokenId),
                    "b's text reached session X in " + std::to_string(frames.size()) +
                        " FLEET_DELIVERY listing " + names(listed) +
                        " (c must be listed; a, who blocked b, must not). On b's own session it listed " +
                        names(own));
        }
        // And the other way: a speaks beside b and d.
        {
            const std::string text = "ck-rules a says " + tag_;
            const size_t from = y_.seen.size();
            x_.message(wire::CLIENT_TEXT_PACKET_2, a.uuid, text, a.tok.gameTokenId);
            spin(step());
            const auto frames = deliveries(y_, from, CLIENT_TEXT_NOTIFICATION, text);
            const auto listed = listedIn(frames);
            verdict("block: the blocking player's text",
                    has(listed, d.tok.gameTokenId) && !has(listed, b.tok.gameTokenId),
                    "a's text reached session Y in " + std::to_string(frames.size()) +
                        " FLEET_DELIVERY listing " + names(listed) +
                        " (d must be listed; b, whom a blocked, must not)");
        }
        // Movement still passes between two players who are blocked.
        size_t plain = 0;
        for (const auto& e : x_.seen) {
            if (e.kind == Seen::Kind::Plain && e.type == wire::ACTOR_UPDATE_NOTIFICATION_2) ++plain;
        }
        verdict("block: state still passes", plain > 0,
                "session X received " + std::to_string(plain) +
                    " plain actor updates from session Y's players, b among them");
    }

    void checkCommunicationsOnlyInDeliveries() {
        size_t plain = 0, deliveriesSeen = 0, hmac = 0;
        std::string types;
        for (const RSession* s : {&x_, &y_}) {
            for (const auto& e : s->seen) {
                if (e.kind == Seen::Kind::Delivery) ++deliveriesSeen;
                if (e.kind == Seen::Kind::HmacFailure) ++hmac;
                if (e.kind == Seen::Kind::Plain && isCommunication(e.type)) {
                    ++plain;
                    types += (types.empty() ? "" : ", ") + std::to_string(e.type);
                }
            }
        }
        verdict("communications arrive only in FLEET_DELIVERY", plain == 0 && hmac == 0,
                std::to_string(deliveriesSeen) + " FLEET_DELIVERY frames, " + std::to_string(plain) +
                    " communications outside one" + (plain ? " (types " + types + ")" : "") + ", " +
                    std::to_string(hmac) + " frames whose HMAC failed");
    }

    void checkMinorVoice() {
        RPlayer& minor = player("minor");
        RPlayer& d = player("d");
        if (!minor.mintError.empty()) {
            fail("a player under 18's voice",
                 "the 16-year-old could not be signed in (" + minor.mintError +
                     "). The app must admit players under 18: see the setup for the rule check");
            return;
        }
        if (minor.status != CARRY_CARRIED) {
            fail("a player under 18's voice", "the 16-year-old was not carried");
            return;
        }
        // The minor's voice is off until a parent agrees: the send is refused.
        const std::string voice = "ck-rules minor voice " + tag_;
        const size_t fromX = x_.seen.size(), fromY = y_.seen.size();
        const uint8_t seq = y_.message(CLIENT_AUDIO_PACKET, minor.uuid, voice, minor.tok.gameTokenId);
        spin(step());
        const Seen* refusal = y_.errorFor(seq, fromY);
        const size_t delivered = deliveries(x_, fromX, CLIENT_AUDIO_NOTIFICATION, voice).size() +
                                 deliveries(y_, fromY, CLIENT_AUDIO_NOTIFICATION, voice).size();
        // The same from an adult beside it, to show voice is delivered here at all.
        const std::string control = "ck-rules adult voice " + tag_;
        const size_t fromX2 = x_.seen.size();
        y_.message(CLIENT_AUDIO_PACKET, d.uuid, control, d.tok.gameTokenId);
        spin(step());
        const auto heard = listedIn(deliveries(x_, fromX2, CLIENT_AUDIO_NOTIFICATION, control));
        verdict("a player under 18's voice",
                refusal && refusal->code == wire::ERR_UNAUTHORIZED && delivered == 0,
                std::string("the 16-year-old's voice was ") +
                    (refusal ? "answered " + std::string(errorCodeName(refusal->code)) + " (" +
                                   std::to_string(refusal->code) + ")"
                             : "not answered") +
                    " and delivered in " + std::to_string(delivered) +
                    " FLEET_DELIVERY (it must be refused UNAUTHORIZED and delivered in none). An "
                    "adult's voice from the same place reached session X listing " +
                    names(heard));
    }

    void checkAnotherPlayersActor() {
        RPlayer &a = player("a"), &b = player("b"), &c = player("c");
        // c, carried by the same session as a, sends under a's actor. So does b,
        // from the other session. Each player owns its own actor only.
        const size_t fromX = x_.seen.size(), fromY = y_.seen.size();
        const uint8_t s1 = x_.update(c, a.uuid, c.tok.gameTokenId);
        const uint8_t s2 = y_.update(b, a.uuid, b.tok.gameTokenId);
        spin(0.8 * step());
        const Seen* r1 = x_.errorFor(s1, fromX);
        const Seen* r2 = y_.errorFor(s2, fromY);
        auto say = [](const Seen* r) {
            return r ? std::string(errorCodeName(r->code)) + " (" + std::to_string(r->code) + ")"
                     : std::string("not refused");
        };
        verdict("another player's actor",
                r1 && r1->code == wire::ERR_UNAUTHORIZED && r2 && r2->code == wire::ERR_UNAUTHORIZED,
                "an update under a's actor sent for c (same session): " + say(r1) +
                    "; sent for b (the other session): " + say(r2) + ". Both must be UNAUTHORIZED");
    }

    void checkNotCarried() {
        RPlayer& b = player("b");
        // Session X names b, whom session Y carries.
        const size_t from = x_.seen.size();
        const uint8_t seq = x_.update(b, b.uuid, b.tok.gameTokenId);
        spin(0.6 * step());
        const Seen* r = x_.errorFor(seq, from);
        verdict("a player the session does not carry", r && r->code == wire::ERR_UNAUTHORIZED,
                std::string("an update session X sent for b, whom session Y carries, was ") +
                    (r ? "answered " + std::string(errorCodeName(r->code)) : "not refused") +
                    " (it must be UNAUTHORIZED)");
    }

    void checkOwnId() {
        RPlayer& c = player("c");
        // Under its own id a session sends only 29, 49, 50 and 26. An actor update
        // under the session's id, and capabilities under a player's, are refused.
        const size_t from = x_.seen.size();
        const uint8_t s1 = x_.update(c, c.uuid, x_.info.gameTokenId);
        uint32_t flags = wire::CAP_BUNDLE_SIGNED;
        const std::string caps(reinterpret_cast<const char*>(&flags), 4);
        uint8_t buf[MAX_CLIENT_DATAGRAM];
        const uint8_t s2 = x_.sendRaw(
            buf, buildLongForm(buf, wire::CLIENT_CAPABILITIES, cfg_.appId, 0, 0, 0, 0, 0, nullptr,
                               reinterpret_cast<const uint8_t*>(caps.data()), caps.size(),
                               c.tok.gameTokenId, x_.seq, x_.token));
        spin(0.6 * step());
        const Seen* r1 = x_.errorFor(s1, from);
        const Seen* r2 = x_.errorFor(s2, from);
        verdict("what a session may send under its own id",
                r1 && r1->code == wire::ERR_UNAUTHORIZED && r2 && r2->code == ERR_INVALID_REQUEST,
                std::string("an actor update under the session's own id was ") +
                    (r1 ? "answered " + std::string(errorCodeName(r1->code)) : "not refused") +
                    " (it must be UNAUTHORIZED); capabilities under a player's id were " +
                    (r2 ? "answered " + std::string(errorCodeName(r2->code)) : "not refused") +
                    " (they must be INVALID_REQUEST)");
    }

    void checkLargeDelivery() {
        RPlayer &c = player("c"), &d = player("d");
        // The largest text a client may send. Its FLEET_DELIVERY cannot be a
        // member of a signed bundle: it comes alone, with its own HMAC, in a
        // datagram of up to 1,242 bytes.
        std::string text(MAX_CLIENT_DATAGRAM - wire::HEADER_SIZE - wire::TAIL_WITH_HMAC, 'x');
        const std::string head = "ck-rules large " + tag_ + " ";
        text.replace(0, head.size(), head);
        const size_t from = x_.seen.size();
        y_.message(wire::CLIENT_TEXT_PACKET_2, d.uuid, text, d.tok.gameTokenId);
        spin(step());
        const auto frames = deliveries(x_, from, CLIENT_TEXT_NOTIFICATION, text);
        size_t largest = 0, ownHmac = 0;
        for (const Seen* f : frames) {
            largest = std::max(largest, f->datagramLen);
            if (f->auth == FrameAuth::OwnHmac) ++ownHmac;
        }
        const auto listed = listedIn(frames);
        verdict("the largest FLEET_DELIVERY",
                !frames.empty() && has(listed, c.tok.gameTokenId) && largest <= MAX_SERVER_DATAGRAM &&
                    ownHmac == frames.size(),
                "a " + std::to_string(text.size()) + "-byte text reached session X in " +
                    std::to_string(frames.size()) + " FLEET_DELIVERY listing " + names(listed) + ", " +
                    std::to_string(ownHmac) + " with their own HMAC, the largest datagram " +
                    std::to_string(largest) + " bytes (at most 1242)");
    }

    void checkIdle() {
        RPlayer& idle = player("idle");
        if (cfg_.rulesIdleTimeoutS <= 0) {
            report("the idle release", "SKIP", "--rules-idle-timeout-s 0");
            return;
        }
        // idle sent one update at the start and only heartbeats since. The
        // others kept sending updates. After the app's idle timeout the session
        // must be sent FLEET_RELEASED reason 5 for idle, and for nobody else.
        const double margin = cfg_.dryRun ? 2.0 : 45.0;
        const double limit = cfg_.rulesIdleTimeoutS + margin;
        auto released = [&]() -> const Seen* {
            for (const auto& e : x_.seen) {
                if (e.kind == Seen::Kind::Released && e.playerId == idle.tok.gameTokenId) return &e;
            }
            return nullptr;
        };
        const double waited = static_cast<double>(nowNs() - idle.lastInputNs) * 1e-9;
        if (log_ && limit > waited + 5) {
            std::fprintf(log_, "[rules] waiting up to %.0f s for the idle release (the app's idle "
                               "timeout is %.0f s)\n",
                         limit - waited, cfg_.rulesIdleTimeoutS);
        }
        if (limit > waited) spin(limit - waited, [&] { return released() != nullptr; });
        const Seen* r = released();
        size_t others = 0;
        for (const RSession* s : {&x_, &y_}) {
            for (const auto& e : s->seen) {
                if (e.kind == Seen::Kind::Released && e.code == RELEASED_IDLE &&
                    e.playerId != idle.tok.gameTokenId)
                    ++others;
            }
        }
        const double after = r ? static_cast<double>(r->atNs - idle.lastInputNs) * 1e-9 : 0.0;
        char text[400];
        if (r) {
            std::snprintf(text, sizeof(text),
                          "FLEET_RELEASED reason %u (%s) for idle %.1f s after its last gameplay "
                          "input, with heartbeats under its id all the while (the timeout is %.0f "
                          "s); %zu other player(s) were released as idle (must be 0)",
                          r->code, releaseReasonName(r->code), after, cfg_.rulesIdleTimeoutS, others);
        } else {
            std::snprintf(text, sizeof(text),
                          "no FLEET_RELEASED for idle within %.0f s of its last gameplay input. Is "
                          "the app's idle timeout %.0f s?",
                          limit, cfg_.rulesIdleTimeoutS);
        }
        if (r) idle.status = -1; // no longer carried
        verdict("the idle release", r && r->code == RELEASED_IDLE && others == 0, text);
    }

    void teardown() {
        // The players leave: FLEET_RELEASE for each, and the sessions close.
        for (auto& p : players_) {
            if (p.home && p.home->fd >= 0 && p.status == CARRY_CARRIED) p.home->release(p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (int64_t id : opened_) {
            try {
                api_.closeFleetSession(id, ApiCause::RulesCheck);
            } catch (const std::exception& e) {
                if (log_) std::fprintf(log_, "[rules] closeFleetSession failed: %s\n", e.what());
            }
        }
        for (RSession* s : {&x_, &y_}) {
            if (s->fd >= 0) close(s->fd);
            s->fd = -1;
        }
    }

    const FleetConfig& cfg_;
    const RunDeps& deps_;
    std::atomic<bool>& stop_;
    FILE* log_;
    ApiCounters counters_;
    FleetApi api_;
    std::mt19937_64 rng_;
    std::string tag_;              // makes this run's texts its own
    std::vector<RPlayer> players_;
    RSession x_, y_;
    std::vector<int64_t> opened_;
    std::vector<Check> checks_;
    size_t absorbedX_ = 0, absorbedY_ = 0;
    int64_t nextUpdateNs_ = 0, nextSlowNs_ = 0;
    bool blockSet_ = false;
    std::string blockError_;
};

} // namespace

int runRulesCheck(const FleetConfig& cfg, const RunDeps& deps, std::atomic<bool>& stop,
                  nlohmann::json& summary, FILE* log) {
    try {
        Rules rules(cfg, deps, stop, log);
        return rules.run(summary);
    } catch (const std::exception& e) {
        summary = {{"tool", "ck-fleet-load"},
                   {"mode", "rules-check"},
                   {"pass", false},
                   {"exit_code", 2},
                   {"error", e.what()}};
        return 2;
    }
}

} // namespace lt::fleet
