#include <doctest/doctest.h>

#include "moonbase/fingerprint.hpp"

using namespace moonbase;

TEST_CASE("default fingerprint hashes structured identity parameters")
{
    CHECK(default_fingerprint_provider::hash_identity_parameters(
        {
            {"ioPlatformUuid", "ABC123"},
            {"cpuModel", "M1"},
        },
        "mac") == "0789f32ee2a491fec91d99f8fcdcb62957c84748c53577bb794cd55e89e1828c");

    CHECK(default_fingerprint_provider::hash_identity_parameters(
        {
            {"boardSerial", "SERIAL-1"},
            {"cpuVendor", "GenuineIntel"},
        },
        "linux") == "d519035802e134e02a42c31344c80071b4872c5744a787900f8b4d2795630719");
}

TEST_CASE("default fingerprint hashing trims empty or padded parameters")
{
    CHECK(default_fingerprint_provider::hash_identity_parameters(
        {
            {" boardSerial ", " SERIAL-1\n"},
            {"empty", " \t"},
            {"cpuVendor", "GenuineIntel"},
        },
        "linux") == "d519035802e134e02a42c31344c80071b4872c5744a787900f8b4d2795630719");
}
