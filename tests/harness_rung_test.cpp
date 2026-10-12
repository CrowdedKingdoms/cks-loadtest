#include "Config.hpp"
#include "Harness.hpp"
#include "Stats.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

std::string atEndIn(const std::filesystem::path& file) {
    std::ifstream f(file);
    auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded()) return "unreadable";
    return j.value("replay_logging_at_end", std::string("missing"));
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / ("lt-rung-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);

    lt::Config config;
    config.statsDir = dir.string();
    config.replayLogging = "leave";
    {
        lt::Harness harness(config);
        check(harness.openRung("r1").empty(), "the first rung opens");
        harness.closeRung();
        check(harness.openRung("r2").empty(), "the last rung opens");
        harness.closeRung();
        check(atEndIn(dir / "rung-r2.json").empty(),
              "a rung file is written before the end reading exists");

        harness.recordReplayLoggingAtEnd("off");
        check(atEndIn(dir / "rung-r2.json") == "off",
              "the last rung file gets the end reading when the run ends");
        check(atEndIn(dir / "rung-r1.json").empty(), "an earlier rung file is left as written");
        check(harness.statusJson().value("replay_logging_at_end", std::string()) == "off",
              "status reports the end reading");
    }
    fs::remove_all(dir);

    // The summary names both readings, and warns when they differ.
    const std::string same = lt::replayLoggingSummary("leave", "on", "on");
    check(same == "replay logging: requested leave; on at the start, on at the end\n",
          "the summary names the request and both readings");
    const std::string toggled = lt::replayLoggingSummary("on", "on", "off");
    check(toggled.find("WARNING") != std::string::npos,
          "a toggle during the run is warned about");
    check(lt::replayLoggingSummary("leave", "unknown", "").find("unknown at the end") !=
              std::string::npos,
          "an end that could not be read says unknown");

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("harness_rung_test: all passed\n");
    return 0;
}
