#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mpv/color.h"

namespace {

bool eq(Color c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}

}  // namespace

TEST_CASE("mpv::parseColor empty / garbage → black") {
    CHECK(eq(mpv::parseColor(""), 0, 0, 0));
    CHECK(eq(mpv::parseColor("garbage"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("blue"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("rgb(0,0,255)"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("#"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("#z00000"), 0, 0, 0));
}

TEST_CASE("mpv::parseColor #RRGGBB") {
    CHECK(eq(mpv::parseColor("#000000"), 0x00, 0x00, 0x00));
    CHECK(eq(mpv::parseColor("#FFFFFF"), 0xFF, 0xFF, 0xFF));
    CHECK(eq(mpv::parseColor("#FF00FF"), 0xFF, 0x00, 0xFF));
    CHECK(eq(mpv::parseColor("#0000FF"), 0x00, 0x00, 0xFF));
    CHECK(eq(mpv::parseColor("#abcdef"), 0xAB, 0xCD, 0xEF));
}

TEST_CASE("mpv::parseColor #AARRGGBB — alpha first, dropped") {
    // Regression for the actual bug: mpv's print_color emits #AARRGGBB,
    // so blue (#0000FF) round-trips as #FF0000FF and must parse to blue,
    // not red.
    CHECK(eq(mpv::parseColor("#FF0000FF"), 0x00, 0x00, 0xFF));
    CHECK(eq(mpv::parseColor("#FFFFFFFF"), 0xFF, 0xFF, 0xFF));
    CHECK(eq(mpv::parseColor("#00000000"), 0x00, 0x00, 0x00));
    CHECK(eq(mpv::parseColor("#0012ab34"), 0x12, 0xAB, 0x34));
}

TEST_CASE("mpv::parseColor rejects CSS shorthand and weird hex lengths") {
    CHECK(eq(mpv::parseColor("#abc"), 0, 0, 0));   // 3 hex — CSS only
    CHECK(eq(mpv::parseColor("#abcd"), 0, 0, 0));  // 4 hex — neither form
    CHECK(eq(mpv::parseColor("#abcde"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("#abcdefg"), 0, 0, 0));
}

TEST_CASE("mpv::parseColor slash form r/g/b") {
    CHECK(eq(mpv::parseColor("0/0/1"), 0, 0, 255));
    CHECK(eq(mpv::parseColor("1/0/0"), 255, 0, 0));
    CHECK(eq(mpv::parseColor("0/1/0"), 0, 255, 0));
    CHECK(eq(mpv::parseColor("1/1/1"), 255, 255, 255));
    CHECK(eq(mpv::parseColor("0.5/0.5/0.5"), 128, 128, 128));
}

TEST_CASE("mpv::parseColor slash form r/g/b/a — alpha dropped") {
    CHECK(eq(mpv::parseColor("0/0/1/1"), 0, 0, 255));
    CHECK(eq(mpv::parseColor("0/0/1/0"), 0, 0, 255));  // even with alpha=0
    CHECK(eq(mpv::parseColor("1/1/1/0.5"), 255, 255, 255));
}

TEST_CASE("mpv::parseColor slash form gray (1 component) and gray+alpha (2)") {
    CHECK(eq(mpv::parseColor("0"), 0, 0, 0));
    CHECK(eq(mpv::parseColor("1"), 0, 0, 0));  // no slash — rejected
    CHECK(eq(mpv::parseColor("0.5/1"), 128, 128, 128));  // gray + alpha
    CHECK(eq(mpv::parseColor("0/1"), 0, 0, 0));          // gray + alpha
}

TEST_CASE("mpv::parseColor slash out-of-range and malformed") {
    CHECK(eq(mpv::parseColor("0/0/2"), 0, 0, 0));      // > 1
    CHECK(eq(mpv::parseColor("-1/0/0"), 0, 0, 0));     // < 0
    CHECK(eq(mpv::parseColor("0/0/"), 0, 0, 0));       // trailing slash
    CHECK(eq(mpv::parseColor("/0/0/0"), 0, 0, 0));     // leading slash
    CHECK(eq(mpv::parseColor("0/0/0/0/0"), 0, 0, 0)); // 5 components
    CHECK(eq(mpv::parseColor("a/b/c"), 0, 0, 0));      // non-numeric
    CHECK(eq(mpv::parseColor("0/0/0.5x"), 0, 0, 0));   // trailing junk
}
