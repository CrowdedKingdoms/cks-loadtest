#include "Stats.hpp"

#include "Wire.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace lt {
namespace {

uint64_t satSub(uint64_t a, uint64_t b) { return a >= b ? a - b : 0; }

int saturatingIntSub(int a, int b) {
    long long d = static_cast<long long>(a) - b;
    if (d < 0) return 0;
    if (d > 2000000000LL) return 2000000000;
    return static_cast<int>(d);
}

const char* errorCodeName(int code) {
    switch (code) {
        case 5: return "INVALID_TOKEN";
        case 6: return "APP_NOT_FOUND";
        case 7: return "UNAUTHORIZED";
        case 13: return "GAME_TOKEN_WRONG_SIZE";
        case 15: return "INVALID_REQUEST";
        case 18: return "INVALID_APP_ID";
        case 20: return "USER_NOT_AUTHENTICATED";
        case 32: return "TOKEN_EXPIRED";
        default: return "UNKNOWN";
    }
}

std::string bucketLabel(int i) {
    if (i <= 0) return "<" + std::to_string(kLatencyBounds[0]) + "ms";
    if (i >= CounterSnap::LATENCY_BUCKETS - 1) {
        return ">=" + std::to_string(kLatencyBounds[CounterSnap::LATENCY_BUCKETS - 2]) +
               "ms";
    }
    return "<" + std::to_string(kLatencyBounds[i]) + "ms";
}

nlohmann::json latencyJson(const CounterSnap& s) {
    nlohmann::json hist = nlohmann::json::array();
    for (int i = 0; i < CounterSnap::LATENCY_BUCKETS; ++i) {
        hist.push_back({{"le_ms", i < CounterSnap::LATENCY_BUCKETS - 1
                                      ? nlohmann::json(kLatencyBounds[i])
                                      : nlohmann::json(nullptr)},
                        {"n", s.latencyHist[static_cast<size_t>(i)]},
                        {"label", bucketLabel(i)}});
    }
    nlohmann::json j = {
        {"samples", s.latencySamples},
        {"sum_ms", s.latencySumMs},
        {"avg_ms", s.latencySamples
                       ? static_cast<double>(s.latencySumMs) / s.latencySamples
                       : 0.0},
        {"max_ms", s.latencyMaxMs},
        {"p50_ms", percentileMs(s, 0.50)},
        {"p95_ms", percentileMs(s, 0.95)},
        {"p99_ms", percentileMs(s, 0.99)},
        {"hist", std::move(hist)},
    };
    return j;
}

nlohmann::json errorsJson(const CounterSnap& s) {
    nlohmann::json arr = nlohmann::json::array();
    for (int c = 0; c < 256; ++c) {
        if (!s.errorCodes[static_cast<size_t>(c)]) continue;
        arr.push_back({{"code", c},
                       {"name", errorCodeName(c)},
                       {"n", s.errorCodes[static_cast<size_t>(c)]}});
    }
    uint64_t unauth = s.errorCodes[7];
    uint64_t unexplained = 0;
    if (unauth > s.unauthorizedFirstContact + s.unauthorizedWindowReload) {
        unexplained = unauth - s.unauthorizedFirstContact - s.unauthorizedWindowReload;
    }
    return {{"by_code", std::move(arr)},
            {"unauthorized_first_contact", s.unauthorizedFirstContact},
            {"unauthorized_window_reload", s.unauthorizedWindowReload},
            {"unauthorized_unexplained", unexplained}};
}

nlohmann::json countersJson(const CounterSnap& s) {
    return {
        {"tx_packets", s.txPackets},
        {"tx_bytes", s.txBytes},
        {"tx_send_errors", s.txSendErrors},
        {"rx_datagrams", s.rxDatagrams},
        {"rx_bytes", s.rxBytes},
        {"rx_bundles", s.rxBundles},
        {"rx_actor_notifications", s.rxActorNotifications},
        {"rx_other_spatial", s.rxOtherSpatial},
        {"rx_error_messages", s.rxErrorMessages},
        {"rx_reconnect_commands", s.rxReconnectCommands},
        {"rx_silent_reassigns", s.rxSilentReassigns},
        {"rx_hmac_failures", s.rxHmacFailures},
        {"rx_malformed", s.rxMalformed},
        {"token_refreshes", s.tokenRefreshes},
        {"reassignments", s.reassignments},
        {"control_failures", s.controlFailures},
        {"latency", latencyJson(s)},
        {"errors", errorsJson(s)},
        {"sign_in",
         {{"reused", s.signInReused},
          {"minted_before", s.signInMintedBefore},
          {"minted_during", s.signInMintedDuring}}},
    };
}

uint64_t jU64(const nlohmann::json& j, const char* k) {
    if (!j.contains(k)) return 0;
    if (j[k].is_number_unsigned()) return j[k].get<uint64_t>();
    if (j[k].is_number_integer()) return static_cast<uint64_t>(j[k].get<int64_t>());
    if (j[k].is_number_float()) return static_cast<uint64_t>(j[k].get<double>());
    return 0;
}

int jInt(const nlohmann::json& j, const char* k) {
    if (!j.contains(k)) return 0;
    if (j[k].is_number_integer()) return j[k].get<int>();
    return 0;
}

} // namespace

