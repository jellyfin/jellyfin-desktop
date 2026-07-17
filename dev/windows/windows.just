# Windows-specific recipes (imported by top-level justfile)
#
# Recipes run under PowerShell 7 so the dev/windows/*.ps1 scripts
# (which use PS 7+ syntax such as `&&`) parse correctly. just's default
# shell is `sh`, which isn't available from a plain PowerShell prompt,
# so we point windows-shell at pwsh.exe explicitly.
set windows-shell := ["C:\\Program Files\\PowerShell\\7\\pwsh.exe", "-NoLogo", "-ExecutionPolicy", "Bypass", "-Command"]

[group('deps')]
[windows]
deps:
    & 'dev/windows/setup.ps1'

[group('build')]
[windows]
build: deps
    & 'dev/windows/build.ps1'

[group('run')]
[windows]
run *args: build
    $env:RUST_BACKTRACE = 'full'; & 'build/jellium-desktop.exe' {{args}} 2>&1 | Tee-Object -FilePath build/stderr.log

# Run the standalone mpv CLI built from the submodule (forwards args)
[group('run')]
[windows]
run-mpv *args: build
    & 'build/mpv-build/mpv.exe' {{args}}
