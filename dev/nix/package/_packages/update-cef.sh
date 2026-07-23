#!/usr/bin/env nix-shell
#!nix-shell -i bash -p curl gnused jq nix-update cargo rustc git

set -euo pipefail

export NIX_CONFIG="experimental-features = nix-command flakes"

if [ -f "${PWD}/flake.nix" ]; then
  ROOT="${PWD}"
else
  ROOT="$(git rev-parse --show-toplevel)"
fi

PACKAGE_PATH="${ROOT}/dev/nix/package/_packages"
PACKAGE="cef-binary"

target_version=$(cargo metadata --format-version 1 --locked --manifest-path "${ROOT}/src/Cargo.toml" \
                  | jq -r 'first(.packages[] | select(.name=="cef") | .version)
                            // error("Cannot determine version")' \
                  | cut -d+ -f2)

get_attr() {
  local attr="$1"
  local default_system='${builtins.currentSystem}'
  local system="${2:-$default_system}"
  nix-instantiate --eval -E \
    "(builtins.getFlake \"${ROOT}\")
      .packages.${system}
      .${PACKAGE}.${attr}" \
  | tr -d '"'
}

current_version="$(get_attr 'version')"

echo "Target version : $target_version"
echo "Current version: $current_version"

if [[ "$target_version" == "$current_version" ]]; then
    echo "Package is up-to-date"
    exit 0
fi


version_json=$(curl --silent https://cef-builds.spotifycdn.com/index.json \
                | jq '[ .linux64.versions[]
                        | select (.channel == "stable")
                        | select (.cef_version | startswith("'"${target_version}"'"))
                      ][0]')

cef_version=$(echo "$version_json" | jq -r '.cef_version' | cut -d'+' -f1)
git_revision=$(echo "$version_json" | jq -r '.cef_version' | cut -d'+' -f2 | cut -c 2-)
chromium_version=$(echo "$version_json" | jq -r '.chromium_version')

echo "Latest version: $cef_version"

if [[ "$cef_version" == "$current_version" ]]; then
    echo "Package is up-to-date, rust crate cef has an unsupported version"
    exit 1
fi



update_nix_value() {
    local key="$1"
    local value="${2:-}"
    sed -i "s|$key = \".*\"|$key = \"$value\"|" "${PACKAGE_PATH}/${PACKAGE}.nix"
}

update_nix_value version "$cef_version"
update_nix_value gitRevision "$git_revision"
update_nix_value chromiumVersion "$chromium_version"

update_hashes () {
  local system="$1"
  local url="$(get_attr 'src.url' "${system}")"
  local hash=$(nix store prefetch-file --json "${url}" | jq -r .hash)
  update_nix_value "${system}" "${hash}"
}

update_hashes x86_64-linux
update_hashes aarch64-linux
