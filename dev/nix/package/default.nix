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
      packages =
        let
          craneLib = inputs.crane.mkLib pkgs;
        in
        (lib.fix (
          p:
          let
            metaSkeleton = {
              inherit (p.jellyfin-desktop_nixpkgs.meta)
                homepage
                license
                maintainers
                description
                ;
            };
            craneCommonArgs = {
              src = ../../../src;
              strictDeps = true;

              inherit (p.jellyfin-desktop_nixpkgs)
                version
                pname
                passthru
                buildInputs
                ;

              CEF_PATH = p.cef-lib;
              EXTERNAL_MPV_DIR = p.mpv-external-prefix;

              cargoExtraArgs = "--bin jellyfin-desktop";

              meta = metaSkeleton;
            };

          in
          (lib.mapAttrs (n: pkgs.callPackage ./_packages/${n}.nix) {
            cef-binary = { };
            cef-lib = { inherit (p) cef-binary; };
            jellyfin-desktop_resources = {
              inherit (p) jellyfin-desktop_nixpkgs;
              inherit metaSkeleton;
            };
            jellyfin-desktop_nixpkgs = {
              inherit (p) cef-lib mpv-external-prefix;
              inherit (self) lastModifiedDate;
            };
            jellyfin-desktop_crane-deps = {
              inherit craneLib craneCommonArgs metaSkeleton;
            };
            jellyfin-desktop_crane = {
              inherit craneLib craneCommonArgs;
              inherit (p) jellyfin-desktop_crane-deps jellyfin-desktop_resources jellyfin-desktop_nixpkgs;
            };
            mpv-external-prefix = { };
          })
          // {
            jellyfin-desktop = p.jellyfin-desktop_crane;
            default = p.jellyfin-desktop;
          }
        ));
    };
}
