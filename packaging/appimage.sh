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
EXCLUDE='^(ld-linux|libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|libresolv\.so|libanl\.so|libnss_|libgcc_s\.so|libstdc\+\+\.so|libEGL|libGLESv2|libGLdispatch|libGLX|libOpenGL|libgbm|libdrm|libwayland|libX11|libxcb)'

ldd "$BIN" | awk '/=> \// { print $3 }' | while read -r lib; do
    base=$(basename "$lib")
    if [[ "$base" =~ $EXCLUDE ]]; then
        continue
    fi
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

# the icon lives in packaging/ as an svg plus the 256x256 png that appimagetool
# wants for .DirIcon (it derives the symlink itself from the png)
cp packaging/json-gl-xr.svg "$APPDIR/json-gl-xr.svg"
cp packaging/json-gl-xr.png "$APPDIR/json-gl-xr.png"

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
