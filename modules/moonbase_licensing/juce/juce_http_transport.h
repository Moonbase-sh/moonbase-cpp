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

        // addParametersToRequestBody = false is the WebInputStream equivalent of
        // ParameterHandling::inAddress: the SDK bakes its query string
        // (?format=JWT&platform=…) into the URL and the POST body must stay
        // exactly our payload. With `true` JUCE would form-encode those params
        // into the body, corrupting the JSON/token and triggering server 400s.
        //
        // We construct the WebInputStream ourselves (rather than
        // URL::createInputStream) so a torn-down controller can cancel() a
        // blocking read instead of leaving a detached worker running module code
        // after the plugin is destroyed/unloaded (scanning, pluginval).
        auto stream = std::make_unique<juce::WebInputStream>(url, /*addParametersToRequestBody*/ false);
        stream->withExtraHeaders(extraHeaders);
        stream->withConnectionTimeout(timeoutMs);

        {
            const juce::ScopedLock sl(streamLock);
            if (cancelled)
                throw moonbase::api_error(0, "HTTP request to " + request.url + " was cancelled");
            active = stream.get();
        }

        const bool connected = stream->connect(nullptr);

        juce::String body;
        int statusCode = 0;
        juce::StringPairArray responseHeaders;
        if (connected)
        {
            body = stream->readEntireStreamAsString();
            statusCode = stream->getStatusCode();
            responseHeaders = stream->getResponseHeaders();
        }

        {
            const juce::ScopedLock sl(streamLock);
            active = nullptr;
        }

        if (! connected)
        {
            // Match curl_http_transport: a transport-level failure surfaces as
            // api_error with status 0, which the SDK's grace-period logic treats
            // as "unreachable" (this also covers a cancelled connect()).
            throw moonbase::api_error(
                0, "HTTP request to " + request.url + " failed (could not connect)");
        }

        moonbase::http_response response;
        response.body = body.toStdString();
        response.status_code = static_cast<long>(statusCode);
        for (const auto& key : responseHeaders.getAllKeys())
            response.headers[key.toStdString()] = responseHeaders[key].toStdString();
        return response;
    }

    // Interrupt any in-flight request and refuse further ones. Safe to call from
    // another thread; the controller calls it on teardown so a blocking read
    // cannot outlive the controller (or the plugin binary).
    void cancel()
    {
        const juce::ScopedLock sl(streamLock);
        cancelled = true;
        if (active != nullptr)
            active->cancel();
    }

private:
    juce::CriticalSection streamLock;
    juce::WebInputStream* active = nullptr; // valid only between connect() and the read completing
    bool cancelled = false;
};

} // namespace moonbase::juce_integration