double percentileMs(const CounterSnap& snap, double p) {
    if (snap.latencySamples == 0) return 0;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    double target = p * static_cast<double>(snap.latencySamples);
    if (target < 1) target = 1;
    uint64_t cum = 0;
    int64_t lo = 0;
    for (int i = 0; i < CounterSnap::LATENCY_BUCKETS; ++i) {
        uint64_t n = snap.latencyHist[static_cast<size_t>(i)];
        int64_t hi = (i < CounterSnap::LATENCY_BUCKETS - 1)
                         ? kLatencyBounds[i]
                         : kLatencyBounds[CounterSnap::LATENCY_BUCKETS - 2] * 2;
        if (cum + n >= target || i == CounterSnap::LATENCY_BUCKETS - 1) {
            if (n == 0) return static_cast<double>(hi);
            double frac = (target - static_cast<double>(cum)) / static_cast<double>(n);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            return static_cast<double>(lo) + frac * static_cast<double>(hi - lo);
        }
        cum += n;
        lo = hi;
    }
    return static_cast<double>(lo);
}

CounterSnap CounterSnap::minus(const CounterSnap& now, const CounterSnap& origin) {
    CounterSnap d;
    d.txPackets = satSub(now.txPackets, origin.txPackets);
    d.txBytes = satSub(now.txBytes, origin.txBytes);
    d.txSendErrors = satSub(now.txSendErrors, origin.txSendErrors);
    d.rxDatagrams = satSub(now.rxDatagrams, origin.rxDatagrams);
    d.rxBytes = satSub(now.rxBytes, origin.rxBytes);
    d.rxBundles = satSub(now.rxBundles, origin.rxBundles);
    d.rxActorNotifications = satSub(now.rxActorNotifications, origin.rxActorNotifications);
    d.rxOtherSpatial = satSub(now.rxOtherSpatial, origin.rxOtherSpatial);
    d.rxErrorMessages = satSub(now.rxErrorMessages, origin.rxErrorMessages);
    d.rxReconnectCommands = satSub(now.rxReconnectCommands, origin.rxReconnectCommands);
    d.rxSilentReassigns = satSub(now.rxSilentReassigns, origin.rxSilentReassigns);
    d.rxHmacFailures = satSub(now.rxHmacFailures, origin.rxHmacFailures);
    d.rxMalformed = satSub(now.rxMalformed, origin.rxMalformed);
    d.tokenRefreshes = satSub(now.tokenRefreshes, origin.tokenRefreshes);
    d.reassignments = satSub(now.reassignments, origin.reassignments);
    d.controlFailures = satSub(now.controlFailures, origin.controlFailures);
    d.unauthorizedFirstContact =
        satSub(now.unauthorizedFirstContact, origin.unauthorizedFirstContact);
    d.unauthorizedWindowReload =
        satSub(now.unauthorizedWindowReload, origin.unauthorizedWindowReload);
    d.latencySamples = satSub(now.latencySamples, origin.latencySamples);
    d.latencySumMs = satSub(now.latencySumMs, origin.latencySumMs);
    d.latencyMaxMs = now.latencyMaxMs; // caller overwrites with window max
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        d.latencyHist[static_cast<size_t>(i)] = satSub(
            now.latencyHist[static_cast<size_t>(i)], origin.latencyHist[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < 256; ++i) {
        d.errorCodes[static_cast<size_t>(i)] = satSub(
            now.errorCodes[static_cast<size_t>(i)], origin.errorCodes[static_cast<size_t>(i)]);
    }
    d.signInReused = saturatingIntSub(now.signInReused, origin.signInReused);
    d.signInMintedBefore =
        saturatingIntSub(now.signInMintedBefore, origin.signInMintedBefore);
    d.signInMintedDuring =
        saturatingIntSub(now.signInMintedDuring, origin.signInMintedDuring);
    return d;
}

CounterSnap CounterSnap::plus(const CounterSnap& a, const CounterSnap& b) {
    CounterSnap s = a;
    s.txPackets += b.txPackets;
    s.txBytes += b.txBytes;
    s.txSendErrors += b.txSendErrors;
    s.rxDatagrams += b.rxDatagrams;
    s.rxBytes += b.rxBytes;
    s.rxBundles += b.rxBundles;
    s.rxActorNotifications += b.rxActorNotifications;
    s.rxOtherSpatial += b.rxOtherSpatial;
    s.rxErrorMessages += b.rxErrorMessages;
    s.rxReconnectCommands += b.rxReconnectCommands;
    s.rxSilentReassigns += b.rxSilentReassigns;
    s.rxHmacFailures += b.rxHmacFailures;
    s.rxMalformed += b.rxMalformed;
    s.tokenRefreshes += b.tokenRefreshes;
    s.reassignments += b.reassignments;
    s.controlFailures += b.controlFailures;
    s.unauthorizedFirstContact += b.unauthorizedFirstContact;
    s.unauthorizedWindowReload += b.unauthorizedWindowReload;
    s.latencySamples += b.latencySamples;
    s.latencySumMs += b.latencySumMs;
    s.latencyMaxMs = std::max(a.latencyMaxMs, b.latencyMaxMs);
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        s.latencyHist[static_cast<size_t>(i)] += b.latencyHist[static_cast<size_t>(i)];
    }
    for (int i = 0; i < 256; ++i) {
        s.errorCodes[static_cast<size_t>(i)] += b.errorCodes[static_cast<size_t>(i)];
    }
    s.signInReused += b.signInReused;
    s.signInMintedBefore += b.signInMintedBefore;
    s.signInMintedDuring += b.signInMintedDuring;
    return s;
}

