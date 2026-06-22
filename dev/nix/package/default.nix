{ inputs, ... }: {
  imports = [
    inputs.flake-parts.flakeModules.easyOverlay
  ];

  perSystem =
    { pkgs, config, ... }:
    {
      overlayAttrs = {
        inherit (config.packages) jellyfin-desktop;
      };
      packages = rec {
        jellyfin-desktop = pkgs.callPackage ./_package.nix { new-cef = cef-binary; };
        cef-binary = pkgs.callPackage ./_updated-cef-binary.nix { };
        default = jellyfin-desktop;
      };
    };
}
