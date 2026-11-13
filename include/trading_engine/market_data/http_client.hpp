#pragma once

// Minimal blocking HTTPS GET client used by the Alpaca REST source.
//
// The interface lives in the Alpaca adapter so tests can drive the source with
// recorded responses. make_default_http_client() returns a libcurl client when
// the adapter was built with libcurl, and otherwise a client whose get() throws
// HttpError explaining how to rebuild.

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trading_engine::market_data {

// Transport failure (DNS, TLS, timeout, no libcurl): no HTTP status was received.
class HttpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct HttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
    long status{0};
    std::string body;
};

class IHttpClient {
public:
    virtual ~IHttpClient();
    [[nodiscard]] virtual HttpResponse get(const HttpRequest& request) = 0;
};

[[nodiscard]] std::unique_ptr<IHttpClient> make_default_http_client();

// true when make_default_http_client() can make real requests
[[nodiscard]] bool http_client_available() noexcept;

}  // namespace trading_engine::market_data
