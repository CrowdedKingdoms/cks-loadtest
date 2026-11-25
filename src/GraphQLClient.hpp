#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lt {

/// Thrown for transport failures, non-2xx responses, and GraphQL errors.
class GraphQLError : public std::runtime_error {
public:
    GraphQLError(std::string message, std::string code, bool transport,
                 int retryAfterMs = -1)
        : std::runtime_error(std::move(message)), code_(std::move(code)),
          transport_(transport), retryAfterMs_(retryAfterMs) {}

    /// `extensions.code` of the first GraphQL error (e.g. "FORBIDDEN",
    /// "UNAUTHENTICATED"), or empty when unavailable.
    const std::string& code() const { return code_; }
    /// True for network/HTTP-level failures (retryable), false for
    /// GraphQL-level errors returned by the API.
    bool isTransport() const { return transport_; }
    /// `extensions.retryAfterMs` of the first GraphQL error (the API sends it
    /// beside PLATFORM_BUSY), or -1 when the error carries none.
    int retryAfterMs() const { return retryAfterMs_; }

private:
    std::string code_;
    bool transport_;
    int retryAfterMs_;
};

/// Minimal blocking GraphQL-over-HTTP client (libcurl). One instance per
/// thread; instances are not thread-safe but are cheap to create.
class GraphQLClient {
public:
    /// `url` is the full GraphQL endpoint (".../graphql" appended if missing).
    GraphQLClient(std::string url, bool tlsInsecure);
    ~GraphQLClient();
    GraphQLClient(const GraphQLClient&) = delete;
    GraphQLClient& operator=(const GraphQLClient&) = delete;

    /// POST {query, variables}; optional `Authorization: Bearer <bearer>` and
    /// extra `Name: value` header lines. Returns the `data` object. Throws
    /// GraphQLError on any failure.
    nlohmann::json request(const std::string& query,
                           const nlohmann::json& variables,
                           const std::string& bearer = "",
                           const std::vector<std::string>& extraHeaders = {});

    const std::string& url() const { return url_; }

    /// How many HTTP requests this instance has sent, including ones that failed.
    /// A caller that counts API calls reads it around a helper that may retry.
    uint64_t requestCount() const { return requestCount_; }

    /// Process-wide libcurl init/cleanup; call once from main().
    static void globalInit();
    static void globalCleanup();

private:
    std::string url_;
    bool tlsInsecure_;
    void* curl_; // CURL*
    uint64_t requestCount_ = 0;
};

/// Normalize a user-supplied API base URL to its /graphql endpoint.
std::string graphqlEndpoint(const std::string& baseUrl);

} // namespace lt
