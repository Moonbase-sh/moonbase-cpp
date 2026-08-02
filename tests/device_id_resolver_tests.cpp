// The migration machinery: recognising ids this device used to have, and telling
// a stale binding apart from an out-of-date SDK.

#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "moonbase/device_id_resolver.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint_spec.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/validator.hpp"
#include "test_helpers.hpp"

namespace fp = moonbase::fingerprint_spec;

namespace {

/// A historical resolver that cannot read this machine any more.
class throwing_resolver : public moonbase::device_id_resolver {
public:
    [[nodiscard]] std::string device_name() const override { return "unreadable"; }
    [[nodiscard]] std::string device_id() const override
    {
        throw moonbase::insufficient_device_identity_error("linux");
    }
};

/// Counts how often its id is computed, to pin the laziness and the memoization.
class counting_resolver : public moonbase::device_id_resolver {
public:
    explicit counting_resolver(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] std::string device_name() const override { return "counted"; }
    [[nodiscard]] std::string device_id() const override
    {
        ++calls;
        return id_;
    }

    mutable int calls = 0;

private:
    std::string id_;
};

std::shared_ptr<moonbase::device_id_resolver> fixed(std::string name, std::string id)
{
    return std::make_shared<moonbase::static_device_id_resolver>(std::move(name), std::move(id));
}

const std::string spec_id = "mbd2_" + std::string(64, 'a');
const std::string legacy_id = std::string(64, 'b');

} // namespace

TEST_CASE("migrating resolver binds the current id and accepts historical ones")
{
    const moonbase::migrating_device_id_resolver resolver(
        fixed("Studio Mac", spec_id), fixed("Studio Mac", legacy_id));

    // Every new activation must bind the current algorithm, or the fleet never
    // finishes migrating.
    CHECK(resolver.device_id() == spec_id);
    CHECK(resolver.device_name() == "Studio Mac");

    CHECK(resolver.accepts_device_id(legacy_id));
    CHECK(!resolver.accepts_device_id("mbd2_" + std::string(64, 'c')));

    SUBCASE("an empty historical id never matches")
    {
        // A historical resolver that could not read anything reduces to "". If
        // that matched, a machine with no identity would accept a license bound
        // to another such machine.
        const moonbase::migrating_device_id_resolver blank(
            fixed("Studio Mac", spec_id), fixed("Studio Mac", ""));
        CHECK(!blank.accepts_device_id(""));
    }
}

TEST_CASE("historical ids are computed lazily, once, and a throwing one is skipped")
{
    auto counted = std::make_shared<counting_resolver>(legacy_id);
    const moonbase::migrating_device_id_resolver resolver(
        fixed("Studio Mac", spec_id),
        std::vector<std::shared_ptr<moonbase::device_id_resolver>>{
            std::make_shared<throwing_resolver>(), counted});

    // The happy path must not pay for the migration.
    CHECK(resolver.device_id() == spec_id);
    CHECK(counted->calls == 0);

    CHECK(resolver.accepts_device_id(legacy_id));
    CHECK(counted->calls == 1);

    // Memoized afterwards, including across a rejection.
    CHECK(!resolver.accepts_device_id("something-else"));
    CHECK(resolver.accepts_device_id(legacy_id));
    CHECK(counted->calls == 1);
}

TEST_CASE("migrating resolver forwards diagnostics and requires a current resolver")
{
    moonbase::moonbase_device_id_resolver_options options;
    options.platform = "linux";
    options.reader = []() -> moonbase::device_identity {
        return {{{"machineId", "b08dfa6083e7567a1921a715000001fb"}}, "host-1"};
    };
    auto current = std::make_shared<moonbase::moonbase_device_id_resolver>(options);

    const moonbase::migrating_device_id_resolver resolver(current, fixed("host-1", legacy_id));
    const auto described = resolver.describe_device();
    REQUIRE(described.has_value());
    CHECK(described->platform == "linux");
    CHECK(described->param_names == std::vector<std::string>{"machineId"});

    // A plain resolver has no history to report, so the base default applies.
    CHECK(!fixed("x", "y")->describe_device().has_value());
    CHECK(!fixed("x", "y")->accepts_device_id("y"));

    CHECK_THROWS_AS(
        moonbase::migrating_device_id_resolver(nullptr, fixed("x", "y")), moonbase::configuration_error);
}

