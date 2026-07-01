{ cef-binary }:
let
  version = "149.0.4";
in
(
  if (cef-binary.version == version) then
    cef-binary
  else
    (cef-binary.override {
      inherit version;
      gitRevision = "2f1bfd8";
      chromiumVersion = "149.0.7827.156";
      srcHashes = {
        aarch64-linux = "sha256-iQmnlonux7I+2ACEtpdmlS1E4A+aNFgylRsykD+KgKA=";
        x86_64-linux = "sha256-bUNgdnXkfta/pA0c/OE20E53IFKfjxxENdS6Hc0YObI=";
      };
    })
).overrideAttrs
  (old: {
    # make nix understand that src and version are defined in this file
    inherit (old) src version;

    passthru = old.passthru // {
      updateScript = ./update-cef.sh;
    };
  })
