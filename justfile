import 'dev/linux/linux.just'
import 'dev/macos/macos.just'
import 'dev/windows/windows.just'

set positional-arguments

# List recipes
list:
    @just --list --unsorted

# Update vendored deps
update-deps *args:
    python3 dev/tools/update_deps.py {{args}}

# Remove build artifacts
clean:
    rm -rf build dist
