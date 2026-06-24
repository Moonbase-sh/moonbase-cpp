#pragma once

// HTTP transport for the Moonbase SDK built on juce::WebInputStream, so the
// module needs no CURL. Build JUCE with JUCE_USE_CURL=0 to keep CURL out
// entirely (the transport uses the platform stack: WinHTTP / NSURLSession /
// libcurl-free POSIX backends).

#include <map>
#include <string>

#include <moonbase/moonbase.hpp>

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class juce_http_transport : public moonbase::http_transport
{
public:
    [[nodiscard]] moonbase::http_response send(const moonbase::http_request& request) override
    {
        const bool isPost = (request.method == "POST");

        juce::URL url(juce::String(request.url));
        if (isPost)
            url = url.withPOSTData(juce::String::fromUTF8(request.body.data(),
                                                          static_cast<int>(request.body.size())));

        juce::String extraHeaders;
        for (const auto& [key, value] : request.headers)
            extraHeaders << juce::String(key) << ": " << juce::String(value) << "\r\n";

        // juce::WebInputStream exposes a single connection timeout; the SDK's
        // separate request timeout has no equivalent here.
        auto timeout = request.connect_timeout.count() > 0
                           ? request.connect_timeout
                           : request.request_timeout;
        const int timeoutMs = timeout.count() > 0 ? static_cast<int>(timeout.count()) : 30000;

        // ParameterHandling::inAddress is essential: the SDK bakes its query
        // string (?format=JWT&platform=…) into the URL, which juce::URL parses
        // into parameters. With inPostData (what `WebInputStream(url, true)`
        // uses) JUCE would form-encode those params and prepend them to the POST
        // body, corrupting the JSON/token payload and triggering server 400s.
        // inAddress keeps them in the URL; the body stays exactly our payload.
        juce::StringPairArray responseHeaders;
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders(extraHeaders)
                           .withConnectionTimeoutMs(timeoutMs)
                           .withResponseHeaders(&responseHeaders)
                           .withStatusCode(&statusCode);

        auto stream = url.createInputStream(options);
        if (stream == nullptr)
        {
            // Match curl_http_transport: a transport-level failure surfaces as
            // api_error with status 0, which the SDK's grace-period logic
            // treats as "unreachable".
            throw moonbase::api_error(
                0, "HTTP request to " + request.url + " failed (could not connect)");
        }

        moonbase::http_response response;
        response.body = stream->readEntireStreamAsString().toStdString();
        response.status_code = static_cast<long>(statusCode);
        for (const auto& key : responseHeaders.getAllKeys())
            response.headers[key.toStdString()] = responseHeaders[key].toStdString();
        return response;
    }
};

} // namespace moonbase::juce_integration
