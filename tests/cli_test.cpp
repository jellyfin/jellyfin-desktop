#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cli.h"

#include <string>
#include <vector>

namespace {

// Builds non-const argv from a list of string literals (parse takes char**).
// Two-pass population: fully build `storage` first, then take `.data()`
// pointers — a one-pass loop relies on a reserve() to keep prior pointers
// valid across vector reallocation.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    Argv(std::initializer_list<const char*> args) {
        storage.reserve(args.size());
        for (const char* a : args) storage.emplace_back(a);
        ptrs.reserve(storage.size());
        for (auto& s : storage) ptrs.push_back(s.data());
    }

    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

}  // namespace

using K = cli::Result::Kind;

TEST_CASE("parse: no args returns Continue, leaves args untouched") {
    Argv av{"app"};
    cli::Args args;
    args.hwdec = "auto";
    args.audio_exclusive = true;

    auto r = cli::parse(av.argc(), av.argv(), args);

    CHECK(r.kind == K::Continue);
    CHECK(args.hwdec == "auto");
    CHECK(args.audio_exclusive == true);
}

TEST_CASE("parse: -h and --help return Help") {
    cli::Args args;
    {
        Argv av{"app", "-h"};
        CHECK(cli::parse(av.argc(), av.argv(), args).kind == K::Help);
    }
    {
        Argv av{"app", "--help"};
        CHECK(cli::parse(av.argc(), av.argv(), args).kind == K::Help);
    }
}

TEST_CASE("parse: -v and --version return Version") {
    cli::Args args;
    {
        Argv av{"app", "-v"};
        CHECK(cli::parse(av.argc(), av.argv(), args).kind == K::Version);
    }
    {
        Argv av{"app", "--version"};
        CHECK(cli::parse(av.argc(), av.argv(), args).kind == K::Version);
    }
}

TEST_CASE("parse: unknown argument returns Error with arg text") {
    cli::Args args;
    {
        Argv av{"app", "--nope"};
        auto r = cli::parse(av.argc(), av.argv(), args);
        CHECK(r.kind == K::Error);
        CHECK(r.unknown_arg == "--nope");
    }
    {
        Argv av{"app", "-x"};
        auto r = cli::parse(av.argc(), av.argv(), args);
        CHECK(r.kind == K::Error);
        CHECK(r.unknown_arg == "-x");
    }
    {
        Argv av{"app", "positional"};
        auto r = cli::parse(av.argc(), av.argv(), args);
        CHECK(r.kind == K::Error);
        CHECK(r.unknown_arg == "positional");
    }
}

TEST_CASE("parse: missing value for trailing --foo treated as unknown arg") {
    // Original behavior: --log-level at end-of-argv falls through to error
    // because match_value requires either a following slot or '=' form.
    Argv av{"app", "--log-level"};
    cli::Args args;
    auto r = cli::parse(av.argc(), av.argv(), args);
    CHECK(r.kind == K::Error);
    CHECK(r.unknown_arg == "--log-level");
}

TEST_CASE("parse: bool flags") {
    {
        Argv av{"app", "--audio-exclusive"};
        cli::Args args;
        REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
        CHECK(args.audio_exclusive == true);
    }
    {
        Argv av{"app", "--disable-gpu-compositing"};
        cli::Args args;
        REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
        CHECK(args.disable_gpu_compositing == true);
    }
}

TEST_CASE("parse: --name value (space form)") {
    Argv av{
        "app",
        "--hwdec", "vaapi",
        "--log-level", "debug",
        "--log-file", "/tmp/x.log",
        "--audio-passthrough", "ac3,dts-hd",
        "--audio-channels", "5.1",
        "--remote-debug-port", "9222",
        "--ozone-platform", "wayland",
        "--platform", "x11",
    };
    cli::Args args;
    auto r = cli::parse(av.argc(), av.argv(), args);

    REQUIRE(r.kind == K::Continue);
    CHECK(args.hwdec == "vaapi");
    CHECK(args.log_level == "debug");
    REQUIRE(args.log_file.has_value());
    CHECK(*args.log_file == "/tmp/x.log");
    CHECK(args.audio_passthrough == "ac3,dts-hd");
    CHECK(args.audio_channels == "5.1");
    CHECK(args.remote_debugging_port == 9222);
    CHECK(args.ozone_platform == "wayland");
    CHECK(args.platform_override == "x11");
}

