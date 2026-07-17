#include "GraphQLClient.hpp"

#include <curl/curl.h>

namespace lt {

namespace {
size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

std::string graphqlEndpoint(const std::string& baseUrl) {
    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 8 || url.compare(url.size() - 8, 8, "/graphql") != 0) {
        url += "/graphql";
    }
    return url;
}

void GraphQLClient::globalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void GraphQLClient::globalCleanup() { curl_global_cleanup(); }

GraphQLClient::GraphQLClient(std::string url, bool tlsInsecure)
    : url_(graphqlEndpoint(url)), tlsInsecure_(tlsInsecure),
      curl_(curl_easy_init()) {
    if (!curl_) throw GraphQLError("curl_easy_init failed", "", true);
}

GraphQLClient::~GraphQLClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

nlohmann::json GraphQLClient::request(const std::string& query,
                                      const nlohmann::json& variables,
                                      const std::string& bearer) {
    CURL* curl = static_cast<CURL*>(curl_);
    curl_easy_reset(curl);

    nlohmann::json body = {{"query", query}, {"variables", variables}};
    std::string payload = body.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string authHeader;
    if (!bearer.empty()) {
        authHeader = "Authorization: Bearer " + bearer;
        headers = curl_slist_append(headers, authHeader.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (tlsInsecure_) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode rc = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        throw GraphQLError(std::string("HTTP request to ") + url_ +
                               " failed: " + curl_easy_strerror(rc),
                           "", true);
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response);
    } catch (const std::exception&) {
        throw GraphQLError("non-JSON response (HTTP " + std::to_string(httpCode) +
                               ") from " + url_ + ": " + response.substr(0, 300),
                           "", true);
    }

    if (parsed.contains("errors") && parsed["errors"].is_array() &&
        !parsed["errors"].empty()) {
        const auto& first = parsed["errors"][0];
        std::string message = first.value("message", "unknown GraphQL error");
        std::string code;
        if (first.contains("extensions") && first["extensions"].is_object()) {
            code = first["extensions"].value("code", "");
        }
        throw GraphQLError(message, code, false);
    }
    if (httpCode < 200 || httpCode >= 300) {
        throw GraphQLError("HTTP " + std::to_string(httpCode) + " from " + url_ +
                               ": " + response.substr(0, 300),
                           "", true);
    }
    if (!parsed.contains("data") || parsed["data"].is_null()) {
        throw GraphQLError("GraphQL response missing data from " + url_, "", true);
    }
    return parsed["data"];
}

} // namespace lt
