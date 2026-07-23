{ config, ... }:
let
  flake = config.flake;
in
{
  perSystem =
    {
      system,
      pkgs,
      ...
    }:
    {
      devShells = rec {
        default = dev;
        dev =
          with pkgs;
          mkShell {
            strictDeps = true;
            nativeBuildInputs = [
              meson # mpv? shouldn't need it
              cmake # mpv? shouldn't need it
              cargo
              rustc
              rustfmt
              clippy
              rustPlatform.bindgenHook
              pkg-config
            ];

            packages = [
              just
              rust-analyzer

              lldb
              strace

              (writeShellApplication {
                name = "sync-rust-nix-cef";
                runtimeInputs = [ nix ];
                text = ../package/_update-cef.sh;
              })

            ];

            inputsFrom = with flake.packages.${system}; [
              jellyfin-desktop
              cef-binary

              # TODO: remove
              mpv-unwrapped
            ];

            shellHook = ''
              [ -f .env ] && source .env
              export JELLYFIN_DESKTOP_LOG_LEVEL="''${JELLYFIN_DESKTOP_LOG_LEVEL:-debug}"
              export JELLYFIN_DESKTOP_LOG_FILE="''${JELLYFIN_DESKTOP_LOG_FILE:-build/run.log}"
              export JFN_MPV_INCLUDE_DIR="${mpv}/include/mpv"
              export CEF_PATH="${flake.packages.${system}.cef-binary}"
            '';

            EXAMPLE_VARIABLE = "This is set from nix!";
          };
      };
    };
}