TEST_CASE("a version difference is reported without claiming machine continuity")
{
    using moonbase::detail::describe_stamp_difference;

    const auto v2 = "mbd2_" + std::string(64, 'a');
    const auto v1 = "mbd1_" + std::string(64, 'b');
    const auto v7 = "mbd7_" + std::string(64, 'b');

    SUBCASE("same version says nothing about versions")
    {
        CHECK(describe_stamp_difference(v2, "mbd2_" + std::string(64, 'e')).empty());
    }

    SUBCASE("a stamp-shaped id that is not a valid stamp counts as unstamped")
    {
        // Uppercase hex, a short digest and a stray character all fail to parse,
        // and an unparseable id is compared literally rather than coerced.
        for (const auto bound : {"mbd2_" + std::string(64, 'z'),
                                 "mbd2_" + std::string(64, 'A'),
                                 "mbd2_" + std::string(63, 'a')}) {
            INFO("bound: " << bound);
            CHECK(
                describe_stamp_difference(v2, bound).find("predating versioned device fingerprints")
                != std::string::npos);
        }
    }

    SUBCASE("a scoped mismatch names the correct side, in both directions")
    {
        // The one case where the version path's "re-activate to find out" phrasing
        // would be actively false: a scoped id cannot be compared with anything from
        // another scope, in either direction, even on the same device.
        const auto scoped = "mbd2s_" + std::string(64, 'c');

        const auto bound_is_scoped = describe_stamp_difference(v2, scoped);
        CHECK(bound_is_scoped.find("The binding uses an app-scoped") != std::string::npos);
        CHECK(bound_is_scoped.find("may instead be the same machine") == std::string::npos);

        // Reachable through a custom resolver or a native bridge. Saying "the
        // binding is scoped" here would describe the wrong id and give the wrong
        // remedy.
        const auto we_are_scoped = describe_stamp_difference(scoped, v2);
        CHECK(we_are_scoped.find("This SDK computes an app-scoped") != std::string::npos);
        CHECK(we_are_scoped.find("bind this app") != std::string::npos);
    }

    SUBCASE("an unrecognised tag says update, not re-activate")
    {
        // Before the parser accepted arbitrary tags this fell through to the
        // version branch and was reported as predating versioned fingerprints,
        // which was exactly backwards: it can only come from a *newer* SDK.
        const auto note = describe_stamp_difference(v2, "mbd2x_" + std::string(64, 'c'));
        CHECK(note.find("\"x\"") != std::string::npos);
        CHECK(note.find("newer Moonbase SDK") != std::string::npos);
        CHECK(note.find("predating") == std::string::npos);
    }

    SUBCASE("hardware versus host-name fallback, in both directions")
    {
        const auto fallback = "mbd2n_" + std::string(64, 'c');
        const auto hardware = "mbd2_" + std::string(64, 'c');

        CHECK(describe_stamp_difference(v2, fallback).find("host-name fallback, while this SDK reads")
            != std::string::npos);
        CHECK(describe_stamp_difference("mbd2n_" + std::string(64, 'a'), hardware)
                  .find("fallen back to the host name")
            != std::string::npos);
    }

    SUBCASE("an older binding suggests migrating, conditionally")
    {
        const auto note = describe_stamp_difference(v2, v1);
        CHECK(note.find("v1") != std::string::npos);
        // Must stay hypothetical: a token copied from another computer carries
        // exactly the same version relationship as one made here by an older SDK.
        CHECK(note.find("may instead be the same machine") != std::string::npos);
        CHECK(note.find("migrating_device_id_resolver") != std::string::npos);
    }

    SUBCASE("an unstamped binding predates versioned fingerprints")
    {
        const auto note = describe_stamp_difference(v2, std::string(64, 'b'));
        CHECK(note.find("predating versioned device fingerprints") != std::string::npos);
    }

    SUBCASE("a newer binding says update the SDK, not re-activate")
    {
        const auto note = describe_stamp_difference(v2, v7);
        CHECK(note.find("newer") != std::string::npos);
        CHECK(note.find("Update the SDK") != std::string::npos);
        CHECK(note.find("re-activate to find out") == std::string::npos);
    }

    SUBCASE("a custom resolver's id is compared literally, with no version talk")
    {
        CHECK(describe_stamp_difference("my-own-device-id", v1).empty());
    }
}

TEST_CASE("a device mismatch is its own error type but still a license_invalid_error")
{
    const auto error = moonbase::detail::device_mismatch_error(
        "mbd2_" + std::string(64, 'a'), "mbd1_" + std::string(64, 'b'));

    CHECK(error.type() == moonbase::error_type::license_device_mismatch);
    CHECK(std::string(error.what()).find("not for this device") != std::string::npos);

    // Deriving from license_invalid_error is load-bearing twice over: existing
    // catch sites keep working across the upgrade, and licensing's offline grace
    // period keys off that type, so a mismatch that escaped it would let a
    // license copied from another machine run for the whole grace window.
    try {
        throw error;
    } catch (const moonbase::license_invalid_error& caught) {
        CHECK(caught.type() == moonbase::error_type::license_device_mismatch);
    }
}

TEST_CASE("the validator accepts a historical binding but rejects a foreign one")
{
    const auto key = moonbase::tests::generate_key();
    moonbase::licensing_options options;
    options.endpoint = "https://demo.moonbase.sh";
    options.product_id = "demo-app";
    options.account_id = "tenant-1";
    options.public_key = key.public_pem;
    options.target_platform = moonbase::platform::unknown;

    const auto legacy_bound_token =
        moonbase::tests::make_token(key.key.get(), moonbase::tests::default_claims(legacy_id));

    SUBCASE("without a migration configured, the mismatch is reported with the version note")
    {
        const moonbase::license_validator validator(options, fixed("Studio Mac", spec_id));
        try {
            (void)validator.validate_token(legacy_bound_token);
            FAIL("expected a device mismatch");
        } catch (const moonbase::license_device_mismatch_error& ex) {
            CHECK(ex.type() == moonbase::error_type::license_device_mismatch);
            CHECK(std::string(ex.what()).find("predating versioned device fingerprints")
                != std::string::npos);
        }
    }

    SUBCASE("with a migrating resolver, the same token validates")
    {
        auto resolver = std::make_shared<moonbase::migrating_device_id_resolver>(
            fixed("Studio Mac", spec_id), fixed("Studio Mac", legacy_id));
        const moonbase::license_validator validator(options, resolver);

        const auto license = validator.validate_token(legacy_bound_token);
        CHECK(license.id == "license-123");
    }

    SUBCASE("a token bound to some other machine is still rejected")
    {
        auto resolver = std::make_shared<moonbase::migrating_device_id_resolver>(
            fixed("Studio Mac", spec_id), fixed("Studio Mac", legacy_id));
        const moonbase::license_validator validator(options, resolver);

        const auto foreign = moonbase::tests::make_token(
            key.key.get(), moonbase::tests::default_claims("mbd2_" + std::string(64, 'f')));
        CHECK_THROWS_AS(validator.validate_token(foreign), moonbase::license_device_mismatch_error);
    }
}