nlohmann::json CounterSnap::toJson() const { return countersJson(*this); }

CounterSnap CounterSnap::fromJson(const nlohmann::json& j) {
    CounterSnap s;
    if (j.is_null() || !j.is_object()) return s;
    s.txPackets = jU64(j, "tx_packets");
    s.txBytes = jU64(j, "tx_bytes");
    s.txSendErrors = jU64(j, "tx_send_errors");
    s.rxDatagrams = jU64(j, "rx_datagrams");
    s.rxBytes = jU64(j, "rx_bytes");
    s.rxBundles = jU64(j, "rx_bundles");
    s.rxActorNotifications = jU64(j, "rx_actor_notifications");
    s.rxOtherSpatial = jU64(j, "rx_other_spatial");
    s.rxErrorMessages = jU64(j, "rx_error_messages");
    s.rxReconnectCommands = jU64(j, "rx_reconnect_commands");
    s.rxSilentReassigns = jU64(j, "rx_silent_reassigns");
    s.rxHmacFailures = jU64(j, "rx_hmac_failures");
    s.rxMalformed = jU64(j, "rx_malformed");
    s.tokenRefreshes = jU64(j, "token_refreshes");
    s.reassignments = jU64(j, "reassignments");
    s.controlFailures = jU64(j, "control_failures");
    if (j.contains("latency") && j["latency"].is_object()) {
        const auto& L = j["latency"];
        s.latencySamples = jU64(L, "samples");
        s.latencySumMs = jU64(L, "sum_ms");
        s.latencyMaxMs = jU64(L, "max_ms");
        if (L.contains("hist") && L["hist"].is_array()) {
            size_t i = 0;
            for (const auto& b : L["hist"]) {
                if (i >= s.latencyHist.size()) break;
                s.latencyHist[i++] = jU64(b, "n");
            }
        }
    }
    if (j.contains("errors") && j["errors"].is_object()) {
        const auto& E = j["errors"];
        s.unauthorizedFirstContact = jU64(E, "unauthorized_first_contact");
        s.unauthorizedWindowReload = jU64(E, "unauthorized_window_reload");
        if (E.contains("by_code") && E["by_code"].is_array()) {
            for (const auto& row : E["by_code"]) {
                int code = jInt(row, "code");
                if (code >= 0 && code < 256) s.errorCodes[static_cast<size_t>(code)] = jU64(row, "n");
            }
        }
    }
    if (j.contains("sign_in") && j["sign_in"].is_object()) {
        s.signInReused = jInt(j["sign_in"], "reused");
        s.signInMintedBefore = jInt(j["sign_in"], "minted_before");
        s.signInMintedDuring = jInt(j["sign_in"], "minted_during");
    }
    return s;
}

