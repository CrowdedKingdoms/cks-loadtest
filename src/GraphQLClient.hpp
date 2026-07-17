#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace lt {

/// Thrown for transport failures, non-2xx responses, and GraphQL errors.
class GraphQLError : public std::runtime_error {
public:
    GraphQLError(std::string message, std::string code, bool transport)
        : std::runtime_error(std::move(message)), code_(std::move(code)),
          transport_(transport) {}

    /// `extensions.code` of the first GraphQL error (e.g. "FORBIDDEN",
    /// "UNAUTHENTICATED"), or empty when unavailable.
    const std::string& code() const { return code_; }
    /// True for network/HTTP-level failures (retryable), false for
    /// GraphQL-level errors returned by the API.
    bool isTransport() const { return transport_; }

private:
    std::string code_;
    bool transport_;
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

    /// POST {query, variables}; optional `Authorization: Bearer <bearer>`.
    /// Returns the `data` object. Throws GraphQLError on any failure.
    nlohmann::json request(const std::string& query,
                           const nlohmann::json& variables,
                           const std::string& bearer = "");

    const std::string& url() const { return url_; }

    /// Process-wide libcurl init/cleanup; call once from main().
    static void globalInit();
    static void globalCleanup();

private:
    std::string url_;
    bool tlsInsecure_;
    void* curl_; // CURL*
};

/// Normalize a user-supplied API base URL to its /graphql endpoint.
std::string graphqlEndpoint(const std::string& baseUrl);

} // namespace lt
