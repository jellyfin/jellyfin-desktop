# Windows-specific recipes (imported by top-level justfile)
#
# Recipes run under PowerShell 7 so the dev/windows/*.ps1 scripts
# (which use PS 7+ syntax such as `&&`) parse correctly. just's default
# shell is `sh`, which isn't available from a plain PowerShell prompt,
# so we point windows-shell at pwsh.exe explicitly.
set windows-shell := ["C:\\Program Files\\PowerShell\\7\\pwsh.exe", "-NoLogo", "-ExecutionPolicy", "Bypass", "-Command"]

[windows]
deps:
    & 'dev/windows/setup.ps1'

[windows]
build: deps
    & 'dev/windows/build.ps1'

[windows]
test: build
    cargo test --manifest-path src/Cargo.toml --workspace --exclude jfn-wlproxy --exclude jfn-signal-guard

[windows]
run *args: build
    $env:RUST_BACKTRACE = 'full'; & 'build/jellyfin-desktop.exe' --log-level=debug --log-file=build/run.log {{args}} 2>&1 | Tee-Object -FilePath build/stderr.log

# Run the standalone mpv CLI built from the submodule (forwards args)
[windows]
run-mpv *args: build
    & 'build/mpv-build/mpv.exe' {{args}}
