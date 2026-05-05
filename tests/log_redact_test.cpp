#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "log_redact.h"

#include <string>

using log_redact::censor;
using log_redact::containsSecret;

namespace {

// 32-char hex token, like Jellyfin emits.
constexpr const char* kToken = "7e7a0b378bc4440e85016c880ba4cfa7";
constexpr const char* kRedacted = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

std::string redacted(std::string s) {
    censor(s);
    return s;
}

}  // namespace

TEST_CASE("censor leaves messages without sensitive patterns alone") {
    CHECK(redacted("") == "");
    CHECK(redacted("nothing to see here") == "nothing to see here");
    CHECK(redacted("token=abc still safe") == "token=abc still safe");
}

TEST_CASE("censor preserves message length (overwrites in place)") {
    std::string s = std::string("ApiKey=") + kToken + "&Tag=z";
    auto before = s.size();
    censor(s);
    CHECK(s.size() == before);
}

TEST_CASE("censor redacts api_key= query param") {
    std::string url = std::string("https://host/socket?api_key=") + kToken + "&deviceId=abc";
    censor(url);
    CHECK(url == std::string("https://host/socket?api_key=") + kRedacted + "&deviceId=abc");
}

TEST_CASE("censor redacts ApiKey= camelcase variant") {
    std::string url = std::string("/stream.mkv?Static=true&ApiKey=") + kToken + "&Tag=xyz";
    censor(url);
    CHECK(url == std::string("/stream.mkv?Static=true&ApiKey=") + kRedacted + "&Tag=xyz");
}

TEST_CASE("censor redacts AccessToken JSON value") {
    std::string json =
        std::string("{\"UserId\":\"abc\",\"AccessToken\":\"") + kToken + "\",\"Other\":1}";
    censor(json);
    CHECK(json == std::string("{\"UserId\":\"abc\",\"AccessToken\":\"") + kRedacted +
                      "\",\"Other\":1}");
}

TEST_CASE("censor redacts AccessToken= form") {
    std::string s = std::string("AccessToken=") + kToken + " trailing";
    censor(s);
    CHECK(s == std::string("AccessToken=") + kRedacted + " trailing");
}

TEST_CASE("censor redacts X-MediaBrowser-Token header forms") {
    std::string plain = std::string("X-MediaBrowser-Token=") + kToken + ";";
    censor(plain);
    CHECK(plain == std::string("X-MediaBrowser-Token=") + kRedacted + ";");

    std::string encoded = std::string("X-MediaBrowser-Token%3D") + kToken + "&";
    censor(encoded);
    CHECK(encoded == std::string("X-MediaBrowser-Token%3D") + kRedacted + "&");
}

TEST_CASE("censor redacts every occurrence of the same pattern") {
    std::string s = std::string("first ApiKey=") + kToken + " second ApiKey=" + kToken;
    censor(s);
    CHECK(s == std::string("first ApiKey=") + kRedacted + " second ApiKey=" + kRedacted);
}

TEST_CASE("censor handles multiple distinct patterns in one message") {
    std::string s = std::string("api_key=") + kToken + " and AccessToken=" + kToken;
    censor(s);
    CHECK(s == std::string("api_key=") + kRedacted + " and AccessToken=" + kRedacted);
}

TEST_CASE("censor redacts to end-of-string when no terminator follows") {
    std::string s = std::string("trailing ApiKey=") + kToken;
    censor(s);
    CHECK(s == std::string("trailing ApiKey=") + kRedacted);
}

TEST_CASE("censor redacts short token values to their actual length") {
    std::string s = "ApiKey=abc&next=1";
    censor(s);
    CHECK(s == "ApiKey=xxx&next=1");
}

TEST_CASE("censor leaves pattern with no token unchanged") {
    std::string s = "ApiKey=&Tag=xyz";
    std::string copy = s;
    censor(copy);
    CHECK(copy == s);

    std::string at_eof = "trailing ApiKey=";
    std::string at_eof_copy = at_eof;
    censor(at_eof_copy);
    CHECK(at_eof_copy == at_eof);
}

TEST_CASE("censor matches case-sensitively (api_key vs API_KEY)") {
    std::string s = std::string("API_KEY=") + kToken;
    std::string copy = s;
    censor(copy);
    CHECK(copy == s);  // pattern is lowercase api_key= only
}

TEST_CASE("containsSecret returns true only when a token follows") {
    CHECK(containsSecret(std::string("ApiKey=") + kToken));
    CHECK(containsSecret("ApiKey=x"));  // even a single char counts

    CHECK_FALSE(containsSecret(""));
    CHECK_FALSE(containsSecret("nothing relevant"));
    CHECK_FALSE(containsSecret("ApiKey=&next=1"));  // pattern present, terminator immediately after
    CHECK_FALSE(containsSecret("ApiKey="));         // pattern at EOF, no value
    CHECK_FALSE(containsSecret("apikey lowercase mention only"));
}

TEST_CASE("containsSecret detects each pattern variant") {
    CHECK(containsSecret(std::string("api_key=") + kToken));
    CHECK(containsSecret(std::string("ApiKey=") + kToken));
    CHECK(containsSecret(std::string("AccessToken=") + kToken));
    CHECK(containsSecret(std::string("AccessToken\":\"") + kToken));
    CHECK(containsSecret(std::string("X-MediaBrowser-Token=") + kToken));
    CHECK(containsSecret(std::string("X-MediaBrowser-Token%3D") + kToken));
}

TEST_CASE("censor preserves rest of multi-pattern realistic playback URL") {
    std::string url =
        "https://localhost:8096/Videos/abc/stream.mkv?Static=true"
        "&deviceId=opaque&ApiKey=";
    url += kToken;
    url += "&Tag=298f007d1192550985fc1e5960bbb3d7";
    censor(url);
    CHECK(url ==
          std::string("https://localhost:8096/Videos/abc/stream.mkv?Static=true"
                      "&deviceId=opaque&ApiKey=") +
              kRedacted + "&Tag=298f007d1192550985fc1e5960bbb3d7");
}
