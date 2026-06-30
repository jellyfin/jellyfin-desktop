{ stdenv, cef-binary }:
# Jellyfin expects CEF in a certain layout.
# Cf the Stremio package for the same issue.
# Can't symlinkJoin here though because CEF uses the realpaths to determine icudtl.dat path
# Trivial compilation and should stay correctly linked.
# There's likely a Rust issue that is the reason why for the fixup.
stdenv.mkDerivation (finalAttrs: {
  pname = "cef-lib";
  inherit (cef-binary) version;
  dontUnpack = true;
  installPhase = ''
    mkdir -p $out
    cp -r ${cef-binary}/Release/* $out/
    cp -r ${cef-binary}/Resources/* $out/
  '';
})
