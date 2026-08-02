// Golden vectors for the pre-4.0.0 device id, FROZEN.
//
// These three digests are the only thing standing between an innocent-looking
// tidy-up of legacy_fingerprint.hpp and every license issued under the old
// algorithm failing to validate. legacy_cpp_device_id_resolver exists solely so a
// migrating_device_id_resolver can keep accepting those bindings, which requires
// it to reproduce them exactly, defects and all.
//
// If a change here makes one of these fail, the change is wrong. Do not update
// the expected values.
//
// The current algorithm is covered by tests/fingerprint_spec_tests.cpp against
// the shipped conformance vectors.

#include <doctest/doctest.h>

#include "moonbase/legacy_fingerprint.hpp"

using namespace moonbase;

TEST_CASE("legacy v1 fingerprint hashes structured identity parameters")
{
    CHECK(legacy_cpp_device_id_resolver::hash_identity_parameters(
        {
            {"ioPlatformUuid", "ABC123"},
            {"cpuModel", "M1"},
        },
        "mac") == "0789f32ee2a491fec91d99f8fcdcb62957c84748c53577bb794cd55e89e1828c");

    CHECK(legacy_cpp_device_id_resolver::hash_identity_parameters(
        {
            {"boardSerial", "SERIAL-1"},
            {"cpuVendor", "GenuineIntel"},
        },
        "linux") == "d519035802e134e02a42c31344c80071b4872c5744a787900f8b4d2795630719");
}

TEST_CASE("legacy v1 fingerprint hashing trims empty or padded parameters")
{
    CHECK(legacy_cpp_device_id_resolver::hash_identity_parameters(
        {
            {" boardSerial ", " SERIAL-1\n"},
            {"empty", " \t"},
            {"cpuVendor", "GenuineIntel"},
        },
        "linux") == "d519035802e134e02a42c31344c80071b4872c5744a787900f8b4d2795630719");
}

TEST_CASE("legacy v1 material is LF-terminated, unlike the spec's LF-joined form")
{
    // Pinning the shape, not just the digest, so the reason these digests differ
    // from spec v2 stays visible: v1 ends every line with a newline, including
    // the last, and carries a moonbase-cpp-specific prefix.
    const auto legacy = legacy_cpp_device_id_resolver::hash_identity_parameters(
        {{"ioPlatformUuid", "0123456789ABCDEF0123456789ABCDEF"}}, "mac");

    CHECK(legacy
        == detail::sha256_hex(
               "moonbase-cpp:fingerprint:v1\nplatform=mac\nioPlatformUuid=0123456789ABCDEF0123456789ABCDEF\n"));

    // And it is a bare digest: no mbd2_ stamp, so a validator can tell a v1
    // binding from a spec one.
    CHECK(legacy.size() == 64);
    CHECK(legacy.rfind("mbd", 0) != 0);
}
