# List available recipes
list:
    @just --list

# Configure (if needed) + build the main app
build: deps
    #!/bin/sh
    set -eu
    if ! [ -f build/CMakeCache.txt ]; then
        cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
    fi
    cmake --build build

# Ensure submodules and CEF are present
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
test: build
    ctest --test-dir build --output-on-failure

# Run the app with debug logging
run: build
    build/jellyfin-desktop --log-level=debug --log-file=build/run.log

# Remove build artifacts (keeps CEF SDK download)
clean:
    rm -rf build third_party/mpv/build