nlohmann::json buildStatsJson(const SnapshotMeta& meta, const CounterSnap& lifetime,
                              const CounterSnap& window, const IntervalRates& interval) {
    return {
        {"instance_id", meta.instanceId},
        {"index_base", meta.indexBase},
        {"used", meta.used},
        {"limit", meta.limit},
        {"busy", meta.busy},
        {"add_error", meta.addError},
        {"rung_id", meta.rungId},
        {"gauges",
         {{"active", meta.activeClients}, {"suspended", meta.suspendedClients}}},
        {"lifetime", lifetime.toJson()},
        {"window",
         {{"id", meta.rungId},
          {"open_epoch_sec", meta.windowOpenEpochSec},
          {"duration_sec", meta.windowDurationSec},
          {"counters", window.toJson()}}},
        {"interval",
         {{"dt_sec", interval.dtSec},
          {"epoch_sec", interval.epochSec},
          {"tx_pps", interval.txPps},
          {"tx_bps", interval.txBps},
          {"rx_dps", interval.rxDps},
          {"rx_bps", interval.rxBps},
          {"rx_notif_ps", interval.rxNotifPs},
          {"avg_latency_ms", interval.avgLatencyMs}}},
    };
}

nlohmann::json mergeFleetStats(const std::vector<nlohmann::json>& hosts) {
    nlohmann::json out;
    out["hosts"] = hosts.size();
    CounterSnap life, win;
    IntervalRates iv;
    SnapshotMeta meta;
    meta.rungId = "";
    bool first = true;
    std::string disagree;
    for (const auto& h : hosts) {
        CounterSnap L = CounterSnap::fromJson(h.value("lifetime", nlohmann::json::object()));
        CounterSnap W;
        if (h.contains("window") && h["window"].is_object()) {
            W = CounterSnap::fromJson(h["window"].value("counters", nlohmann::json::object()));
            std::string rid = h["window"].value("id", h.value("rung_id", std::string()));
            if (first) meta.rungId = rid;
            else if (rid != meta.rungId) disagree = "rung_id";
            meta.windowDurationSec =
                std::max(meta.windowDurationSec, h["window"].value("duration_sec", 0.0));
            // EARLIEST open, so the fleet window is the union of the hosts' windows
            // rather than whichever host happened to be merged last. Without this the
            // merged blob reported `open_epoch_sec: 0` — it was the one field the loop
            // never carried across — and a rung then had no absolute start time in it.
            // That matters to anything correlating a rung with a monitoring system:
            // duration alone forces the reader to guess the start from the file's mtime,
            // which is the close, and a window guessed backwards from the close of the
            // NEXT rung silently spans this rung's ramp.
            //
            // MIN rather than max, paired with the max above, because the two together
            // bound the interval every host was inside. `rung-open` is one fanned-out
            // HTTP call, so the spread is milliseconds; a host that missed the call
            // entirely reports 0 and is skipped rather than dragging the start to the
            // epoch.
            uint64_t open = h["window"].value("open_epoch_sec", static_cast<uint64_t>(0));
            if (open != 0 && (meta.windowOpenEpochSec == 0 || open < meta.windowOpenEpochSec)) {
                meta.windowOpenEpochSec = open;
            }
        }
        if (first) {
            life = L;
            win = W;
        } else {
            life = CounterSnap::plus(life, L);
            win = CounterSnap::plus(win, W);
        }
        meta.used += h.value("used", 0);
        meta.limit += h.value("limit", 0);
        if (h.contains("gauges")) {
            meta.activeClients += h["gauges"].value("active", 0);
            meta.suspendedClients += h["gauges"].value("suspended", 0);
        }
        if (h.contains("interval") && h["interval"].is_object()) {
            const auto& I = h["interval"];
            iv.dtSec = std::max(iv.dtSec, I.value("dt_sec", 0.0));
            iv.txPps += I.value("tx_pps", 0.0);
            iv.txBps += I.value("tx_bps", 0.0);
            iv.rxDps += I.value("rx_dps", 0.0);
            iv.rxBps += I.value("rx_bps", 0.0);
            iv.rxNotifPs += I.value("rx_notif_ps", 0.0);
            // Do not average avg_latency_ms. Fleet latency is the merged hist.
        }
        if (h.value("busy", false)) meta.busy = true;
        first = false;
    }
    meta.instanceId = "fleet";
    iv.epochSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    out = buildStatsJson(meta, life, win, iv);
    out["hosts"] = hosts.size();
    if (!disagree.empty()) out["warning"] = "hosts disagree on " + disagree;
    return out;
}

