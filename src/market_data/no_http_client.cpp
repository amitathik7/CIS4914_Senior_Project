#include "trading_engine/market_data/http_client.hpp"

// Compiled instead of curl_http_client.cpp when CMake found no libcurl.

namespace trading_engine::market_data {

namespace {

class UnavailableHttpClient final : public IHttpClient {
public:
    HttpResponse get(const HttpRequest&) override {
        throw HttpError(
            "this build has no HTTP client: install libcurl (brew install curl / "
            "apt install libcurl4-openssl-dev / vcpkg install curl) and reconfigure");
    }
};

}  // namespace

IHttpClient::~IHttpClient() = default;

std::unique_ptr<IHttpClient> make_default_http_client() {
    return std::make_unique<UnavailableHttpClient>();
}

bool http_client_available() noexcept { return false; }

}  // namespace trading_engine::market_data
