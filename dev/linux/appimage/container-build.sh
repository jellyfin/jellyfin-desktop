#!/bin/sh
# Run inside the appimage build container (jellyfin-desktop-appimage:base).
# Bind mounts (set up by `just appimage build`):
#   /src           rw  -- repo root (CEF + submodules must be populated by host)
#   /build         rw  -- cmake/ninja/meson state, persists on host for incremental builds
#   /host-output   rw  -- .AppImage output dir
# Env:
#   VERSION        -- version string for the output filename
set -eu

: "${VERSION:?VERSION env must be set}"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  LD_SONAME=ld-linux-x86-64.so.2 ;;
    aarch64) LD_SONAME=ld-linux-aarch64.so.1 ;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

cd /src

# Build (cmake's mpv_build target invokes meson)
if [ ! -f /build/CMakeCache.txt ]; then
    cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SKIP_RPATH=1 \
        -Wno-dev \
        -S /src -B /build
fi
cmake --build /build
strip /build/*.so /build/jellyfin-desktop

BUILD=/build

# -- AppDir assembly (ephemeral) --
APPDIR=/tmp/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share"

# Binary + CEF resources (CEF finds resources relative to /proc/self/exe)
cp "$BUILD"/jellyfin-desktop "$APPDIR/usr/bin/"
cp "$BUILD"/*.pak "$APPDIR/usr/bin/"
cp "$BUILD"/icudtl.dat "$APPDIR/usr/bin/"
cp "$BUILD"/v8_context_snapshot.bin "$APPDIR/usr/bin/"
cp -r "$BUILD"/locales "$APPDIR/usr/bin/"
cp "$BUILD"/vk_swiftshader_icd.json "$APPDIR/usr/bin/" 2>/dev/null || true

# CEF's own libs (ANGLE, SwiftShader) live in usr/bin/, separated from system
# GPU libs in usr/lib/ which get removed below.
cp "$BUILD"/libcef.so "$APPDIR/usr/bin/"
cp "$BUILD"/libEGL.so "$APPDIR/usr/bin/"
cp "$BUILD"/libGLESv2.so "$APPDIR/usr/bin/"
cp "$BUILD"/libvk_swiftshader.so "$APPDIR/usr/bin/"

# Ship the full system library stack (junest approach: glibc + ld-linux
# bundled so the AppImage works regardless of host glibc version).
# Fedora x86_64: shared libs live in /usr/lib64; /usr/lib holds noarch only.
cp -a /usr/lib64 "$APPDIR/usr/lib"

# mpv lib (cmake post-build copies it next to jellyfin-desktop)
cp "$BUILD"/libmpv.so.2 "$APPDIR/usr/lib/"

# Fedora's ffmpeg-free links GnuTLS; GnuTLS needs a system priority file from
# crypto-policies. Bundle the DEFAULT one; AppRun points GNUTLS_SYSTEM_PRIORITY_FILE at it.
mkdir -p "$APPDIR/usr/share/crypto-policies/DEFAULT"
cp /usr/share/crypto-policies/DEFAULT/gnutls.txt \
   "$APPDIR/usr/share/crypto-policies/DEFAULT/gnutls.txt"

# Desktop integration
mkdir -p "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/metainfo"
cp /src/resources/linux/org.jellyfin.JellyfinDesktop.desktop \
   "$APPDIR/usr/share/applications/"
cp /src/resources/linux/org.jellyfin.JellyfinDesktop.svg \
   "$APPDIR/usr/share/icons/hicolor/scalable/apps/"
cp /src/resources/linux/org.jellyfin.JellyfinDesktop.metainfo.xml \
   "$APPDIR/usr/share/metainfo/"

# Strip non-runtime cruft to reduce size
find "$APPDIR/usr/lib" -name '*.a' -delete
rm -rf "$APPDIR/usr/lib/pkgconfig" \
       "$APPDIR/usr/lib/cmake" \
       "$APPDIR/usr/lib/python"* \
       "$APPDIR/usr/lib/perl"* \
       "$APPDIR/usr/lib/ruby"* \
       "$APPDIR/usr/lib/node_modules" \
       "$APPDIR/usr/lib/gcc" \
       "$APPDIR/usr/lib/bfd-plugins" \
       "$APPDIR/usr/lib/ldscripts" \
       "$APPDIR/usr/lib/systemd" \
       "$APPDIR"/usr/lib/libLLVM* \
       "$APPDIR"/usr/lib/llvm* \
       "$APPDIR"/usr/lib/clang* \
       "$APPDIR"/usr/lib/LLVMgold* \
       "$APPDIR"/usr/lib/libLTO* \
       "$APPDIR"/usr/lib/libgallium* \
       "$APPDIR"/usr/lib/dri_gbm.so* \
       "$APPDIR"/usr/lib/gbm/dri_gbm.so* \
       "$APPDIR"/usr/lib/udev* \
       "$APPDIR"/usr/lib/guile* \
       "$APPDIR"/usr/lib/git* \
       "$APPDIR/usr/share/doc" \
       "$APPDIR/usr/share/man" \
       "$APPDIR/usr/share/info" \
       "$APPDIR/usr/share/gtk-doc" \
       "$APPDIR/usr/share/locale" \
       "$APPDIR/usr/share/i18n" \
       "$APPDIR/usr/share/help" \
       "$APPDIR/usr/share/bash-completion" \
       "$APPDIR/usr/share/zsh" \
       "$APPDIR/usr/share/fish" \
       "$APPDIR/usr/share/vala"

# Remove system GPU libs — must come from host (kernel driver match required).
# CEF's own ANGLE libs in usr/bin/ are not affected.
rm -rf "$APPDIR/usr/lib/dri" "$APPDIR/usr/lib/vdpau"
rm -f "$APPDIR"/usr/lib/libEGL.so* \
      "$APPDIR"/usr/lib/libEGL_mesa.so* \
      "$APPDIR"/usr/lib/libGL.so* \
      "$APPDIR"/usr/lib/libGLX.so* \
      "$APPDIR"/usr/lib/libGLX_mesa.so* \
      "$APPDIR"/usr/lib/libGLESv1_CM.so* \
      "$APPDIR"/usr/lib/libGLESv2.so* \
      "$APPDIR"/usr/lib/libGLdispatch.so* \
      "$APPDIR"/usr/lib/libOpenGL.so* \
      "$APPDIR"/usr/lib/libgbm.so* \
      "$APPDIR"/usr/lib/libvulkan.so* \
      "$APPDIR"/usr/lib/libdrm.so* \
      "$APPDIR"/usr/lib/libdrm_*.so* \
      "$APPDIR"/usr/lib/libxshmfence.so* \
      "$APPDIR"/usr/lib/libglapi.so*

# Flatten libs from subdirs (e.g. pulseaudio/, pipewire-*/) into usr/lib so
# AppRun's single --library-path resolves them all. Hard copies, not symlinks.
find "$APPDIR/usr/lib" -mindepth 2 \( -name '*.so' -o -name '*.so.*' \) | while read lib; do
    base="$(basename "$lib")"
    [ ! -e "$APPDIR/usr/lib/$base" ] && cp -L "$lib" "$APPDIR/usr/lib/$base"
