#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "settings.h"

#include <string>

#ifdef __APPLE__
// Stub for src/platform/macos.mm symbol so tests don't link the platform layer.
std::string macosComputerName() { return "test-host"; }
#endif

TEST_CASE("setDeviceName trims leading and trailing whitespace") {
    Settings s;
    s.setDeviceName("  foo  ");
    CHECK(s.deviceName() == "foo");

    s.setDeviceName("\t\nfoo\r\n");
    CHECK(s.deviceName() == "foo");
}

TEST_CASE("setDeviceName collapses internal whitespace runs to a single space") {
    Settings s;
    s.setDeviceName("foo  bar");
    CHECK(s.deviceName() == "foo bar");

    s.setDeviceName("foo\t\tbar");
    CHECK(s.deviceName() == "foo bar");

    s.setDeviceName("foo \t\nbar   baz");
    CHECK(s.deviceName() == "foo bar baz");
}

TEST_CASE("setDeviceName treats whitespace-only input as empty") {
    Settings s;
    s.setDeviceName("   \t\n  ");
    CHECK(s.deviceName().empty());
}

TEST_CASE("setDeviceName preserves single internal spaces") {
    Settings s;
    s.setDeviceName("Andrew's MacBook Pro");
    CHECK(s.deviceName() == "Andrew's MacBook Pro");
}

TEST_CASE("setDeviceName clamps to 64 chars (server's DeviceName column limit)") {
    Settings s;
    std::string long_name(100, 'x');
    s.setDeviceName(long_name);
    CHECK(s.deviceName().size() == 64);
    CHECK(s.deviceName() == std::string(64, 'x'));
}

TEST_CASE("setDeviceName clamps after whitespace normalization") {
    Settings s;
    std::string padded = "  " + std::string(70, 'x') + "  ";
    s.setDeviceName(padded);
    CHECK(s.deviceName().size() == 64);
}

TEST_CASE("setDeviceName clears override when value equals platform default") {
    Settings s;
    s.setDeviceName("custom");
    REQUIRE(s.deviceName() == "custom");

    s.setDeviceName(Settings::platformDeviceName());
    CHECK(s.deviceName().empty());
}

TEST_CASE("setDeviceName clears override when whitespace-padded default is supplied") {
    Settings s;
    s.setDeviceName("custom");
    REQUIRE(s.deviceName() == "custom");

    s.setDeviceName("  " + Settings::platformDeviceName() + "  ");
    CHECK(s.deviceName().empty());
}

TEST_CASE("effectiveDeviceName falls back to platform default when override empty") {
    Settings s;
    CHECK(s.deviceName().empty());
    CHECK(s.effectiveDeviceName() == Settings::platformDeviceName());

    s.setDeviceName("custom");
    CHECK(s.effectiveDeviceName() == "custom");
}

TEST_CASE("platformDeviceName respects the 64-char server limit") {
    CHECK(Settings::platformDeviceName().size() <= 64);
}
