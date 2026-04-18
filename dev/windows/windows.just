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

[windows]
run: build
    build\jellyfin-desktop.exe --log-level=debug --log-file=build\run.log
