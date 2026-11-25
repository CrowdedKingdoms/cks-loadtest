#include "fleet/Roster.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace lt::fleet {

namespace {
std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}
} // namespace

RosterLoad loadRoster(const std::string& path, const std::string& api, int64_t appId,
                      Roster& out, std::string& error) {
    out = Roster{};
    out.api = trimSlash(api);
    out.appId = appId;

    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return RosterLoad::Missing;
        error = "cannot read the roster file '" + path + "': " + std::strerror(errno);
        return RosterLoad::Refused;
    }
    std::ifstream f(path);
    if (!f) {
        error = "cannot open the roster file '" + path + "'";
        return RosterLoad::Refused;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception&) {
        // The parser's message can quote the file, and the file holds tokens.
        error = "the roster file '" + path + "' is not valid JSON";
        return RosterLoad::Refused;
    }
    if (!doc.is_object() || doc.value("kind", std::string()) != "fleet-players") {
        error = "the roster file '" + path + "' is not a ck-fleet-load roster";
        return RosterLoad::Refused;
    }
    const std::string theirs = trimSlash(doc.value("api", std::string()));
    if (theirs != out.api) {
        error = "the roster file was minted against '" + theirs + "' and this run targets '" +
                out.api + "'. Use another --roster-file for this API.";
        return RosterLoad::Refused;
    }
    const std::string theirApp = doc.value("appId", std::string());
    if (theirApp != std::to_string(appId)) {
        error = "the roster file was minted for app " + theirApp + " and this run uses app " +
                std::to_string(appId) + ". Use another --roster-file for this app.";
        return RosterLoad::Refused;
    }
    if (doc.contains("players") && doc["players"].is_array()) {
        for (const auto& e : doc["players"]) {
            if (!e.is_object() || !e.contains("n") || !e["n"].is_number_integer()) continue;
            PlayerToken t;
            t.token = e.value("token", std::string());
            try {
                t.gameTokenId = std::stoll(e.value("gameTokenId", std::string("0")));
                t.expiresAtMs = std::stoll(e.value("expiresAtMs", std::string("0")));
            } catch (const std::exception&) {
                continue;
            }
            if (t.token.size() != 64 || t.gameTokenId == 0) continue;
            out.players[e["n"].get<int>()] = std::move(t);
        }
    }
    return RosterLoad::Loaded;
}

bool saveRoster(const std::string& path, const Roster& roster, std::string& error) {
    nlohmann::json players = nlohmann::json::array();
    for (const auto& [n, t] : roster.players) {
        players.push_back({{"n", n},
                           {"externalId", playerExternalId(n)},
                           {"token", t.token},
                           {"gameTokenId", std::to_string(t.gameTokenId)},
                           {"expiresAtMs", std::to_string(t.expiresAtMs)}});
    }
    const nlohmann::json doc = {{"kind", "fleet-players"},
                                {"api", trimSlash(roster.api)},
                                {"appId", std::to_string(roster.appId)},
                                {"players", std::move(players)}};
    const std::string body = doc.dump();

    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    // 0600 at creation, so the tokens are never readable by anyone else, even
    // for the instant before a chmod.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot write the roster file '" + path + "': " + std::strerror(errno);
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0; // a file left by an earlier umask
    size_t done = 0;
    while (ok && done < body.size()) {
        const ssize_t n = ::write(fd, body.data() + done, body.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (ok) ok = ::fsync(fd) == 0;
    const int saved = errno;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot write the roster file '" + path + "': " +
                std::strerror(ok ? errno : saved);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace lt::fleet
