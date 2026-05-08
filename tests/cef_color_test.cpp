#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cef/color.h"

namespace {

bool eq(Color c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}

}  // namespace

TEST_CASE("cef::parseColor empty / non-hash → black") {
    CHECK(eq(cef::parseColor(""), 0, 0, 0));
    CHECK(eq(cef::parseColor("garbage"), 0, 0, 0));
    CHECK(eq(cef::parseColor("blue"), 0, 0, 0));
    CHECK(eq(cef::parseColor("rgb(0,0,255)"), 0, 0, 0));
    CHECK(eq(cef::parseColor("000000"), 0, 0, 0));  // missing #
    CHECK(eq(cef::parseColor("#"), 0, 0, 0));
}

TEST_CASE("cef::parseColor #RRGGBB") {
    CHECK(eq(cef::parseColor("#000000"), 0x00, 0x00, 0x00));
    CHECK(eq(cef::parseColor("#FFFFFF"), 0xFF, 0xFF, 0xFF));
    CHECK(eq(cef::parseColor("#FF00FF"), 0xFF, 0x00, 0xFF));
    CHECK(eq(cef::parseColor("#0000FF"), 0x00, 0x00, 0xFF));
    CHECK(eq(cef::parseColor("#abcdef"), 0xAB, 0xCD, 0xEF));
    CHECK(eq(cef::parseColor("#202020"), 0x20, 0x20, 0x20));  // jellyfin default
    CHECK(eq(cef::parseColor("#101010"), 0x10, 0x10, 0x10));  // kBgColor
}

TEST_CASE("cef::parseColor #RGB shorthand expands to #RRGGBB") {
    CHECK(eq(cef::parseColor("#000"), 0x00, 0x00, 0x00));
    CHECK(eq(cef::parseColor("#fff"), 0xFF, 0xFF, 0xFF));
    CHECK(eq(cef::parseColor("#abc"), 0xAA, 0xBB, 0xCC));
    CHECK(eq(cef::parseColor("#f0f"), 0xFF, 0x00, 0xFF));
}

TEST_CASE("cef::parseColor rejects mpv-only forms and weird lengths") {
    CHECK(eq(cef::parseColor("#FF0000FF"), 0, 0, 0));  // 8 hex (mpv #AARRGGBB)
    CHECK(eq(cef::parseColor("#0/0/1"), 0, 0, 0));     // mpv slash
    CHECK(eq(cef::parseColor("0/0/1"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#ab"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#abcd"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#abcde"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#abcdefg"), 0, 0, 0));
}

TEST_CASE("cef::parseColor rejects malformed hex") {
    CHECK(eq(cef::parseColor("#zzz"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#zzzzzz"), 0, 0, 0));
    CHECK(eq(cef::parseColor("#ab cdef"), 0, 0, 0));
    CHECK(eq(cef::parseColor("# 00000"), 0, 0, 0));
}
