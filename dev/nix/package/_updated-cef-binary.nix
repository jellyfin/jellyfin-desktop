{ cef-binary }:
(cef-binary.override {
  version = "149.0.4";
  gitRevision = "2f1bfd8";
  chromiumVersion = "149.0.7827.156";
  srcHashes = {
    aarch64-linux = "sha256-iQmnlonux7I+2ACEtpdmlS1E4A+aNFgylRsykD+KgKA=";
    x86_64-linux = "sha256-bUNgdnXkfta/pA0c/OE20E53IFKfjxxENdS6Hc0YObI=";
  };
}).overrideAttrs
  (old: {
    passthru.updateScript = ./_update-cef.sh;
  })
