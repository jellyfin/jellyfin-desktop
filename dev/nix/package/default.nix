{ inputs, self, ... }: {
  imports = [
    inputs.flake-parts.flakeModules.easyOverlay
  ];
  perSystem =
    {
      pkgs,
      lib,
      config,
      ...
    }:
    {
      overlayAttrs = {
        inherit (config.packages) jellyfin-desktop;
      };
      packages = (
        lib.fix (
          p:
          (lib.mapAttrs (n: d: pkgs.callPackage ./_packages/${n}.nix d) {
            cef-binary = { };
            cef-lib = { inherit (p) cef-binary; };
            jellyfin-desktop = {
              inherit (p) cef-lib mpv-external-prefix;
              inherit (self) lastModifiedDate;
            };
            mpv-external-prefix = { };
          })
          // {
            default = p.jellyfin-desktop;
          }
        )
      );
    };
}
