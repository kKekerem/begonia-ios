#!/bin/bash
# SPDX-License-Identifier: MIT
# Build Begonia ARM Mali-G76 (Panfrost) + Softpipe Metal & OpenGLES userspace payload for iOS 18 arm64e.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${BEGONIA_METAL_WORK:-/tmp/begonia-metal-work}"
OUT="${1:-$WORK/out-metal}"
BERGAMOTA_DIR="${BERGAMOTA_DIR:-$WORK/bergamota}"
CHLOROPHYLL_DIR="${CHLOROPHYLL_DIR:-$WORK/chlorophyll}"
MESA_DIR="${MESA_DIR:-$WORK/mesa-src}"
LIBDRM_DIR="${LIBDRM_DIR:-$WORK/libdrm-src}"
VENV="${MESA_VENV:-$WORK/venv}"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=18.0 -isysroot $SDK"
BISON="$(brew --prefix bison)/bin"
PACSTRIP="$BERGAMOTA_DIR/tools/renegade/pacstrip.py"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

mkdir -p "$WORK" "$OUT/bgmtl" "$OUT/bergamota-gl/OpenGLES.framework" "$OUT/Chlorophyll.bundle" "$OUT/probes"
export PATH="$BISON:$VENV/bin:$PATH"

[ -f "$PACSTRIP" ] || { echo "Missing pacstrip.py at $PACSTRIP" >&2; exit 1; }
[ -d "$MESA_DIR/.git" ] || git clone -q --depth 1 --branch mesa-25.0.7 https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_DIR"
[ -d "$LIBDRM_DIR/.git" ] || git clone -q --depth 1 --branch libdrm-2.4.124 https://gitlab.freedesktop.org/mesa/drm.git "$LIBDRM_DIR"
if [ ! -d "$CHLOROPHYLL_DIR/.git" ]; then
  git clone -q https://github.com/ry-diffusion/Chlorophyll.git "$CHLOROPHYLL_DIR"
  git -C "$CHLOROPHYLL_DIR" checkout -q 787338fc7c297f54e2000bca02f4fa36f1154c5a
fi

# Fresh patched Chlorophyll worktree (never mutate source if reused)
CHL_WORK="$WORK/chlorophyll-work"
rm -rf "$CHL_WORK"
git clone -q --shared "$CHLOROPHYLL_DIR" "$CHL_WORK"
git -C "$CHL_WORK" apply "$HERE/patches/chlorophyll-panfrost.patch"

# 1. Build libdrm.2.dylib (SweetMali Panfrost stand-in)
DRM_LINK="$WORK/drm-link"
PC_DIR="$WORK/pc"
mkdir -p "$DRM_LINK" "$PC_DIR"
$CC -O2 -g -Wall -Wextra -Werror -fvisibility=hidden -dynamiclib -install_name "@rpath/libdrm.2.dylib" \
  "$HERE/sweet_drm_mali.c" "$HERE/sweet_drm_syncobj.c" -o "$DRM_LINK/libdrm.2.dylib" \
  -framework IOKit -framework CoreFoundation
ln -sf libdrm.2.dylib "$DRM_LINK/libdrm.dylib"
cat > "$PC_DIR/libdrm.pc" <<EOP
Name: libdrm
Description: Begonia SweetMali libdrm stand-in
Version: 2.4.124
Cflags: -I$LIBDRM_DIR/include/drm -I$LIBDRM_DIR
Libs: -L$DRM_LINK -ldrm
EOP

