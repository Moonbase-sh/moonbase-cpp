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

    /// Perform one exchange and return the response, whatever its status.
    ///
    /// When no response arrives at all (DNS, a refused connection, TLS, a timeout,
    /// cancellation), throw api_error with status code 0, as both bundled
    /// transports do. That is what tells "Moonbase could not be reached" apart from
    /// a failure on this machine: the JUCE activation screen asks the user to check
    /// their connection for the first, and shows any other exception as it is.
    [[nodiscard]] virtual http_response send(const http_request& request) = 0;
};

} // namespace moonbase
