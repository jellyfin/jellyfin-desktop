{
  description = "Jellyfin Desktop Client";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs-stable.url = "github:NixOS/nixpkgs/nixos-26.05";

    flake-parts.url = "github:hercules-ci/flake-parts";
    import-tree.url = "github:denful/import-tree";

    crane.url = "github:ipetkov/crane";
    home-manager.url = "github:nix-community/home-manager";
  };

  nixConfig = {
    extra-substituters = [
      "https://xaltsc-jfnd.cachix.org"
    ];
    extra-trusted-public-keys = [
      "xaltsc-jfnd.cachix.org-1:cCD4MB/Hqw1ktSbT+Dtv0clFpK1/YksbIQExL1hBxqo="
    ];
  };

  outputs =
    inputs@{ flake-parts, import-tree, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } (import-tree ./dev/nix);
}
