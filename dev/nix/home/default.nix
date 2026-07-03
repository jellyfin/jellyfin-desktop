{ inputs, config, ... }:
let
  packages = config.flake.packages;
in
{
  imports = [ inputs.home-manager.flakeModules.home-manager ];
  flake.homeModules = rec {
    default = jellyfin-desktop;
    jellyfin-desktop =
      {
        config,
        lib,
        pkgs,
        ...
      }:
      let
        cfg = config.programs.jellyfin-desktop;
        inherit (lib)
          mkEnableOption
          mkOption
          mkMerge
          mkIf
          types
          literalExpression
          ;

        inherit (types) nullOr;

        jsonFormat = pkgs.formats.json { };

      in
      {
        options.programs.jellyfin-desktop = {
          cache.enable = mkEnableOption "Enable jellyfin cache";
          enable = mkEnableOption "Enable jellyfin-desktop";
          package = mkOption {
            description = "Package for jellyfin-desktop";
            type = types.package;
            default = packages.${pkgs.stdenv.hostPlatform.system}.jellyfin-desktop;
            defaultText = literalExpression "<flake>.packages.$system.jellyfin-desktop";
          };
          settings =
            let
              warningSuffix = "(if set, this option will overwrite settings on each generation)";
            in
            (lib.mapAttrs
              (
                _: v:
                v
                // {
                  type = nullOr v.type;
                  description = "${v.description} ${warningSuffix}.";
                  default = null;
                }
              )
              {
                serverUrl = mkOption {
                  description = "Server URL";
                  type = types.str;
                  example = "https://myserver.mydomain.tld";
                };

                transparentTitlebar = mkOption {
                  description = "Whether to enable transparent title bar";
                  type = types.bool;
                  example = true;
                };

                hideScrollbar = mkOption {
                  description = "Whether to hide scrollbar";
                  type = types.bool;
                  example = true;
                };

                deviceName = mkOption {
                  description = "Device Name";
                  type = types.str;
                  example = "myhost (jellyfin-desktop)";
                };

              }
            );
          extraConfig = mkOption {
            description = ''
              Extra configuration to append to `settings.json`.
              Translated from nix to JSON.
              Values defined here overwrite those in settings.
            '';
            type = jsonFormat.type;
            default = { };
            example = {
              someOption = true;
              someOtherOption = false;
            };
          };
        };

        config = mkMerge [
          (mkIf cfg.enable {
            home.packages = [
              # don't use the overlay as there's already a "jellyfin-desktop" package on nixpkgs.
              cfg.package
            ];

            home.activation.mergeJellyfinDesktopConfiguration =
              let
                nonNullSettings = (lib.attrsets.filterAttrs (_: v: v != null) cfg.settings) // cfg.extraConfig;
                newSettingsFile = pkgs.writeText "jfnd-settings.json" (builtins.toJSON nonNullSettings);
                target = "${config.xdg.configHome}/jellyfin-desktop/settings.json";
                jq = lib.getExe pkgs.jq;
              in
              (mkIf (nonNullSettings != { }) (
                lib.hm.dag.entryAfter [ "writeBoundary" ] ''
                  if [ -f "${target}" ]; then
                    ${jq} -s '.[0] * .[1]' "${target}" "${newSettingsFile}" > "${target}.hm-tmp.json"
                    chmod --reference="${target}" "${target}.hm-tmp.json"
                    chown --reference="${target}" "${target}.hm-tmp.json"
                    $DRY_RUN_CMD mv "${target}.hm-tmp.json" "${target}"
                  else
                    $DRY_RUN_CMD mkdir -p "$(dirname "${target}")"
                    $DRY_RUN_CMD cp "${newSettingsFile}" "${target}"
                    $DRY_RUN_CMD chmod u+rw "${target}"
                  fi
                ''
              ));
          })
          (mkIf cfg.cache.enable {
            nix.settings = {
              extra-substituters = [ "https://xaltsc-jfnd.cachix.org" ];
              extra-trusted-public-keys = [ "xaltsc-jfnd.cachix.org-1:cCD4MB/Hqw1ktSbT+Dtv0clFpK1/YksbIQExL1hBxqo=" ];
            };
          })
        ];
      };
  };
}
