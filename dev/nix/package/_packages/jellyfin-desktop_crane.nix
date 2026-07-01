{
  craneLib,
  craneCommonArgs,
  rustPlatform,
  pkg-config,
  wrapGAppsHook4,

  # Dependencies
  jellyfin-desktop_crane-deps,
  jellyfin-desktop_resources,

  jellyfin-desktop_nixpkgs,
}:
craneLib.buildPackage (
  craneCommonArgs
  // {

    nativeBuildInputs = [
      wrapGAppsHook4
      pkg-config
      rustPlatform.bindgenHook
    ];

    cargoArtifacts = jellyfin-desktop_crane-deps;

    installPhase = ''
      runHook preInstall

      install -Dm755 \
        target/release/jellyfin-desktop \
        $out/bin/jellyfin-desktop

      install -Dm644 \
        ${jellyfin-desktop_resources}/linux/org.jellyfin.JellyfinDesktop.desktop \
        $out/share/applications/org.jellyfin.JellyfinDesktop.desktop
      install -Dm644 \
        ${jellyfin-desktop_resources}/linux/org.jellyfin.JellyfinDesktop.metainfo.xml \
        $out/share/metainfo/org.jellyfin.JellyfinDesktop.metainfo.xml
      install -Dm644 \
        ${jellyfin-desktop_resources}/linux/org.jellyfin.JellyfinDesktop.svg \
        $out/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg

      runHook postInstall
    '';

    inherit (jellyfin-desktop_nixpkgs) preFixup;
  }
)
