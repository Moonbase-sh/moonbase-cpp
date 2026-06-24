#pragma once

#include <chrono>
#include <map>
#include <string>

namespace moonbase {

struct http_request {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::chrono::milliseconds connect_timeout{0};
    std::chrono::milliseconds request_timeout{0};
    std::string body;
};

struct http_response {
    long status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

class http_transport {
public:
    virtual ~http_transport() = default;
    [[nodiscard]] virtual http_response send(const http_request& request) = 0;
};

} // namespace moonbase
