#pragma once

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <curl/curl.h>

#include "moonbase/errors.hpp"
#include "moonbase/http.hpp"

namespace moonbase {

class curl_http_transport : public http_transport {
public:
    curl_http_transport()
    {
        static const auto init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (init_result != CURLE_OK) {
            throw api_error(0, curl_easy_strerror(init_result));
        }
    }

    [[nodiscard]] http_response send(const http_request& request) override
    {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) {
            throw api_error(0, "Could not initialize curl");
        }

        http_response response;
        curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, timeout_milliseconds(request.connect_timeout));
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_milliseconds(request.request_timeout));
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &curl_http_transport::write_body);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &curl_http_transport::write_header);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response.headers);

        if (request.method == "POST") {
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, request.body.size());
        } else if (request.method != "GET") {
            curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
            if (!request.body.empty()) {
                curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
                curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, request.body.size());
            }
        }

        curl_slist* raw_headers = nullptr;
        for (const auto& [key, value] : request.headers) {
            const auto header = key + ": " + value;
            raw_headers = curl_slist_append(raw_headers, header.c_str());
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
        if (headers) {
            curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        }

        const auto result = curl_easy_perform(curl.get());
        if (result != CURLE_OK) {
            throw api_error(0, curl_easy_strerror(result));
        }
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status_code);
        return response;
    }

private:
    static long timeout_milliseconds(std::chrono::milliseconds timeout)
    {
        if (timeout.count() <= 0) {
            return 0L;
        }
        if (timeout.count() > std::numeric_limits<long>::max()) {
            throw api_error(0, "HTTP timeout is too large");
        }
        return static_cast<long>(timeout.count());
    }

    static std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
    {
        const auto total = size * nmemb;
        auto* body = static_cast<std::string*>(userdata);
        body->append(ptr, total);
        return total;
    }

    static std::size_t write_header(char* buffer, std::size_t size, std::size_t nitems, void* userdata)
    {
        const auto total = size * nitems;
        std::string line(buffer, total);
        const auto separator = line.find(':');
        if (separator != std::string::npos) {
            auto key = line.substr(0, separator);
            auto value = line.substr(separator + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
                value.pop_back();
            }
            auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
            (*headers)[std::move(key)] = std::move(value);
        }
        return total;
    }
};

} // namespace moonbase
