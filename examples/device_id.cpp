// Print this machine's Moonbase device id and how it was derived.
//
// Two uses. It is the diagnostic to run when a customer reports "this license is
// not for this device": the output is safe to paste into a support ticket,
// because it names the identity parameters that contributed but never their
// values (those are hardware serial numbers, and an unsalted hash of one is a
// stable global correlator, no safer to publish than the value itself).
//
// It is also how CI proves cross-SDK parity. The device fingerprint spec is only
// worth anything if two SDKs agree on real hardware, so
// .github/workflows/fingerprint-parity.yml runs this next to
// @moonbase.sh/licensing on the same runner and requires the two device ids to
// be identical. Hence the machine-readable output.
//
// Exits 0 even when this machine has no usable identity: that is a legitimate
// answer about the machine, reported as an "error" field, and the parity check
// needs to see it in order to confirm the other SDK reached the same conclusion.

#include <iostream>

#include <nlohmann/json.hpp>

#include <moonbase/moonbase_device_id_resolver.hpp>

int main()
{
    // Report what this machine actually offers, so a host with no hardware
    // identity is visible as such rather than silently downgraded.
    moonbase::moonbase_device_id_resolver resolver;

    nlohmann::json out;
    out["deviceName"] = resolver.device_name();

    try {
        const auto described = resolver.describe_device().value();

        out["deviceId"] = described.device_id;
        out["version"] = described.version;
        out["platform"] = described.platform;
        out["source"] = described.source == moonbase::fingerprint_spec::device_id_source::device_name
            ? "deviceName"
            : "identity";
        out["paramNames"] = described.param_names;
    } catch (const moonbase::insufficient_device_identity_error& ex) {
        out["error"] = "DeviceIdentityUnavailable";
        out["message"] = ex.what();
        out["platform"] = ex.platform();
    }

    std::cout << out.dump(2) << '\n';
    return 0;
}
