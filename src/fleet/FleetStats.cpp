#include "fleet/FleetStats.hpp"

#include "Wire.hpp"
#include "fleet/FleetWire.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lt::fleet {

namespace {
const std::chrono::steady_clock::time_point kStart = std::chrono::steady_clock::now();
constexpr double kFirstBoundUs = 50.0;
constexpr double kRatio = 1.25;
} // namespace

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - kStart)
        .count();
}

int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double Hist::boundUs(int i) { return kFirstBoundUs * std::pow(kRatio, i); }

void Hist::record(int64_t us) {
    if (us < 0) us = 0;
    int idx = 0;
    if (static_cast<double>(us) >= kFirstBoundUs) {
        idx = 1 + static_cast<int>(std::floor(std::log(static_cast<double>(us) / kFirstBoundUs) /
                                              std::log(kRatio)));
        if (idx >= BUCKETS) idx = BUCKETS - 1;
    }
    buckets_[static_cast<size_t>(idx)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
    uint64_t prev = max_.load(std::memory_order_relaxed);
    const uint64_t v = static_cast<uint64_t>(us);
    while (v > prev && !max_.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

double Hist::meanMs() const {
    const uint64_t n = count_.load(std::memory_order_relaxed);
    return n ? static_cast<double>(sum_.load(std::memory_order_relaxed)) / n / 1000.0 : 0.0;
}

double Hist::percentileMs(double p) const {
    std::array<uint64_t, BUCKETS> snap{};
    uint64_t total = 0;
    for (int i = 0; i < BUCKETS; ++i) {
        snap[static_cast<size_t>(i)] = buckets_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
        total += snap[static_cast<size_t>(i)];
    }
    if (!total) return 0.0;
    const double want = p * static_cast<double>(total);
    uint64_t seen = 0;
    for (int i = 0; i < BUCKETS; ++i) {
        const uint64_t n = snap[static_cast<size_t>(i)];
        if (!n) continue;
        if (static_cast<double>(seen + n) >= want) {
            const double lo = i == 0 ? 0.0 : boundUs(i - 1);
            double hi = boundUs(i);
            const double maxUs = static_cast<double>(max_.load(std::memory_order_relaxed));
            if (hi > maxUs && maxUs >= lo) hi = maxUs; // never report past the exact maximum
            const double frac = (want - static_cast<double>(seen)) / static_cast<double>(n);
            return (lo + (hi - lo) * std::clamp(frac, 0.0, 1.0)) / 1000.0;
        }
        seen += n;
    }
    return maxMs();
}

nlohmann::json Hist::toJson() const {
    return {{"samples", count()},
            {"p50_ms", percentileMs(0.50)},
            {"p95_ms", percentileMs(0.95)},
            {"p99_ms", percentileMs(0.99)},
            {"max_ms", maxMs()},
            {"mean_ms", meanMs()}};
}

const char* apiKindName(ApiKind k) {
    switch (k) {
        case ApiKind::OpenFleetSession: return "openFleetSession";
        case ApiKind::MintManagedPlayerToken: return "mintManagedPlayerToken";
        case ApiKind::CloseFleetSession: return "closeFleetSession";
        case ApiKind::RefreshFleetSession: return "refreshFleetSession";
        case ApiKind::RefreshAppToken: return "refreshAppToken";
        case ApiKind::DeleteManagedPlayer: return "deleteManagedPlayer";
        case ApiKind::BlockPlayer: return "blockPlayer";
        default: return "unknown";
    }
}

const char* apiCauseName(ApiCause c) {
    switch (c) {
        case ApiCause::Setup: return "setup";
        case ApiCause::Teardown: return "teardown";
        case ApiCause::SessionMove: return "session_move";
        case ApiCause::SessionKeep: return "session_keep";
        case ApiCause::TokenRefresh: return "token_refresh";
        case ApiCause::SignInAgain: return "sign_in_again";
        case ApiCause::Crossing: return "crossing";
        case ApiCause::Cleanup: return "cleanup";
        case ApiCause::RulesCheck: return "rules_check";
        default: return "unknown";
    }
}

const char* carryCauseName(CarryCause c) {
    switch (c) {
        case CarryCause::Join: return "join";
        case CarryCause::Crossing: return "crossing";
        case CarryCause::TokenRefresh: return "token_refresh";
        case CarryCause::SignInAgain: return "sign_in_again";
        case CarryCause::Recarry: return "recarry";
        case CarryCause::SessionMove: return "session_move";
        default: return "unknown";
    }
}

const char* moveTriggerName(MoveTrigger t) {
    switch (t) {
        case MoveTrigger::ReconnectCommand: return "reconnect_command";
        case MoveTrigger::SessionReleased: return "session_released";
        case MoveTrigger::SendErrors: return "send_errors";
        case MoveTrigger::ErrorStreak: return "error_streak";
        case MoveTrigger::RxSilence: return "rx_silence";
        default: return "unknown";
    }
}

uint64_t ApiCounters::byKind(ApiKind k) const {
    return sum(calls[static_cast<size_t>(k)]);
}

uint64_t ApiCounters::byCause(ApiCause c) const {
    uint64_t n = 0;
    for (int k = 0; k < K; ++k)
        n += calls[static_cast<size_t>(k)][static_cast<size_t>(c)].load(std::memory_order_relaxed);
    return n;
}

uint64_t ApiCounters::total() const {
    uint64_t n = 0;
    for (int k = 0; k < K; ++k) n += byKind(static_cast<ApiKind>(k));
    return n;
}

void ApiCounters::count(ApiKind k, ApiCause c) {
    calls[static_cast<size_t>(k)][static_cast<size_t>(c)].fetch_add(1, std::memory_order_relaxed);
    if (windowOpen.load(std::memory_order_relaxed)) {
        duringWindow[static_cast<size_t>(k)].fetch_add(1, std::memory_order_relaxed);
        duringWindowByCause[static_cast<size_t>(c)].fetch_add(1, std::memory_order_relaxed);
    }
}

FleetStats::FleetStats(int sessionCount) {
    sessions.reserve(static_cast<size_t>(sessionCount));
    for (int i = 0; i < sessionCount; ++i) sessions.push_back(std::make_unique<SessionIo>());
}

uint64_t sum(const Counter* begin, size_t n) {
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += begin[i].load(std::memory_order_relaxed);
    return total;
}

const char* errorCodeName(int code) {
    switch (code) {
        case 0: return "NO_ERROR";
        case 1: return "UNKNOWN_ERROR";
        case 5: return "INVALID_TOKEN";
        case 6: return "APP_NOT_FOUND";
        case 7: return "UNAUTHORIZED";
        case 13: return "GAME_TOKEN_WRONG_SIZE";
        case 15: return "INVALID_REQUEST";
        case 18: return "INVALID_APP_ID";
        case 20: return "USER_NOT_AUTHENTICATED";
        case 32: return "TOKEN_EXPIRED";
        case 33: return "APP_PAUSED";
        default: return "UNKNOWN";
    }
}

const char* messageTypeName(int type) {
    switch (type) {
        case wire::GENERIC_ERROR_MESSAGE: return "GENERIC_ERROR_MESSAGE";
        case 18: return "CHANNEL_MESSAGE_NOTIFICATION";
        case 36: return "CHANNEL_AUDIO_NOTIFICATION";
        case FLEET_CARRY_RESULT: return "FLEET_CARRY_RESULT";
        case FLEET_RELEASED: return "FLEET_RELEASED";
        case FLEET_DELIVERY: return "FLEET_DELIVERY";
        case wire::ACTOR_UPDATE_NOTIFICATION_2: return "ACTOR_UPDATE_NOTIFICATION";
        case wire::VOXEL_UPDATE_NOTIFICATION_2: return "VOXEL_UPDATE_NOTIFICATION";
        case 135: return "CLIENT_AUDIO_NOTIFICATION";
        case 137: return "CLIENT_TEXT_NOTIFICATION";
        case wire::CLIENT_EVENT_NOTIFICATION_2: return "CLIENT_EVENT_NOTIFICATION";
        case wire::SERVER_EVENT_NOTIFICATION_2: return "SERVER_EVENT_NOTIFICATION";
        case wire::GENERIC_SPATIAL_1: return "GENERIC_SPATIAL_1";
        case wire::SINGLE_ACTOR_MESSAGE: return "SINGLE_ACTOR_MESSAGE";
        case 144: return "CLIENT_VIDEO_NOTIFICATION";
        case 145: return "ACTOR_LEFT_NOTIFICATION";
        case 146: return "CHUNK_CHANGED_NOTIFICATION";
        default: return "OTHER";
    }
}

} // namespace lt::fleet
