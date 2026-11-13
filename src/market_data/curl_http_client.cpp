#include "trading_engine/market_data/http_client.hpp"

#include <curl/curl.h>

#include <mutex>

namespace trading_engine::market_data {

namespace {

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

class CurlHttpClient final : public IHttpClient {
public:
    CurlHttpClient() {
        static std::once_flag global_init;
        std::call_once(global_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    HttpResponse get(const HttpRequest& request) override {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            throw HttpError("curl_easy_init failed");
        }

        curl_slist* headers = nullptr;
        for (const auto& [name, value] : request.headers) {
            headers = curl_slist_append(headers, (name + ": " + value).c_str());
        }

        HttpResponse response;
        char error[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "trading-engine/1.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        const CURLcode code = curl_easy_perform(curl);
        if (code == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (code != CURLE_OK) {
            throw HttpError(std::string("HTTP request failed: ") +
                            (error[0] != '\0' ? error : curl_easy_strerror(code)));
        }
        return response;
    }
};

}  // namespace

IHttpClient::~IHttpClient() = default;

std::unique_ptr<IHttpClient> make_default_http_client() {
    return std::make_unique<CurlHttpClient>();
}

bool http_client_available() noexcept { return true; }

}  // namespace trading_engine::market_data
