set dotenv-load := true

export JELLYFIN_DESKTOP_LOG_LEVEL := env_var_or_default("JELLYFIN_DESKTOP_LOG_LEVEL", "debug")
export JELLYFIN_DESKTOP_LOG_FILE := env_var_or_default("JELLYFIN_DESKTOP_LOG_FILE", "build/run.log")

import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

# List recipes
[private]
list:
    @just --list --unsorted

# Remove build artifacts
[macos]
[linux]
clean:
    rm -rf build dist
    cargo clean --manifest-path src/Cargo.toml

# Remove build artifacts
[windows]
clean:
    if (Test-Path build) { Remove-Item -Recurse -Force build }
    if (Test-Path dist) { Remove-Item -Recurse -Force dist }
    cargo clean --manifest-path src/Cargo.toml

# Run tests
test: build
    cargo test --manifest-path src/Cargo.toml --workspace

# Lint workspace
lint:
    cargo fmt --manifest-path src/Cargo.toml --all -- --check
    JFN_MPV_INCLUDE_DIR=third_party/mpv/include \
        cargo clippy --manifest-path src/Cargo.toml --workspace --all-targets
