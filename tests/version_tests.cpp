#include <doctest/doctest.h>

#include "moonbase/version.hpp"

using namespace moonbase;

TEST_CASE("parse_semver handles common forms")
{
    auto full = parse_semver("2.4.0");
    REQUIRE(full.has_value());
    CHECK(full->major == 2);
    CHECK(full->minor == 4);
    CHECK(full->patch == 0);
    CHECK(full->prerelease.empty());

    auto leading = parse_semver("v1.2.3");
    REQUIRE(leading.has_value());
    CHECK(leading->major == 1);
    CHECK(leading->minor == 2);
    CHECK(leading->patch == 3);

    auto twoPart = parse_semver("2.4");
    REQUIRE(twoPart.has_value());
    CHECK(twoPart->minor == 4);
    CHECK(twoPart->patch == 0);

    auto onePart = parse_semver("3");
    REQUIRE(onePart.has_value());
    CHECK(onePart->major == 3);
    CHECK(onePart->minor == 0);

    auto pre = parse_semver("2.4.0-beta.1");
    REQUIRE(pre.has_value());
    CHECK(pre->prerelease == "beta.1");

    auto build = parse_semver("2.4.0+build.7");
    REQUIRE(build.has_value());
    CHECK(build->prerelease.empty()); // build metadata is dropped

    CHECK_FALSE(parse_semver("").has_value());
    CHECK_FALSE(parse_semver("abc").has_value());
    CHECK_FALSE(parse_semver("1.x.0").has_value());
    CHECK_FALSE(parse_semver("1.2.3.4").has_value()); // too many numeric parts
}

TEST_CASE("compare orders versions per SemVer precedence")
{
    CHECK(compare(*parse_semver("2.4.0"), *parse_semver("2.3.1")) > 0);
    CHECK(compare(*parse_semver("2.3.1"), *parse_semver("2.4.0")) < 0);
    CHECK(compare(*parse_semver("1.0.0"), *parse_semver("1.0.0")) == 0);
    CHECK(compare(*parse_semver("2.4"), *parse_semver("2.4.0")) == 0);
    CHECK(compare(*parse_semver("1.10.0"), *parse_semver("1.9.0")) > 0); // numeric, not lexical

    // A prerelease has lower precedence than the matching release.
    CHECK(compare(*parse_semver("1.0.0-rc.1"), *parse_semver("1.0.0")) < 0);
    CHECK(compare(*parse_semver("1.0.0-alpha"), *parse_semver("1.0.0-beta")) < 0);
    CHECK(compare(*parse_semver("1.0.0-alpha.1"), *parse_semver("1.0.0-alpha")) > 0);
    CHECK(compare(*parse_semver("1.0.0-1"), *parse_semver("1.0.0-alpha")) < 0); // numeric < alnum id
}

TEST_CASE("update_available is true only for a strictly newer, parseable version")
{
    CHECK(update_available("2.3.1", "2.4.0"));
    CHECK(update_available("2.3.1", "3.0.0"));
    CHECK(update_available("1.9.0", "1.10.0"));

    CHECK_FALSE(update_available("2.4.0", "2.4.0"));
    CHECK_FALSE(update_available("2.4.0", "2.3.9"));

    // Never prompt when either side is unparseable.
    CHECK_FALSE(update_available("2.4.0", ""));
    CHECK_FALSE(update_available("", "2.4.0"));
    CHECK_FALSE(update_available("garbage", "2.4.0"));

    // A prerelease of the next version is still newer than the current release...
    CHECK(update_available("2.3.1", "2.4.0-beta.1"));
    // ...but a prerelease of the SAME version is not an update over the release.
    CHECK_FALSE(update_available("2.4.0", "2.4.0-beta.1"));
}
