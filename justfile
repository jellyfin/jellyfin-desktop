set dotenv-load := true

export JELLIUM_DESKTOP_LOG_LEVEL := env_var_or_default("JELLIUM_DESKTOP_LOG_LEVEL", "debug")
export JELLIUM_DESKTOP_LOG_FILE := env_var_or_default("JELLIUM_DESKTOP_LOG_FILE", "build/run.log")
export JFN_MPV_INCLUDE_DIR := "third_party/mpv/include"

import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

# List recipes
[private]
list:
    @just --list --unsorted

# Remove build artifacts
[group('maintenance')]
[macos]
[linux]
clean:
    rm -rf build dist
    cargo clean --manifest-path src/Cargo.toml

# Remove build artifacts
[group('maintenance')]
[windows]
clean:
    if (Test-Path build) { Remove-Item -Recurse -Force build }
    if (Test-Path dist) { Remove-Item -Recurse -Force dist }
    cargo clean --manifest-path src/Cargo.toml

# Run tests
[group('test')]
[unix]
test: build
    cargo test --manifest-path src/Cargo.toml --workspace

# Run tests (loads MSVC + bindgen libclang env via dev/windows/env.ps1)
[group('test')]
[windows]
test: build
    . 'dev/windows/env.ps1'; cargo test --manifest-path src/Cargo.toml --workspace

# Format workspace
[group('lint')]
fmt:
    cargo fmt --manifest-path src/Cargo.toml --all

# Check formatting
[group('lint')]
fmt-check:
    cargo fmt --manifest-path src/Cargo.toml --all -- --check

# Run clippy
[group('lint')]
[unix]
clippy:
    cargo clippy --manifest-path src/Cargo.toml --workspace --all-targets -- \
        -D warnings \
        -D clippy::unwrap_used \
        -D clippy::expect_used \
        -D clippy::panic

# Run clippy (loads MSVC + bindgen libclang env via dev/windows/env.ps1)
[group('lint')]
[windows]
clippy:
    . 'dev/windows/env.ps1'; \
    cargo clippy --manifest-path src/Cargo.toml --workspace --all-targets -- \
        -D warnings \
        -D clippy::unwrap_used \
        -D clippy::expect_used \
        -D clippy::panic

# Lint workspace
[group('lint')]
lint: fmt-check clippy

# Strict lint workspace
[group('lint')]
strict-lint:
    cargo fmt --manifest-path src/Cargo.toml --all -- --check
    cargo clippy --manifest-path src/Cargo.toml --workspace --all-targets -- \
        -D warnings \
        -D clippy::pedantic \
        -D clippy::nursery \
        -D clippy::unwrap_used \
        -D clippy::expect_used \
        -D clippy::panic
