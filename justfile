import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

# List available recipes
list:
    @just --list

# Configure (if needed) + build the main app
[linux]
build: deps
    #!/bin/sh
    set -eu
    if ! [ -f build/CMakeCache.txt ]; then
        cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
    fi
    cmake --build build

# Ensure submodules and CEF are present
[linux]
deps:
    #!/bin/sh
    set -eu
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    if ! [ -d third_party/cef ]; then
        python3 dev/download_cef.py
    fi

# Run unit tests
[linux]
test: build
    ctest --test-dir build --output-on-failure

# Run the app with debug logging
[linux]
run: build
    build/jellyfin-desktop --log-level=debug --log-file=build/run.log

# Update vendored/fetched deps (CEF, doctest, quill); pass --check to verify only
update-deps *args:
    python3 dev/tools/update_deps.py {{args}}

# Remove build artifacts (keeps CEF SDK download)
clean:
    rm -rf build third_party/mpv/build
