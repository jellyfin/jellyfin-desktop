import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

set positional-arguments

# List recipes
list:
    @just --list --unsorted

# Update vendored deps
update-deps *args:
    python3 dev/tools/update_deps.py {{args}}

# Remove build artifacts
clean:
    rm -rf build dist

# Lint Rust crates (rustfmt --check + clippy -D warnings).
# jfn-wlproxy is Linux-only and skipped on other platforms.
lint:
    #!/bin/sh
    set -eu
    cargo fmt --manifest-path src/config/Cargo.toml -- --check
    cargo clippy --manifest-path src/config/Cargo.toml --all-targets -- -D warnings
    cargo fmt --manifest-path src/cli/Cargo.toml -- --check
    cargo clippy --manifest-path src/cli/Cargo.toml --all-targets -- -D warnings
    cargo fmt --manifest-path src/log_redact/Cargo.toml -- --check
    cargo clippy --manifest-path src/log_redact/Cargo.toml --all-targets -- -D warnings
    cargo fmt --manifest-path src/paths/Cargo.toml -- --check
    cargo clippy --manifest-path src/paths/Cargo.toml --all-targets -- -D warnings
    if [ "$(uname)" = "Linux" ]; then
        cargo fmt --manifest-path src/wlproxy/Cargo.toml -- --check
        cargo clippy --manifest-path src/wlproxy/Cargo.toml --all-targets -- -D warnings
    fi
