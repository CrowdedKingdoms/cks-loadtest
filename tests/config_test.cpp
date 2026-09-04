#include "Config.hpp"

#include <cstdio>
#include <string>

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

} // namespace

int main() {
    lt::Config c;
    c.email = "alice@studio.example";
    c.emailPattern = "{local}+lt-{index}@{domain}";
    c.indexWidth = 4;
    check(c.derivedEmail(0) == "alice+lt-0000@studio.example", "width 4 index 0");
    check(c.derivedEmail(12) == "alice+lt-0012@studio.example", "width 4 index 12");
    check(c.formatIndex(12) == "0012", "formatIndex 4");

    c.indexWidth = 8;
    check(c.derivedEmail(10000) == "alice+lt-00010000@studio.example",
          "width 8 index 10000");
    check(c.derivedEmail(7) == "alice+lt-00000007@studio.example", "width 8 index 7");

    auto loop = lt::parseControlBind("127.0.0.1:9109");
    check(loop.loopback && loop.port == 9109 && !loop.disabled && !loop.wildcard,
          "loopback bind");
    auto wild = lt::parseControlBind("0.0.0.0:9109");
    check(wild.wildcard && !wild.loopback && wild.port == 9109, "wildcard bind");
    auto off = lt::parseControlBind("off");
    check(off.disabled, "bind off");

    c.email = "bots@example.invalid";
    c.password = "password1";
    c.managementApiUrl = "http://127.0.0.1:3000";
    c.appId = 1;
    c.clients = 0;
    c.indexLimit = 1000;
    c.indexBase = 500;
    c.controlBind = "127.0.0.1:9109";
    c.threads = 2;
    check(c.validate().empty(), "fleet idle generator validates");

    c.controlBind = "0.0.0.0:9109";
    c.controlToken.clear();
    check(!c.validate().empty(), "wildcard without token is refused");
    c.controlToken = "secret";
    check(c.validate().empty(), "wildcard with token is ok");

    // The orphan window is off by default (a lone client legitimately hears
    // nothing) and refuses a negative value; 30 is what the fleet ladder sets.
    check(c.rxSilentReassignSec == 0, "rx-silent reassign is off by default");
    c.rxSilentReassignSec = -1;
    check(!c.validate().empty(), "negative rx-silent window is refused");
    c.rxSilentReassignSec = 30;
    check(c.validate().empty(), "rx-silent window of 30 validates");
    c.rxSilentReassignSec = 0;

    c.controlBind = "off";
    c.clients = 0;
    check(!c.validate().empty(), "zero clients and no control is refused");

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all ok\n");
    return 0;
}
