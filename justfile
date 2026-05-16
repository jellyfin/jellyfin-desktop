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
lint:
    #!/bin/sh
    set -eu
    cargo fmt --manifest-path src/config/Cargo.toml -- --check
    cargo clippy --manifest-path src/config/Cargo.toml --all-targets -- -D warnings
