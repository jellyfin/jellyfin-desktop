# Windows-specific recipes (imported by top-level justfile)

[windows]
deps:
    powershell -ExecutionPolicy Bypass -File dev/windows/setup.ps1

[windows]
build: deps
    cmd /c dev\windows\build.bat

[windows]
test: build
    ctest --test-dir build --output-on-failure
    cargo test --manifest-path src/Cargo.toml --workspace --exclude jfn-wlproxy --exclude jfn-signal-guard

[windows]
run *args: build
    build\jellyfin-desktop.exe --log-level=debug --log-file=build\run.log {{args}}

# Run the standalone mpv CLI built from the submodule (forwards args)
[windows]
run-mpv *args: build
    build\mpv-build\mpv.exe {{args}}
