#pragma once

// HTTP transport for the Moonbase SDK built on juce::WebInputStream, so the
// module brings no HTTP library of its own. WebInputStream uses the platform
// stack (WinINet / NSURLSession / HttpURLConnection on Android), except on Linux,
// where JUCE makes HTTPS requests through libcurl: moonbase_licensing.cpp fails
// the build when JUCE_USE_CURL is off there.

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

        bool connected = false;
        juce::String body;
        int statusCode = 0;
        juce::StringPairArray responseHeaders;
        {
            const in_flight registration(*this, *stream, request.url);
            connected = stream->connect(nullptr);
            if (connected)
            {
                body = stream->readEntireStreamAsString();
                statusCode = stream->getStatusCode();
                responseHeaders = stream->getResponseHeaders();
            }
        }

        if (! connected)
        {
            // Match curl_http_transport: a transport-level failure surfaces as
            // api_error with status 0, which the SDK's grace-period logic treats
            // as "unreachable" (this also covers a cancelled connect()).
            //
            // The friendly message stays user-safe; the macOS sandbox hint goes in
            // the detail field so it reaches developers (via onDiagnostic) without
            // surfacing in the plugin UI. A status-0 connect failure inside a
            // sandboxed Apple build is most often the missing network entitlement.
            juce::String message = "Couldn't connect to " + request.url
                                 + " (check the internet connection or firewall)";
            juce::String detail;
           #if JUCE_MAC || JUCE_IOS
            detail = "If this build runs sandboxed (AUv3 / Mac App Store), enable the "
                     "com.apple.security.network.client entitlement.";
           #endif
            throw moonbase::api_error(0, message.toStdString(), {}, detail.toStdString());
        }

        moonbase::http_response response;
        response.body = body.toStdString();
        response.status_code = static_cast<long>(statusCode);
        for (const auto& key : responseHeaders.getAllKeys())
            response.headers[key.toStdString()] = responseHeaders[key].toStdString();
        return response;
    }

    // Interrupt every in-flight request and refuse further ones. Safe to call
    // from another thread; the controller calls it on teardown so a blocking read
    // cannot outlive the controller (or the plugin binary).
    void cancel()
    {
        const juce::ScopedLock sl(streamLock);
        cancelled = true;
        for (auto* stream : active)
            stream->cancel();
    }

private:
    // Puts a request's stream where cancel() can reach it, from connect() until
    // the read completes, or refuses the request once cancel() has run. The
    // controller's workers share this transport, so several can be in flight.
    class in_flight
    {
    public:
        in_flight(juce_http_transport& transport, juce::WebInputStream& stream, const std::string& url)
            : owner(transport), registered(stream)
        {
            const juce::ScopedLock sl(owner.streamLock);
            if (owner.cancelled)
                throw moonbase::api_error(0, "HTTP request to " + url + " was cancelled");
            owner.active.add(&registered);
        }

        ~in_flight()
        {
            const juce::ScopedLock sl(owner.streamLock);
            owner.active.removeFirstMatchingValue(&registered);
        }

        in_flight(const in_flight&) = delete;
        in_flight& operator=(const in_flight&) = delete;

    private:
        juce_http_transport& owner;
        juce::WebInputStream& registered;
    };

    juce::CriticalSection streamLock;
    juce::Array<juce::WebInputStream*> active;
    bool cancelled = false;
};

} // namespace moonbase::juce_integration
