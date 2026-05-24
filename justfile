import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

# List recipes
list:
    @just --list --unsorted

# Update vendored deps
update-deps *args:
    python3 dev/tools/update_deps.py {{args}}

# Remove build artifacts
clean:
    rm -rf build dist

# Run the workspace test suite (depends on the per-platform `build`).
test: build
    cargo test --manifest-path src/Cargo.toml --workspace

# Lint the whole workspace (rustfmt --check + clippy). Lint levels are denied
# centrally via [workspace.lints] in src/Cargo.toml, so no -D flag is needed.
lint:
    cargo fmt --manifest-path src/Cargo.toml --all -- --check
    JFN_MPV_INCLUDE_DIR=third_party/mpv/include \
        cargo clippy --manifest-path src/Cargo.toml --workspace --all-targets
