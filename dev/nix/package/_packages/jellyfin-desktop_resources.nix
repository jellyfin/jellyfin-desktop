{
  stdenv,
  jellyfin-desktop_nixpkgs,
  metaSkeleton,
}:
stdenv.mkDerivation (finalAttrs: {
  src = ../../../../resources;
  pname = "${jellyfin-desktop_nixpkgs.pname}-resources";
  inherit (jellyfin-desktop_nixpkgs) version;
  dontUnpack = true;
  installPhase = ''
    cp -r $src $out
  '';
  meta = metaSkeleton;
})
