{
  description = "Jellyfin Desktop Client (CEF + mpv)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # The project requires a customized mpv that exposes wayland-display and
    # wayland-surface as observable mpv properties so CEF can composite its
    # overlay on top of mpv's video surface. Vanilla mpv does NOT have these
    mpv-cef-src = {
      url = "github:andrewrabert/mpv/aa910a7";
      flake = false;
    };
  };
  outputs =
    {
      self,
      nixpkgs,
      mpv-cef-src,
    }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      lib = pkgs.lib;

      appVersion = lib.fileContents ./VERSION;

      # CEF_VERSION format: "147.0.10+gd58e84d+chromium-147.0.7727.118"
      cefVersionString = lib.fileContents ./CEF_VERSION;
      cefMatch =
        let
          m = builtins.match "([0-9.]+)\\+g([0-9a-f]+)\\+chromium-([0-9.]+)" cefVersionString;
        in
        if m == null then throw "Failed to parse CEF_VERSION: ${cefVersionString}" else m;

      # CEF binary distribution (just the unpacked Spotify CDN tarball)
      cef = pkgs.cef-binary.override {
        version = builtins.elemAt cefMatch 0;
        gitRevision = builtins.elemAt cefMatch 1;
        chromiumVersion = builtins.elemAt cefMatch 2;
        srcHashes.x86_64-linux = "sha256-CHzPofBDhCniDZEpOxXK4I7p57SYjMAY1HVo3Vna0e8=";
      };

      # ----------------------------------------------------------------------
      # CEF + a pre-built libcef_dll_wrapper.a
      # ----------------------------------------------------------------------
      # CEF distributes libcef_dll_wrapper as source under libcef_dll/, not
      # as a pre-built static library. The standard CEF integration pattern
      # is `add_subdirectory(${CEF_ROOT}/libcef_dll libcef_dll_wrapper)`,
      # but jellyfin-desktop's custom FindCEF.cmake expects to find a
      # pre-built `lib/libcef_dll_wrapper.a` instead. Build it here so we
      # only do it once and cache the result.
      cefWithWrapper = pkgs.stdenv.mkDerivation {
        pname = "cef-with-wrapper";
        version = cef.version or (builtins.elemAt cefMatch 0);

        # Use cef-binary's output directly as the source.
        src = cef;
        dontUnpack = true;
        # We invoke cmake ourselves in buildPhase against the cef-binary
        # source tree; suppress the default cmake configure that would
        # otherwise run in /build (which has no CMakeLists.txt).
        dontUseCmakeConfigure = true;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
        ];

        buildPhase = ''
          runHook preBuild

          # CEF's bundled CMakeLists writes into the source tree, so we
          # need a writable copy.
          cp -rL $src cef-root
          chmod -R u+w cef-root

          # Configure the CEF distribution and build only the wrapper —
          # skip the bundled cefclient/cefsimple test apps.
          cmake -G Ninja -S cef-root -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
          cmake --build build --target libcef_dll_wrapper

          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall

          # Mirror the original cef-binary contents and add the wrapper
          # under lib/ where jellyfin-desktop's FindCEF.cmake looks for it.
          cp -rL cef-root $out
          chmod -R u+w $out
          mkdir -p $out/lib

          # The wrapper's exact build path varies by CEF version; just
          # find it.
          wrapper=$(find build -name 'libcef_dll_wrapper.a' -print -quit)
          if [ -z "$wrapper" ]; then
            echo "ERROR: libcef_dll_wrapper.a not found after build" >&2
            find build -name '*.a' >&2 || true
            exit 1
          fi
          cp "$wrapper" $out/lib/libcef_dll_wrapper.a


          # CEF's runtime files (libcef.so, libEGL.so, libGLESv2.so,
          # libvk_swiftshader.so, *.pak, icudtl.dat, v8_context_snapshot.bin,
          # vk_swiftshader_icd.json, locales/, ...) live under Release/ in
          # the tarball (with some overlap into Resources/ depending on
          # platform). jellyfin-desktop's FindCEF.cmake expects all of
          # these at {CEF_ROOT}/lib alongside the wrapper.
          #
          # Mirror them as REAL FILES (not symlinks) via `cp -rL`. Symlinks
          # would survive into downstream cmake `install` rules and end up
          # as dangling links because cmake preserves relative symlink
          # targets verbatim when copying. This costs disk in this
          # intermediate derivation but keeps consumers correct.
          for srcdir in Release Resources; do
            [ -d "$out/$srcdir" ] || continue
            cp -rLn "$out/$srcdir/." "$out/lib/"
          done

          runHook postInstall
        '';

        # Avoid stripping the static archive - it can confuse downstream
        # linkers if the symbol table is touched.
        dontStrip = true;
      };

      # Bump libplacebo to 7.360.1 (cef-mpv requires >=7.360.1; nixpkgs has 7.351.0)
      libplacebo-pinned = pkgs.libplacebo.overrideAttrs (old: rec {
        version = "7.360.1";
        src = pkgs.fetchFromGitHub {
          owner = "haasn";
          repo = "libplacebo";
          rev = "v${version}";
          fetchSubmodules = true;
          hash = "sha256-2F3eUKjvAveahvqKuJFwHvIem9g156hCeKbeYBPovLk=";
        };
        patches = [ ];
      });

      # ----------------------------------------------------------------------
      # mpv (custom fork — andrewrabert/mpv on the cef-mpv branch)
      # ----------------------------------------------------------------------
      # This fork adds `wayland-display` and `wayland-surface` mpv properties
      # that jellyfin-desktop reads at startup. Vanilla mpv-unwrapped does
      # NOT work - runtime fails with "Failed to get Wayland display/surface
      # from mpv"
      mpv =
        (pkgs.mpv-unwrapped.override {
          libplacebo = libplacebo-pinned;
        }).overrideAttrs
          (old: {
            pname = "mpv-cef";
            # mpv reads its version from a generated header that falls back to
            # "0.41.0-UNKNOWN" when there's no .git dir (which there isn't,
            # since we're building from a tarball-style flake input). Match
            # what `mpv --help` actually prints so nixpkgs' versionCheckHook
            # doesn't fail. The cef-mpv branch fact is encoded in pname.
            version = "0.41.0-UNKNOWN";
            src = mpv-cef-src;

            patches = [ ];

            # Matching `version` to mpv's self-report keeps the version check
            # passing on this build, but if upstream's printed string ever
            # drifts we'd rather skip the check than chase it.
            doInstallCheck = false;
          });


      nativeBuildDeps = with pkgs; [
        cmake
        ninja
        pkg-config
        python3
        wayland-scanner
        makeWrapper
      ];

      buildDeps = with pkgs; [
        # Wayland stack
        wayland
        wayland-protocols
        kdePackages.plasma-wayland-protocols
        libxkbcommon
        libdrm

        # X11 stack
        libxcb
        xcbutil
        xcbutilcursor
        libXrandr
        libX11

        # System libs
        systemdLibs
        libGL
        mesa # libgbm, libEGL

        # Chromium runtime requirements (CEF DT_NEEDED)
        alsa-lib
        atk
        at-spi2-core
        at-spi2-atk
        cairo
        cups
        dbus
        expat
        fontconfig
        freetype
        gdk-pixbuf
        glib
        gtk3
        nspr
        nss
        pango
        libxcomposite
        libxdamage
        libxext
        libxfixes
        libxkbfile
        libxshmfence
        libxtst
        libxi
        libxrender
        libpulseaudio
      ];

      # Main package
      jellyfin-desktop = pkgs.stdenv.mkDerivation (finalAttrs: {
        pname = "jellyfin-desktop";
        version = appVersion;

        src = self;

        nativeBuildInputs = nativeBuildDeps ++ [ pkgs.autoPatchelfHook ];

        buildInputs = buildDeps ++ [
          cefWithWrapper # CEF + pre-built wrapper
          mpv
        ];

        # The CMakeLists hard-codes /usr/share/plasma-wayland-protocols
        # and /app/share/...; redirect to the Nix store path.
        postPatch = ''
          substituteInPlace CMakeLists.txt \
            --replace-fail \
              "PATHS /usr/share/plasma-wayland-protocols" \
              "PATHS ${pkgs.kdePackages.plasma-wayland-protocols}/share/plasma-wayland-protocols"
        '';

        cmakeFlags = [
          "-DEXTERNAL_CEF_DIR=${cefWithWrapper}"
          "-DEXTERNAL_MPV_DIR=${mpv}"
          "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}/libexec/jellyfin-desktop"
        ];

        postInstall = ''
          # The project's CMakeLists installs libmpv.so as a symlink to
          # libmpv.so.2, but the actual file lives in mpv's store path and
          # isn't copied alongside. The binary loads libmpv.so.2 via RPATH
          # directly from mpv's store path (autoPatchelfHook handles that),
          # so this local symlink is unused - delete it so the broken-
          # symlinks check passes.
          rm -f $out/libexec/jellyfin-desktop/libmpv.so

          mkdir -p $out/bin
          makeWrapper \
            $out/libexec/jellyfin-desktop/jellyfin-desktop \
            $out/bin/jellyfin-desktop \
            --prefix PATH : ${lib.makeBinPath [ pkgs.xdg-utils ]} \
            --prefix LD_LIBRARY_PATH : ${
              lib.makeLibraryPath [
                pkgs.libGL
                pkgs.mesa
              ]
            }
            # cmake's installPhase leaves cwd at the build subdirectory, so
            # reference the source via $src (the flake source store path)
            # rather than relative paths.
            install -Dm644 $src/resources/linux/org.jellyfin.JellyfinDesktop.desktop \
              $out/share/applications/org.jellyfin.JellyfinDesktop.desktop
            install -Dm644 $src/resources/linux/org.jellyfin.JellyfinDesktop.svg \
              $out/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg
            install -Dm644 $src/resources/linux/org.jellyfin.JellyfinDesktop.metainfo.xml \
              $out/share/metainfo/org.jellyfin.JellyfinDesktop.metainfo.xml
        '';

        meta = {
          description = "Jellyfin Desktop Client (CEF + mpv)";
          homepage = "https://github.com/jellyfin/jellyfin-desktop";
          license = lib.licenses.gpl2Only;
          platforms = [
            "x86_64-linux"
          ];
          mainProgram = "jellyfin-desktop";
        };
      });
    in
    {
      packages.${system} = {
        default = jellyfin-desktop;
        inherit jellyfin-desktop cef cefWithWrapper;
      };

      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs =
          nativeBuildDeps
          ++ (with pkgs; [
            just
            git
          ]);
        buildInputs = buildDeps ++ [
          cefWithWrapper
          mpv
        ];

        EXTERNAL_CEF_DIR = "${cefWithWrapper}";
        EXTERNAL_MPV_DIR = "${mpv}";

        shellHook = ''
          cat <<'EOF'
          jellyfin-desktop dev shell
            EXTERNAL_CEF_DIR  -> CEF + pre-built libcef_dll_wrapper.a
            EXTERNAL_MPV_DIR  -> mpv built from the cef-mpv fork
          EOF
        '';
      };

      apps.${system}.default = {
        type = "app";
        program = "${jellyfin-desktop}/bin/jellyfin-desktop";
      };
    };
}
