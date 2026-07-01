This repository provides, for Nix users, packages, an overlay overriding `nixpkgs`'s
`jellyfin-desktop`, as well as a `home-manager` module.

**Note**: everything said in this file is untested on foreign distributions and on other
architectures than `x86_64-linux` or on OS X with [`nix-darwin`](https://github.com/nix-darwin/nix-darwin).
For foreign distributions, you may want to use [`nixGL`](https://github.com/nix-community/nixgl)
because of the issues arising from the use of `nix`-provided `libGL`.
If you come across issues on foreign distributions, OS X or other architectures, please report them
on the [bug tracker](https://github.com/jellyfin/jellyfin-desktop/issues).

# Flake usage

Simply add this to your `flake.nix` inputs:
```nix
inputs = {
  jellyfin-desktop = {
    url = "github:jellyfin/jellyfin-desktop";
    inputs.nixpkgs.follows = "nixpkgs";
  };
};
```
And it should be available in your inputs.

## Cache usage

There is a cache available at [`jellyfin.cachix.org`](https://jellyfin.chachix.org). To use is, in your `flake.nix`,
add the following to your `nixConfig` and accept the configuration on `rebuild`. Caches distribute
binaries that are compiled by us, so us it at your own assesment of the risk it represents.

```nix
nixConfig = {
  extra-substituters = [
    "https://jellyfin.cachix.org"
  ];
  extra-trusted-public-keys = [
    "jellyfin-desktop.cachix.org-1:SOMEPLACEHOLDERHASH"
  ];
};
```

There is also an option `programs.jellyfin-desktop.cache.enable` in the [Home-Manager module](#home-manager-module),
but it only affects those using Home-Manager outside of a global NixOS config.

## Installing the package

You can install the package directly, by you might want to use the `home-manager` module instead,
cf. [below](#home-manager-module).

### Overlay use
Simply put somewhere in either your `nixosConfiguration` of `home-manager` configuration.
```nix
nixpkgs.overlays = [ inputs.jellyfin-desktop.overlays.default ];
```

The `pkgs.jellyfin-desktop` package now points to the main package provided by this flake.

### System-wide installation

Without the overlay, use the following in your `nixosConfiguration`:

```nix
environment.systemPackages = [
  inputs.jellyfin-desktop.packages.YOUR_ARCH.jellyfin-desktop
];
```

### Home-Manager installation

Likewise, even though you might prefer using the [module](#home-manager-module), in your
`home-manager` configuration, put:
```nix
home.packages = [
  inputs.jellyfin-desktop.packages.YOUR_ARCH.jellyfin-desktop
];
```

# Usage without flakes

TODO

# Home-Manager Module

Example usage of the `home-manager` is as below:
```nix
{ pkgs, lib, inputs, ...}: {
    imports = [ inputs.jellyfin-desktop.homeModules.default ];
    config = {
        programs.jellyfin-desktop = {

            # Enable jellyfin-desktop and use of this module
            enable = true;

            # Enable the cache
            cache.enable = false;

            # Optional. Defaults to the main package package of the flake.
            # In the future, this will also be used to get the list of settings.
            package = somePackage;

            # Currently settings are manually kept in sync with available settings and merged
            # with the ~/.config/jellyfin-desktop/settings.json file existing values,
            # overriding them if needed.
            # Unsetting an option here does not remove it from settings.json.
            # All settings are optional (lib.types.nullOr).
            # If unset, they produce no change in settings.json
            settings = {
                # Server URL (string)
                serverUrl = "https://jellyfin.domain.tld";

                # Device Name (string)
                deviceName = "MYHOST";

                # Enable transparent title bar (bool)
                transparentTitlebar = true;

                # Hide scrollbars (bool)
                hideScrollBar = true;
            };

            # This options provide an escape hatch to cover options not handled by settings.
            # Values defined here take precedence over settings.
            # The type of this option is a free-form attrset that must be convertible to JSON.
            extraConfig = {
                whatever = {
                    you.want = "json valid value";
                };
            };
        };
    };
}
```

Refer to [`home/default.nix`](./home/default.nix) for more details.


# Development

There is a `devShell` availble TODO.
