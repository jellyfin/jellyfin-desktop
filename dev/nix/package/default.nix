{ inputs, ... }: {
  imports = [
    inputs.flake-parts.flakeModules.easyOverlay
  ];

  perSystem =
    {
      system,
      pkgs,
      config,
      ...
    }:
    {
      overlayAttrs = {
        inherit (config.packages) jellyfin-desktop;
      };
      packages = rec {
        jellyfin-desktop = pkgs.callPackage ./_package.nix {
          inherit (inputs.cef-update.legacyPackages.${system}) cef-binary;
        };
        default = jellyfin-desktop;
      };
    };
}