done

# Some Arch packages embed absolute paths in DT_NEEDED (e.g. /usr/lib/libmujs.so).
# The dynamic linker follows literal paths even with --library-path, so rewrite
# them to bare sonames.
find "$APPDIR/usr/lib" "$APPDIR/usr/bin" -type f \( -name '*.so*' -o -executable \) 2>/dev/null | while read f; do
    patchelf --print-needed "$f" 2>/dev/null | grep '^/' | while read lib; do
        base="$(basename "$lib")"
        patchelf --replace-needed "$lib" "$base" "$f" 2>/dev/null || true
    done
done

# Patch ELF interpreter to a runtime symlink so /proc/self/exe still points at
# the binary itself — required for CEF, which re-execs /proc/self/exe for its
# renderer/GPU/utility subprocesses. AppRun creates the symlink at startup.
patchelf --set-interpreter "/tmp/.jf-cef-interp/${LD_SONAME}" \
    "$APPDIR/usr/bin/jellyfin-desktop"

# AppDir root files (per AppImage spec)
cp "$APPDIR/usr/share/applications/org.jellyfin.JellyfinDesktop.desktop" "$APPDIR/"
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg" "$APPDIR/"
ln -sf org.jellyfin.JellyfinDesktop.svg "$APPDIR/.DirIcon"
cp /src/dev/linux/appimage/AppRun "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"

# Package
ARCH="$ARCH" /opt/tools/appimagetool/AppRun --no-appstream \
    --runtime-file "/opt/tools/runtime-${ARCH}" \
    "$APPDIR" "/host-output/JellyfinDesktop-${VERSION}-${ARCH}.AppImage"