void Stats::recordLatencyMs(int64_t ms) {
    if (ms < 0) ms = 0;
    latencySamples.fetch_add(1, std::memory_order_relaxed);
    latencySumMs.fetch_add(static_cast<uint64_t>(ms), std::memory_order_relaxed);
    auto bumpMax = [](std::atomic<uint64_t>& slot, uint64_t v) {
        uint64_t prev = slot.load(std::memory_order_relaxed);
        while (v > prev &&
               !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
        }
    };
    bumpMax(latencyMaxMs, static_cast<uint64_t>(ms));
    bumpMax(windowLatencyMaxMs, static_cast<uint64_t>(ms));
    int bucket = CounterSnap::LATENCY_BUCKETS - 1;
    for (int i = 0; i < CounterSnap::LATENCY_BUCKETS - 1; ++i) {
        if (ms < kLatencyBounds[i]) {
            bucket = i;
            break;
        }
    }
    latencyHist[static_cast<size_t>(bucket)].fetch_add(1, std::memory_order_relaxed);
}

CounterSnap Stats::loadLifetime() const {
    CounterSnap s;
    s.txPackets = txPackets.load(std::memory_order_relaxed);
    s.txBytes = txBytes.load(std::memory_order_relaxed);
    s.txSendErrors = txSendErrors.load(std::memory_order_relaxed);
    s.rxDatagrams = rxDatagrams.load(std::memory_order_relaxed);
    s.rxBytes = rxBytes.load(std::memory_order_relaxed);
    s.rxBundles = rxBundles.load(std::memory_order_relaxed);
    s.rxActorNotifications = rxActorNotifications.load(std::memory_order_relaxed);
    s.rxOtherSpatial = rxOtherSpatial.load(std::memory_order_relaxed);
    s.rxErrorMessages = rxErrorMessages.load(std::memory_order_relaxed);
    s.rxReconnectCommands = rxReconnectCommands.load(std::memory_order_relaxed);
    s.rxHmacFailures = rxHmacFailures.load(std::memory_order_relaxed);
    s.rxMalformed = rxMalformed.load(std::memory_order_relaxed);
    s.tokenRefreshes = tokenRefreshes.load(std::memory_order_relaxed);
    s.reassignments = reassignments.load(std::memory_order_relaxed);
    s.controlFailures = controlFailures.load(std::memory_order_relaxed);
    s.unauthorizedFirstContact = unauthorizedFirstContact.load(std::memory_order_relaxed);
    s.unauthorizedWindowReload = unauthorizedWindowReload.load(std::memory_order_relaxed);
    s.latencySamples = latencySamples.load(std::memory_order_relaxed);
    s.latencySumMs = latencySumMs.load(std::memory_order_relaxed);
    s.latencyMaxMs = latencyMaxMs.load(std::memory_order_relaxed);
    for (int i = 0; i < CounterSnap::LATENCY_BUCKETS; ++i) {
        s.latencyHist[static_cast<size_t>(i)] =
            latencyHist[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < 256; ++i) {
        s.errorCodes[static_cast<size_t>(i)] =
            errorCodes[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(mu_);
    s.signInReused = signInReused_;
    s.signInMintedBefore = signInMintedBefore_;
    s.signInMintedDuring = signInMintedDuring_;
    return s;
}

void Stats::markWindow(std::string id) {
    CounterSnap now = loadLifetime();
    std::lock_guard<std::mutex> lock(mu_);
    windowOrigin_ = now;
    rungId_ = std::move(id);
    windowOpenEpochSec_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    windowOpenSteady_ = std::chrono::steady_clock::now();
    windowLatencyMaxMs.store(0, std::memory_order_relaxed);
}

CounterSnap Stats::loadWindow() const {
    CounterSnap now = loadLifetime();
    std::lock_guard<std::mutex> lock(mu_);
    CounterSnap d = CounterSnap::minus(now, windowOrigin_);
    d.latencyMaxMs = windowLatencyMaxMs.load(std::memory_order_relaxed);
    return d;
}

std::string Stats::rungId() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rungId_;
}

uint64_t Stats::windowOpenEpochSec() const {
    std::lock_guard<std::mutex> lock(mu_);
    return windowOpenEpochSec_;
}

double Stats::windowDurationSec() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (windowOpenSteady_.time_since_epoch().count() == 0) return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         windowOpenSteady_)
        .count();
}