TEST_CASE("parse: --name=value (equals form)") {
    Argv av{
        "app",
        "--hwdec=nvdec",
        "--log-level=trace",
        "--log-file=/tmp/y.log",
        "--audio-passthrough=eac3",
        "--audio-channels=stereo",
        "--remote-debug-port=8080",
        "--ozone-platform=x11",
        "--platform=wayland",
    };
    cli::Args args;
    auto r = cli::parse(av.argc(), av.argv(), args);

    REQUIRE(r.kind == K::Continue);
    CHECK(args.hwdec == "nvdec");
    CHECK(args.log_level == "trace");
    REQUIRE(args.log_file.has_value());
    CHECK(*args.log_file == "/tmp/y.log");
    CHECK(args.audio_passthrough == "eac3");
    CHECK(args.audio_channels == "stereo");
    CHECK(args.remote_debugging_port == 8080);
    CHECK(args.ozone_platform == "x11");
    CHECK(args.platform_override == "wayland");
}

TEST_CASE("parse: --log-file with empty value distinguishes from unset") {
    {
        Argv av{"app"};
        cli::Args args;
        REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
        CHECK(!args.log_file.has_value());
    }
    {
        Argv av{"app", "--log-file="};
        cli::Args args;
        REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
        REQUIRE(args.log_file.has_value());
        CHECK(*args.log_file == "");
    }
    {
        Argv av{"app", "--log-file", ""};
        cli::Args args;
        REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
        REQUIRE(args.log_file.has_value());
        CHECK(*args.log_file == "");
    }
}

TEST_CASE("parse: CLI overrides pre-populated defaults") {
    cli::Args args;
    args.hwdec = "auto";
    args.log_level = "info";
    args.audio_exclusive = false;

    Argv av{"app", "--hwdec", "vulkan", "--log-level=warn", "--audio-exclusive"};
    REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);

    CHECK(args.hwdec == "vulkan");
    CHECK(args.log_level == "warn");
    CHECK(args.audio_exclusive == true);
}

TEST_CASE("parse: defaults preserved when matching flag absent") {
    cli::Args args;
    args.hwdec = "auto";
    args.log_level = "info";
    args.audio_channels = "stereo";

    Argv av{"app", "--log-level=debug"};
    REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);

    CHECK(args.hwdec == "auto");
    CHECK(args.log_level == "debug");
    CHECK(args.audio_channels == "stereo");
}

TEST_CASE("parse: --remote-debug-port non-numeric becomes 0 (atoi)") {
    Argv av{"app", "--remote-debug-port=bogus"};
    cli::Args args;
    REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
    CHECK(args.remote_debugging_port == 0);
}

TEST_CASE("parse: prefix collision — --log-level= does not eat --log-file=") {
    Argv av{"app", "--log-file=path", "--log-level=trace"};
    cli::Args args;
    REQUIRE(cli::parse(av.argc(), av.argv(), args).kind == K::Continue);
    REQUIRE(args.log_file.has_value());
    CHECK(*args.log_file == "path");
    CHECK(args.log_level == "trace");
}

TEST_CASE("parse: stops at first unknown arg, partially populates") {
    Argv av{"app", "--hwdec", "vaapi", "--garbage", "--log-level", "debug"};
    cli::Args args;
    auto r = cli::parse(av.argc(), av.argv(), args);
    CHECK(r.kind == K::Error);
    CHECK(r.unknown_arg == "--garbage");
    CHECK(args.hwdec == "vaapi");
    CHECK(args.log_level == "");  // not reached
}
