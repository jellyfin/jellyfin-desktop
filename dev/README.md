# Development

After cloning this repo, you will need to initialize and update its submodules:
```bash
git submodule update --init --recursive
```

Platform-specific build instructions:
- [macOS](macos/README.md)
- [Windows](windows/README.md)
- Linux: See [GitHub Actions workflow](../.github/workflows/test.yml) - install deps from `debian/control`, then CMake build

## Web Debugger

To get browser devtools, use remote debugging:

1. Run with `--remote-debugging-port=9222`
2. Open Chromium/Chrome and navigate to `chrome://inspect/#devices`
3. Make sure "Discover Network Targets" is checked and `localhost:9222` is configured