void Stats::setInterval(const IntervalRates& r) {
    std::lock_guard<std::mutex> lock(mu_);
    interval_ = r;
}

IntervalRates Stats::interval() const {
    std::lock_guard<std::mutex> lock(mu_);
    return interval_;
}

void Stats::setSignIn(int reused, int mintedBefore, int mintedDuring) {
    std::lock_guard<std::mutex> lock(mu_);
    signInReused_ = reused;
    signInMintedBefore_ = mintedBefore;
    signInMintedDuring_ = mintedDuring;
}

void Stats::loadSignIn(int& reused, int& mintedBefore, int& mintedDuring) const {
    std::lock_guard<std::mutex> lock(mu_);
    reused = signInReused_;
    mintedBefore = signInMintedBefore_;
    mintedDuring = signInMintedDuring_;
}

StatsReporter::StatsReporter(Stats& stats, int intervalSec, std::string csvPath,
                             std::string jsonlPath, std::string instanceId)
    : stats_(stats), intervalSec_(intervalSec), csvPath_(std::move(csvPath)),
      jsonlPath_(std::move(jsonlPath)), instanceId_(std::move(instanceId)) {}

StatsReporter::~StatsReporter() { stop(); }

void StatsReporter::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void StatsReporter::stop() {
    bool was = running_.exchange(false);
    if (was && thread_.joinable()) thread_.join();
}

