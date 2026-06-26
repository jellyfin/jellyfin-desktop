{
  lib,
  fetchFromGitHub,
  rustPlatform,
  stdenv,
  symlinkJoin,
  pkg-config,
  wrapGAppsHook4,

  # Needed at runtime by CEF
  libGL,

  # Updated CEF
  new-cef,

  # Dependencies
  ffmpeg,
  mpv-unwrapped,
  cef-binary,
  libxkbcommon,
  libxcb,
}:
let
  mpvPrefix = symlinkJoin {
    name = "mpv-external-prefix";
    paths = [
      (lib.getDev mpv-unwrapped)
      (lib.getLib mpv-unwrapped)
    ];
  };

  src = ../../..;

  cef-required-version =
    let
      lockFile = lib.importTOML "${src}/src/Cargo.lock";
      cefPackage = lib.findFirst (p: p.name == "cef") { version = "+FAILED"; } lockFile.package;
    in
    builtins.elemAt (lib.splitString "+" cefPackage.version) 1;

  # Assuming new-cef version always matches this repo needed version
  # If it's the same version, then avoid unnecessary compilation and use the provided one.
  cef-bin = if cef-required-version != cef-binary.version then new-cef else cef-binary;

  # Jellyfin expects CEF in a certain layout.
  # Cf the Stremio package for the same issue.
  # Can't symlinkJoin here though because CEF uses the realpaths to determine icudtl.dat path
  # Trivial compilation and should stay correctly linked.
  # There's likely a Rust issue that is the reason why for the fixup.
  cef = stdenv.mkDerivation (finalAttrs: {
    name = "cef-for-jellyfin";
    dontUnpack = true;
    installPhase = ''
      mkdir -p $out
      cp -r ${cef-bin}/Release/* $out/
      cp -r ${cef-bin}/Resources/* $out/
    '';
  });

in
rustPlatform.buildRustPackage (finalAttrs: {
  inherit src;
  pname = "jellyfin-desktop";
  version = "3.0.0-unstable-2026-06-18";

  # Fixes some Cargo.lock issues
  cargoRoot = "src";
  cargoHash = "sha256-GqSk6ZjY34esHGBmaY7sbFjQI6q9e4J3Qu87tFEW6O0=";
  cargoLock = {
    # Fixes some other Cargo.lock issues
    lockFile = "${finalAttrs.src}/src/Cargo.lock";
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
      --cef-path ${cef} \
      --external-mpv ${mpvPrefix} \
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
