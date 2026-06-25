{
  lib,
  rustPlatform,
  pkg-config,
  wrapGAppsHook4,

  # Needed at runtime by CEF
  libGL,

  # Dependencies
  ffmpeg,
  libxkbcommon,
  libxcb,
  cef-lib,
  mpv-external-prefix,

  lastModifiedDate,
}:
rustPlatform.buildRustPackage (finalAttrs: {
  src = ../../../..;
  pname = "jellyfin-desktop";
  version =
    let
      majorVersion =
        (lib.importTOML "${finalAttrs.src}/${finalAttrs.cargoRoot}/Cargo.toml").workspace.package.version;

      s = b: l: builtins.substring b l lastModifiedDate;
      date = "${s 0 4}-${s 4 2}-${s 6 2}";
    in
    "${majorVersion}-${date}";

  # Fixes some Cargo.lock issues
  cargoRoot = "src";
  cargoHash = "sha256-GqSk6ZjY34esHGBmaY7sbFjQI6q9e4J3Qu87tFEW6O0=";
  cargoLock = {
    # Fixes some other Cargo.lock issues
    lockFile = "${finalAttrs.src}/${finalAttrs.cargoRoot}/Cargo.lock";
    outputHashes = {
      "wl-proxy-0.1.2" = "sha256-8NMNPhBSW2gLXc9bwyg2kmHb12XIaV6b4PjM62xLldQ=";
    };
  };

  strictDeps = true;

  nativeBuildInputs = [
    wrapGAppsHook4
    rustPlatform.bindgenHook # fixes clang issues
    pkg-config
  ];

  buildInputs = [
    libxcb
    libxkbcommon
    ffmpeg
  ];

  buildPhase = ''
    runHook preBuild
    cargo xtask build \
      --cef-path ${cef-lib} \
      --external-mpv ${mpv-external-prefix} \
      --out build/
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 \
      build/jellyfin-desktop \
      $out/bin/jellyfin-desktop

    install -Dm644 \
      resources/linux/org.jellyfin.JellyfinDesktop.desktop \
      $out/share/applications/org.jellyfin.JellyfinDesktop.desktop
    install -Dm644 \
      resources/linux/org.jellyfin.JellyfinDesktop.metainfo.xml \
      $out/share/metainfo/org.jellyfin.JellyfinDesktop.metainfo.xml
    install -Dm644 \
      resources/linux/org.jellyfin.JellyfinDesktop.svg \
      $out/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg

    runHook postInstall
  '';

  preFixup = ''
    gappsWrapperArgs+=(
      --prefix LD_LIBRARY_PATH : "${lib.makeLibraryPath [ libGL ]}" \
    )
  '';

  doCheck = false;

  meta = {
    description = "Jellyfin desktop client";
    homepage = "https://github.com/jellyfin/jellyfin-desktop";
    license = lib.licenses.gpl2Only;
    mainProgram = "jellyfin-desktop";
    # TODO: add myself once this goes on nixpkgs.
    maintainers = with lib.maintainers; [
      {
        email = "hey+dev@xaltsc.dev";
        name = "xaltsc";
        github = "xaltsc";
        githubId = 41400742;
      }
    ];
  };
})