void StatsReporter::run() {
    using namespace std::chrono;

    std::ofstream csv;
    if (!csvPath_.empty()) {
        bool writeHeader = true;
        {
            std::ifstream probe(csvPath_);
            writeHeader = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
        }
        csv.open(csvPath_, std::ios::app);
        if (csv && writeHeader) {
            csv << "epoch_sec,instance_id,rung_id,active_clients,suspended_clients,"
                   "tx_pps,tx_bps,rx_dps,rx_bps,rx_notif_ps,window_tx_packets,"
                   "window_rx_notifs,rx_errors_total,reconnects_total,"
                   "hmac_failures_total,avg_latency_ms,p50_ms,p95_ms,p99_ms\n";
        }
    }
    std::ofstream jsonl;
    if (!jsonlPath_.empty()) {
        jsonl.open(jsonlPath_, std::ios::app);
    }

    CounterSnap prev = stats_.loadLifetime();
    auto prevTime = steady_clock::now();

    while (running_.load()) {
        for (int i = 0; i < intervalSec_ * 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        if (!running_.load()) break;

        CounterSnap cur = stats_.loadLifetime();
        auto now = steady_clock::now();
        double dt = duration_cast<duration<double>>(now - prevTime).count();
        if (dt <= 0) dt = 1;

        IntervalRates iv;
        iv.dtSec = dt;
        iv.epochSec = static_cast<uint64_t>(
            duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
        iv.txPps = static_cast<double>(satSub(cur.txPackets, prev.txPackets)) / dt;
        iv.txBps = static_cast<double>(satSub(cur.txBytes, prev.txBytes)) / dt;
        iv.rxDps = static_cast<double>(satSub(cur.rxDatagrams, prev.rxDatagrams)) / dt;
        iv.rxBps = static_cast<double>(satSub(cur.rxBytes, prev.rxBytes)) / dt;
        iv.rxNotifPs =
            static_cast<double>(satSub(cur.rxActorNotifications, prev.rxActorNotifications)) /
            dt;
        uint64_t dLatSamples = satSub(cur.latencySamples, prev.latencySamples);
        uint64_t dLatSum = satSub(cur.latencySumMs, prev.latencySumMs);
        iv.avgLatencyMs = dLatSamples ? static_cast<double>(dLatSum) / dLatSamples : 0.0;
        stats_.setInterval(iv);

        int active = stats_.activeClients.load(std::memory_order_relaxed);
        int suspended = stats_.suspendedClients.load(std::memory_order_relaxed);
        CounterSnap win = stats_.loadWindow();

        std::printf(
            "[stats] clients=%d/%d tx=%.0f pps %.1f KB/s | rx=%.0f dps %.1f KB/s "
            "notif=%.0f/s | lat~%.1fms p99=%.1fms | errs=%" PRIu64
            " reconnects=%" PRIu64 " hmac_fail=%" PRIu64 " send_fail=%" PRIu64 "\n",
            active, active + suspended, iv.txPps, iv.txBps / 1024.0, iv.rxDps,
            iv.rxBps / 1024.0, iv.rxNotifPs, iv.avgLatencyMs, percentileMs(win, 0.99),
            cur.rxErrorMessages, cur.rxReconnectCommands, cur.rxHmacFailures,
            cur.txSendErrors);
        std::fflush(stdout);

        std::string rid = stats_.rungId();
        if (csv) {
            csv << iv.epochSec << ',' << instanceId_ << ',' << rid << ',' << active
                << ',' << suspended << ',' << static_cast<uint64_t>(iv.txPps) << ','
                << static_cast<uint64_t>(iv.txBps) << ','
                << static_cast<uint64_t>(iv.rxDps) << ','
                << static_cast<uint64_t>(iv.rxBps) << ','
                << static_cast<uint64_t>(iv.rxNotifPs) << ',' << win.txPackets << ','
                << win.rxActorNotifications << ',' << cur.rxErrorMessages << ','
                << cur.rxReconnectCommands << ',' << cur.rxHmacFailures << ','
                << iv.avgLatencyMs << ',' << percentileMs(win, 0.50) << ','
                << percentileMs(win, 0.95) << ',' << percentileMs(win, 0.99) << '\n';
            csv.flush();
        }
        if (jsonl) {
            SnapshotMeta meta;
            meta.instanceId = instanceId_;
            meta.rungId = rid;
            meta.activeClients = active;
            meta.suspendedClients = suspended;
            meta.windowOpenEpochSec = stats_.windowOpenEpochSec();
            meta.windowDurationSec = stats_.windowDurationSec();
            jsonl << buildStatsJson(meta, cur, win, iv).dump() << '\n';
            jsonl.flush();
        }

        prev = cur;
        prevTime = now;
    }
}

void StatsReporter::printFinalSummary(int permWindowRadiusChunks, int reused,
                                      int mintedBefore, int mintedDuring) const {
    CounterSnap s = stats_.loadLifetime();
    std::printf("\n===== load test summary =====\n");
    if (mintedDuring >= 0) {
        std::printf("sign-in: %d session(s) minted DURING the run, "
                    "%d before the ramp, %d reused from the roster\n",
                    mintedDuring, mintedBefore, reused);
        if (mintedDuring > 0) {
            std::printf("  WARNING: a session minted during the run costs "
                        "0.4-1.4s of API bcrypt, so the steady-state numbers "
                        "below include it.\n");
        }
    }
    std::printf("tx: %" PRIu64 " packets, %" PRIu64 " bytes (%" PRIu64 " send errors)\n",
                s.txPackets, s.txBytes, s.txSendErrors);
    std::printf("rx: %" PRIu64 " datagrams, %" PRIu64 " bytes, %" PRIu64
                " bundles, %" PRIu64 " actor notifications\n",
                s.rxDatagrams, s.rxBytes, s.rxBundles, s.rxActorNotifications);
    std::printf("control: %" PRIu64 " token refreshes, %" PRIu64
                " reassignments, %" PRIu64 " failures\n",
                s.tokenRefreshes, s.reassignments, s.controlFailures);
    if (s.rxHmacFailures) {
        std::printf("HMAC verification failures: %" PRIu64 "\n", s.rxHmacFailures);
    }

    if (s.latencySamples) {
        std::printf("one-way latency estimate (server epoch vs local clock; "
                    "includes clock skew):\n");
        std::printf("  samples=%" PRIu64 " avg=%.1fms max=%" PRIu64
                    "ms  p50=%.1f p95=%.1f p99=%.1f\n",
                    s.latencySamples,
                    static_cast<double>(s.latencySumMs) / s.latencySamples, s.latencyMaxMs,
                    percentileMs(s, 0.50), percentileMs(s, 0.95), percentileMs(s, 0.99));
        for (int i = 0; i < CounterSnap::LATENCY_BUCKETS; ++i) {
            uint64_t n = s.latencyHist[static_cast<size_t>(i)];
            if (!n) continue;
            std::printf("  %-10s %" PRIu64 " (%.1f%%)\n", bucketLabel(i).c_str(), n,
                        100.0 * static_cast<double>(n) /
                            static_cast<double>(s.latencySamples));
        }
    }

    uint64_t firstContact = s.unauthorizedFirstContact;
    uint64_t windowReload = s.unauthorizedWindowReload;
    bool anyErr = false;
    for (int c = 0; c < 256; ++c) {
        uint64_t n = s.errorCodes[static_cast<size_t>(c)];
        if (!n) continue;
        if (!anyErr) {
            std::printf("server error messages by code:\n");
            anyErr = true;
        }
        std::printf("  code %d (%s): %" PRIu64 "\n", c, errorCodeName(c), n);
    }
    if (firstContact) {
        uint64_t unauth = s.errorCodes[7];
        std::printf("  of which EXPECTED: %" PRIu64
                    " on first contact with a Buddy, within 2s of an "
                    "assignment. Buddy loads the permission window lazily and "
                    "the client's own first packet triggers it, so roughly one "
                    "per client is normal and not a fault.\n",
                    firstContact);
        if (windowReload) {
            std::printf("  of which EXPECTED: %" PRIu64
                        " on crossing out of the server's cached "
                        "grid-permission box (assumed radius %d chunks). The "
                        "same lazy load as first contact: the box is centred "
                        "where the last lookup ran, so a walking client leaves "
                        "it and the packet crossing the edge is refused while "
                        "the re-query is in flight. Onset scales as 1/walk "
                        "speed, so it is geometry rather than a timeout -- "
                        "expect MORE of these the faster or further your "
                        "clients walk, and none at all if they stand still.\n",
                        windowReload, permWindowRadiusChunks);
        }
        if (unauth > firstContact + windowReload) {
            std::printf("  UNEXPLAINED: %" PRIu64
                        " UNAUTHORIZED arrived while the client was neither "
                        "newly assigned NOR outside the modelled permission "
                        "box. These are the ones worth chasing: check the "
                        "granted tier carries runtime permissions, and a steady "
                        "stream means sessions are wedged. If the count is "
                        "large and your clients roam far, check "
                        "LT_PERMISSION_WINDOW_RADIUS_CHUNKS matches the "
                        "server's before reading it as a fault.\n",
                        unauth - firstContact - windowReload);
        }
    }
    std::printf("=============================\n");
    std::fflush(stdout);
}

} // namespace lt