# 2. Prepare Mesa 25.0.7 build tree with Panfrost + AIR + OSMesa ES patches
MESA_WORK="$WORK/mesa-work"
if [ ! -d "$MESA_WORK/.patched" ]; then
  rm -rf "$MESA_WORK"
  git clone -q --shared "$MESA_DIR" "$MESA_WORK"
  git -C "$MESA_WORK" apply "$HERE/patches/begonia-mesa-25.0.7-panfrost.patch"
  mkdir -p "$MESA_WORK/src/compiler/air"
  cp -f "$CHL_WORK"/air/* "$MESA_WORK/src/compiler/air/"
  touch "$MESA_WORK/.patched"
else
  cp -f "$CHL_WORK"/air/* "$MESA_WORK/src/compiler/air/"
fi

CROSS="$WORK/ios-arm64e.cross"
cat > "$CROSS" <<EOT
[binaries]
c = ['xcrun','--sdk','iphoneos','clang','-arch','arm64e']
cpp = ['xcrun','--sdk','iphoneos','clang++','-arch','arm64e']
objc = ['xcrun','--sdk','iphoneos','clang','-arch','arm64e']
ar = ['xcrun','--sdk','iphoneos','ar']
strip = ['xcrun','--sdk','iphoneos','strip']
ranlib = ['xcrun','--sdk','iphoneos','ranlib']
bison = '$BISON/bison'
pkg-config = '$(command -v pkg-config)'

[built-in options]
pkg_config_path = ['$PC_DIR']
c_args = ['-miphoneos-version-min=18.0','-isysroot','$SDK']
cpp_args = ['-miphoneos-version-min=18.0','-isysroot','$SDK']
c_link_args = ['-miphoneos-version-min=18.0','-isysroot','$SDK']
cpp_link_args = ['-miphoneos-version-min=18.0','-isysroot','$SDK']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64e'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOT

BLD="$WORK/build-ios"
if [ ! -f "$BLD/build.ninja" ]; then
  PKG_CONFIG_PATH="$PC_DIR" meson setup "$BLD" "$MESA_WORK" \
    --cross-file "$CROSS" --buildtype=release \
    -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dllvm=disabled \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= \
    -Dosmesa=true -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Dshared-glapi=enabled \
    -Dgallium-xa=disabled -Dtools= -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dzstd=disabled -Dxmlconfig=disabled -Dbuild-tests=false -Dglvnd=disabled \
    -Dlmsensors=disabled -Dgallium-opencl=disabled -Dgallium-rusticl=false \
    -Dshader-cache=enabled -Dshader-cache-max-size=128M -Dprefix=/
fi

ninja -j"$JOBS" -C "$BLD" \
  src/gallium/drivers/panfrost/libbgair.dylib \
  src/gallium/targets/osmesa/libOSMesa.8.dylib

# Normalize built Mesa dylib install_names BEFORE downstream linking
install_name_tool -id "@rpath/libbgair.dylib" "$BLD/src/gallium/drivers/panfrost/libbgair.dylib"
install_name_tool -id "@rpath/libOSMesa.8.dylib" "$BLD/src/gallium/targets/osmesa/libOSMesa.8.dylib"

# Prepare clean dylibs with @rpath install_names
cp -f "$DRM_LINK/libdrm.2.dylib" "$OUT/bgmtl/libdrm.2.dylib"
cp -f "$BLD/src/gallium/drivers/panfrost/libbgair.dylib" "$OUT/bgmtl/libbgair.dylib"
xcrun strip -x "$OUT/bgmtl/libbgair.dylib"
for rp in $(otool -l "$OUT/bgmtl/libbgair.dylib" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}'); do
  install_name_tool -delete_rpath "$rp" "$OUT/bgmtl/libbgair.dylib" 2>/dev/null || true
done
install_name_tool -add_rpath "@loader_path" "$OUT/bgmtl/libbgair.dylib" 2>/dev/null || true

# 3. Build bgmtl-inject.dylib (backboardd & MercuryPosterExtension)
$CC -O2 -g -fno-objc-arc -Wall -Wno-deprecated-declarations -I"$CHL_WORK/air" \
  -c "$CHL_WORK/src/bgmtl.m" -o "$WORK/bgmtl.o"
$CC -O2 -g -fno-objc-arc -Wall -Wno-deprecated-declarations \
  -c "$CHL_WORK/inject/bgmtl_inject.m" -o "$WORK/bgmtl_inject.o"
$CC -dynamiclib -o "$OUT/bgmtl/bgmtl-inject.dylib" "$WORK/bgmtl.o" "$WORK/bgmtl_inject.o" \
  "$OUT/bgmtl/libbgair.dylib" "$OUT/bgmtl/libdrm.2.dylib" \
  -install_name "/usr/local/lib/bgmtl/bgmtl-inject.dylib" -Wl,-not_for_dyld_shared_cache \
  -framework Foundation -framework Metal -framework IOSurface -Wl,-rpath,@loader_path

# 4. Build Chlorophyll.bundle (/System/Library/Extensions/Chlorophyll.bundle)
B="$OUT/Chlorophyll.bundle"
cp -f "$OUT/bgmtl/libbgair.dylib" "$B/libbgair.dylib"
cp -f "$OUT/bgmtl/libdrm.2.dylib" "$B/libdrm.2.dylib"
RENAME=""
for c in $(grep -oE '^@interface (BG[A-Za-z0-9_]+)' "$CHL_WORK/src/bgmtl.m" | sed 's/@interface //' | sort -u); do
  RENAME="$RENAME -D$c=CHL_$c"
done
$CC -O2 -g -fno-objc-arc -Wall -Wno-deprecated-declarations $RENAME \
  -I"$CHL_WORK/air" -c "$CHL_WORK/plugin/chlorophyll_plugin.m" -o "$WORK/chlorophyll_plugin.o"
$CC -bundle -o "$B/Chlorophyll" "$WORK/chlorophyll_plugin.o" \
  "$B/libbgair.dylib" "$B/libdrm.2.dylib" \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit -Wl,-rpath,@loader_path
cat > "$B/Info.plist" <<'EOP'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>Chlorophyll</string>
  <key>CFBundleIdentifier</key><string>com.bergamota.chlorophyll</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>Chlorophyll</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>NSPrincipalClass</key><string>CHLDevice</string>
</dict></plist>
EOP
printf "backboardd\n" > "$B/chlorophyll.deny"   # 22 Nov: SpringBoard gets a Metal device too; without one UIKit drew every material opaque

# 5. Build OpenGLES / EAGL / libOSMesa.8.dylib / bgl-launch payload (Panfrost + Softpipe)
cp -f "$OUT/bgmtl/libdrm.2.dylib" "$DRM_LINK/libdrm.2.dylib"
(
  export GPU_EAGL_WORK="$WORK/eagl-work"
  mkdir -p "$GPU_EAGL_WORK/drm-standin"
  ln -sf "$MESA_WORK" "$GPU_EAGL_WORK/mesa"
  cp -f "$OUT/bgmtl/libdrm.2.dylib" "$GPU_EAGL_WORK/drm-standin/libdrm.2.dylib"
  EAGL_DEFS=-DEAGL_INJECT MESA_BLD="$BLD" \
    "$BERGAMOTA_DIR/gpu/eagl/build.sh" "$WORK/eagl-inj" >/dev/null
  cp -f "$WORK/eagl-inj/OpenGLES.framework/OpenGLES" "$OUT/bergamota-gl/OpenGLES.framework/OpenGLES"
  cp -f "$WORK/eagl-inj/libOSMesa.8.dylib" "$OUT/bergamota-gl/libOSMesa.8.dylib"
  cp -f "$OUT/bgmtl/libdrm.2.dylib" "$OUT/bergamota-gl/libdrm.2.dylib"
  # 24 Nov: bgl-launch with the one-shot GPU self-test (patches/bgl-launch-probe.patch)
  BGL_SRC="$WORK/bgl-launch-src"
  rm -rf "$BGL_SRC"
  mkdir -p "$BGL_SRC/gpu/eagl/launch"
  cp "$BERGAMOTA_DIR/gpu/eagl/launch/bgl-launch.c" "$BGL_SRC/gpu/eagl/launch/"
  (cd "$BGL_SRC" && git init -q . && git apply "$HERE/patches/bgl-launch-probe.patch")
  $CC -O2 -Wall -Wextra "$BGL_SRC/gpu/eagl/launch/bgl-launch.c" -o "$OUT/bergamota-gl/bgl-launch"
)
install_name_tool -id "@rpath/libOSMesa.8.dylib" "$OUT/bergamota-gl/libOSMesa.8.dylib"
for rp in $(otool -l "$OUT/bergamota-gl/libOSMesa.8.dylib" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}'); do
  install_name_tool -delete_rpath "$rp" "$OUT/bergamota-gl/libOSMesa.8.dylib" 2>/dev/null || true
done
install_name_tool -add_rpath "@loader_path" "$OUT/bergamota-gl/libOSMesa.8.dylib" 2>/dev/null || true

# 6. Standalone Metal compute & render probes + compiled .metallib shaders
xcrun -sdk iphoneos metal -mios-version-min=18.0 -c "$CHL_WORK/test/bg_kernels.metal" -o "$WORK/bg_kernels.air"
xcrun -sdk iphoneos metallib "$WORK/bg_kernels.air" -o "$OUT/probes/bg_kernels.metallib"
xcrun -sdk iphoneos metal -mios-version-min=18.0 -c "$CHL_WORK/test/bg_render.metal" -o "$WORK/bg_render.air"
xcrun -sdk iphoneos metallib "$WORK/bg_render.air" -o "$OUT/probes/bg_render.metallib"
$CC -O1 -g -fno-objc-arc -Wall -Wno-deprecated-declarations -c "$CHL_WORK/test/bgmtl_probe.m" -o "$WORK/bgmtl_probe.o"
$CC -o "$OUT/probes/bgmtl_probe" "$WORK/bgmtl.o" "$WORK/bgmtl_probe.o" \
  "$OUT/bgmtl/libbgair.dylib" "$OUT/bgmtl/libdrm.2.dylib" \
  -framework Foundation -framework Metal -framework IOSurface -Wl,-rpath,/usr/local/lib/bgmtl
$CC -O1 -g -fno-objc-arc -Wall -Wno-deprecated-declarations -c "$CHL_WORK/test/bgmtl_render_probe.m" -o "$WORK/bgmtl_render_probe.o"
$CC -o "$OUT/probes/bgmtl_render_probe" "$WORK/bgmtl.o" "$WORK/bgmtl_render_probe.o" \
  "$OUT/bgmtl/libbgair.dylib" "$OUT/bgmtl/libdrm.2.dylib" \
  -framework Foundation -framework Metal -framework IOSurface -Wl,-rpath,/usr/local/lib/bgmtl

# 7. PAC-strip + ad-hoc sign all Mach-O outputs (strip first, deps last -> callers valid)
BINARIES=(
  "$OUT/bgmtl/libdrm.2.dylib"
  "$OUT/bgmtl/libbgair.dylib"
  "$OUT/bgmtl/bgmtl-inject.dylib"
  "$B/libdrm.2.dylib"
  "$B/libbgair.dylib"
  "$B/Chlorophyll"
  "$OUT/bergamota-gl/libdrm.2.dylib"
  "$OUT/bergamota-gl/libOSMesa.8.dylib"
  "$OUT/bergamota-gl/OpenGLES.framework/OpenGLES"
  "$OUT/bergamota-gl/bgl-launch"
  "$OUT/probes/bgmtl_probe"
  "$OUT/probes/bgmtl_render_probe"
)
for f in "${BINARIES[@]}"; do
  bad=$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^/(Users|tmp|var|opt|lib)' || true)
  if [ -n "$bad" ]; then
    echo "Bad dylib dependency in $f:" >&2
    echo "$bad" >&2
    exit 1
  fi
  python3 -I "$PACSTRIP" --sign "$f" >/dev/null
done

(cd "$OUT" && find . -type f ! -name 'SHA256SUMS.txt' | sort | xargs shasum -a 256 > SHA256SUMS.txt)
cat "$OUT/SHA256SUMS.txt"
