{
  mkShell,
  lib,
  stdenv,
  alsa-lib,
  at-spi2-atk,
  at-spi2-core,
  atk,
  cairo,
  cargo,
  clippy,
  cups,
  dbus,
  expat,
  ffmpeg,
  git,
  glib,
  just,
  libass,
  libdrm,
  libgbm,
  libGL,
  libplacebo,
  libXScrnSaver,
  libx11,
  libxcb,
  libxcomposite,
  libxdamage,
  libxext,
  libxfixes,
  libxkbcommon,
  libXpresent,
  libxrandr,
  meson,
  ninja,
  nspr,
  nss,
  pango,
  pkg-config,
  python3,
  rustPlatform,
  rustc,
  rustfmt,
  systemd,
  vulkan-loader,
  wayland,
  wayland-protocols,
  wayland-scanner,
}:

let
  mpvBuildInputs = [
    ffmpeg
    libass
    libdrm
    libgbm
    libGL
    libplacebo
    libXScrnSaver
    libx11
    libxext
    libxkbcommon
    libXpresent
    libxrandr
    vulkan-loader
    wayland
    wayland-protocols
    wayland-scanner
  ];

  cefRuntimeLibraries = [
    alsa-lib
    at-spi2-atk
    at-spi2-core
    atk
    cairo
    cups
    dbus
    expat
    glib
    libgbm
    libx11
    libxcomposite
    libxdamage
    libxext
    libxfixes
    libxrandr
    nspr
    nss
    pango
    systemd
  ];

  appLinkLibraries = [
    libxcb
    libxkbcommon
  ];

  runtimeLibraryPath = lib.makeLibraryPath (
    cefRuntimeLibraries
    ++ mpvBuildInputs
    ++ appLinkLibraries
  );
in
mkShell {
  name = "jellyfin-desktop";

  packages = [
    cargo
    clippy
    git
    just
    meson
    ninja
    pkg-config
    python3
    rustPlatform.bindgenHook
    rustc
    rustfmt
  ]
  ++ lib.optionals stdenv.hostPlatform.isLinux (
    mpvBuildInputs
    ++ cefRuntimeLibraries
    ++ appLinkLibraries
  );

  # Expose the runtime libary path so that `just run` can find the required libaries to run the app.
  shellHook = lib.optionalString stdenv.hostPlatform.isLinux ''
    export LD_LIBRARY_PATH="${runtimeLibraryPath}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    if [ -z "''${XDG_RUNTIME_DIR:-}" ] || [ ! -w "''${XDG_RUNTIME_DIR:-}" ]; then
      export XDG_RUNTIME_DIR="/tmp"
    fi
  '';
}
