#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "jellyfin_api.h"

using jellyfin_api::extract_base_url;
using jellyfin_api::is_valid_public_info;
using jellyfin_api::normalize_input;

TEST_CASE("normalize_input trims whitespace") {
    CHECK(normalize_input("  http://example.com  ") == "http://example.com");
    CHECK(normalize_input("\thttps://host\n")       == "https://host");
}

TEST_CASE("normalize_input lowercases scheme") {
    CHECK(normalize_input("HTTP://example.com")  == "http://example.com");
    CHECK(normalize_input("HTTPS://example.com") == "https://example.com");
    CHECK(normalize_input("Http://example.com")  == "http://example.com");
    CHECK(normalize_input("Https://example.com") == "https://example.com");
}

TEST_CASE("normalize_input prepends http:// when no scheme") {
    CHECK(normalize_input("example.com")      == "http://example.com");
    CHECK(normalize_input("example.com:8096") == "http://example.com:8096");
    CHECK(normalize_input("192.168.1.10")     == "http://192.168.1.10");
}

TEST_CASE("normalize_input trims whitespace before prepending scheme") {
    // Trim must happen first: otherwise a leading space would get trapped
    // between the prepended scheme and the host, producing "http:// host".
    CHECK(normalize_input(" example.com")     == "http://example.com");
    CHECK(normalize_input("\texample.com\n")  == "http://example.com");
    CHECK(normalize_input("   example.com  ") == "http://example.com");
}

TEST_CASE("normalize_input leaves well-formed input unchanged") {
    CHECK(normalize_input("http://example.com")           == "http://example.com");
    CHECK(normalize_input("https://example.com/jellyfin") == "https://example.com/jellyfin");
}

TEST_CASE("normalize_input passes non-http schemes through") {
    // Only Http:/Https: prefixes are touched; anything else passes through.
    CHECK(normalize_input("FTP://example.com") == "FTP://example.com");
}

TEST_CASE("extract_base_url truncates at /web") {
    CHECK(extract_base_url("https://host/web/index.html") == "https://host");
    CHECK(extract_base_url("https://host/web")            == "https://host");
}

TEST_CASE("extract_base_url preserves prefix before /web") {
    CHECK(extract_base_url("https://host/jellyfin/web/index.html")
          == "https://host/jellyfin");
    CHECK(extract_base_url("https://host:8096/jellyfin/web/")
          == "https://host:8096/jellyfin");
}

TEST_CASE("extract_base_url uses LAST /web when multiple present") {
    CHECK(extract_base_url("https://host/web/app/web/index.html")
          == "https://host/web/app");
}

TEST_CASE("extract_base_url is case-insensitive for /web") {
    CHECK(extract_base_url("https://host/WEB/index.html") == "https://host");
    CHECK(extract_base_url("https://host/Web/index.html") == "https://host");
    CHECK(extract_base_url("https://host/wEb/index.html") == "https://host");
}

TEST_CASE("extract_base_url returns origin when no /web in path") {
    CHECK(extract_base_url("https://host/")        == "https://host");
    CHECK(extract_base_url("https://host")         == "https://host");
    CHECK(extract_base_url("https://host/foo")     == "https://host");
    CHECK(extract_base_url("http://host:8096/foo") == "http://host:8096");
}

TEST_CASE("extract_base_url handles port in origin") {
    CHECK(extract_base_url("http://host:8096/web/index.html") == "http://host:8096");
    CHECK(extract_base_url("http://localhost:8096/web/")      == "http://localhost:8096");
    CHECK(extract_base_url("http://192.168.1.100:8096/web/")  == "http://192.168.1.100:8096");
    CHECK(extract_base_url("http://[::1]:8096/web/")          == "http://[::1]:8096");
}

TEST_CASE("extract_base_url strips query string and fragment after /web") {
    CHECK(extract_base_url("https://host/web/?foo=bar")                  == "https://host");
    CHECK(extract_base_url("https://host/web/#section")                  == "https://host");
    CHECK(extract_base_url("https://host/jellyfin/web/?foo=bar#section") == "https://host/jellyfin");
}

TEST_CASE("extract_base_url treats /website and /webdav as /web match") {
    // Matches Qt behavior: substring match on "/web" does not distinguish
    // these longer path segments. Locked in here so a future fix is deliberate.
    CHECK(extract_base_url("https://host/website/") == "https://host");
    CHECK(extract_base_url("https://host/webdav/")  == "https://host");
}

TEST_CASE("extract_base_url handles degenerate URLs") {
    CHECK(extract_base_url("https://")      == "https://");
    CHECK(extract_base_url("https:///web/") == "https://");
}

TEST_CASE("URLs with non-ASCII IDN hosts survive unchanged") {
    CHECK(normalize_input("http://example.みんな")         == "http://example.みんな");
    CHECK(normalize_input("example.みんな")                == "http://example.みんな");
    CHECK(normalize_input("  HTTPS://example.みんな/web ") == "https://example.みんな/web");

    CHECK(extract_base_url("http://example.みんな/web/")          == "http://example.みんな");
    CHECK(extract_base_url("https://example.みんな/jellyfin/web") == "https://example.みんな/jellyfin");
    CHECK(extract_base_url("http://example.みんな/")              == "http://example.みんな");
}

TEST_CASE("is_valid_public_info accepts object with non-empty Id") {
    CHECK(is_valid_public_info(R"({"Id":"abc","ServerName":"x"})"));
    CHECK(is_valid_public_info(R"({"ServerName":"x","Id":"zzz"})"));
}

TEST_CASE("is_valid_public_info rejects empty or missing Id") {
    CHECK_FALSE(is_valid_public_info(R"({"Id":""})"));
    CHECK_FALSE(is_valid_public_info(R"({"ServerName":"x"})"));
    CHECK_FALSE(is_valid_public_info(R"({})"));
}

TEST_CASE("is_valid_public_info rejects non-string Id") {
    CHECK_FALSE(is_valid_public_info(R"({"Id":null})"));
    CHECK_FALSE(is_valid_public_info(R"({"Id":123})"));
    CHECK_FALSE(is_valid_public_info(R"({"Id":true})"));
}

TEST_CASE("is_valid_public_info rejects non-object JSON") {
    CHECK_FALSE(is_valid_public_info(R"(["Id"])"));
    CHECK_FALSE(is_valid_public_info(R"("Id")"));
    CHECK_FALSE(is_valid_public_info("null"));
}

TEST_CASE("is_valid_public_info rejects invalid JSON") {
    CHECK_FALSE(is_valid_public_info(""));
    CHECK_FALSE(is_valid_public_info("not json"));
    CHECK_FALSE(is_valid_public_info(R"({"Id":"abc")"));  // unterminated
}

TEST_CASE("is_valid_public_info does not false-positive on substring") {
    // Regression: the old code string-matched \"Id\" which matched any body
    // containing that substring. Real JSON parse must reject these.
    CHECK_FALSE(is_valid_public_info(R"(<html>oops "Id" lives here</html>)"));
}
