#!/bin/sh
# Build a self-contained aarch64 AppImage for json-gl-xr.
#
# Runs inside the arm64 build container (see packaging/Dockerfile), from the
# repository root:
#
#     make appimage
#
# Output: dist/json-gl-xr-aarch64.AppImage
set -eu

BIN=json-gl-xr
APPDIR=build/AppDir
DIST=dist
OUT=$DIST/json-gl-xr-aarch64.AppImage

[ -x "$BIN" ] || { echo "$BIN not built; run 'make vr' first" >&2; exit 1; }

rm -rf "$APPDIR" "$OUT"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"

cp "$BIN" "$APPDIR/usr/bin/$BIN"

# Bundle everything the binary needs except the things that must come from
# the host: the C library, the dynamic loader, and the GL/EGL dispatch layer
# (libglvnd + Mesa drivers are provided by SteamOS).
EXCLUDE='^(ld-linux|libc|libm|libpthread|libdl|librt|libgcc_s|libstdc\+\+|libEGL|libGLESv2|libGLdispatch|libGLX|libOpenGL|libgbm|libdrm|libwayland|libX11|libxcb)'

ldd "$BIN" | awk '/=> \// { print $3 }' | while read -r lib; do
    base=$(basename "$lib")
    case "$base" in
        $EXCLUDE*) continue ;;
    esac
    cp -L "$lib" "$APPDIR/usr/lib/$base"
done

echo "bundled:"
ls "$APPDIR/usr/lib" | sed 's/^/  /'

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/json-gl-xr" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/json-gl-xr.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=JSON in dark space (XR)
Comment=Walk a JSON document as a graph in VR
Exec=json-gl-xr
Icon=json-gl-xr
Categories=Development;Graphics;
Terminal=true
EOF

cat > "$APPDIR/json-gl-xr.svg" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <rect width="128" height="128" rx="20" fill="#0a0c14"/>
  <g stroke="#e8e8ff" stroke-width="3" fill="none" opacity="0.8">
    <path d="M64 64 L40 34 M64 64 L88 34 M64 64 L28 74 M64 64 L100 74 M64 64 L52 100 M64 64 L76 100"/>
  </g>
  <rect x="52" y="52" width="24" height="24" fill="#ff3030"/>
  <rect x="31" y="25" width="18" height="18" fill="#30ff30"/>
  <rect x="79" y="25" width="18" height="18" fill="#3030ff"/>
  <rect x="19" y="65" width="18" height="18" fill="#ffff30"/>
  <rect x="91" y="65" width="18" height="18" fill="#ff30ff"/>
  <rect x="43" y="91" width="18" height="18" fill="#30ffff"/>
  <rect x="67" y="91" width="18" height="18" fill="#ff8030"/>
</svg>
EOF
cp "$APPDIR/json-gl-xr.svg" "$APPDIR/.DirIcon"

# appimagetool itself: pinned to the continuous aarch64 build.
TOOL=build/appimagetool-aarch64.AppImage
if [ ! -x "$TOOL" ]; then
    curl -fsSL -o "$TOOL" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-aarch64.AppImage
    chmod +x "$TOOL"
fi

mkdir -p "$DIST"
ARCH=aarch64 "$TOOL" --appimage-extract-and-run --no-appstream "$APPDIR" "$OUT"

echo
echo "built: $OUT"
ls -lh "$OUT"
