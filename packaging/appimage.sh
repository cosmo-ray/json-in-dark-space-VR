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

cp -vL $( ldd $BIN | grep '=>' | cut -d ' ' -f 3 | grep -v -e 'libm.so' -e 'libc.so' -e 'libstdc++.so' -e 'libdl.so.2' -e 'ld-linux-x86-64.so.2' -e 'libpthread.so.0' -e 'librt.so' -e 'lib*GL*' -e 'libXau' -e 'libxcb' -e 'libX11' -e 'libXext' -e 'libOpenGL.so' -e 'linux-vdso' -e 'libXdmcp' -e 'libwaylan' -e 'libEGL.so' -e 'libbsd.so' -e 'libmd.so' -e 'libdrm.so' -e 'libgbm.so' ) "$APPDIR/usr/lib/"

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

# Self-extracting installer: the AppImage, the example document and the icon
# packed behind a small shell header, so the headset only needs one file and
# no root. Installs under ~/.local (PREFIX=... / --prefix DIR to change it).

STAGE=build/installer
PAYLOAD=build/payload.tar.gz
INST=$DIST/json-gl-xr-aarch64-installer.sh

rm -rf "$STAGE" "$PAYLOAD"
mkdir -p "$STAGE/share/json-gl-xr" "$STAGE/share/icons/hicolor/256x256/apps"

cp "$OUT" "$STAGE/share/json-gl-xr/json-gl-xr.AppImage"
cp example.json "$STAGE/share/json-gl-xr/example.json"
cp packaging/json-gl-xr.png "$STAGE/share/icons/hicolor/256x256/apps/json-gl-xr.png"

tar -C "$STAGE" -czf "$PAYLOAD" .

# the header knows on which line the payload starts, so extracting it is just
# `tail -n +N`
lines=$(wc -l < packaging/installer.sh.in)
sed "s/@OFFSET@/$((lines + 1))/" packaging/installer.sh.in > "$INST"
cat "$PAYLOAD" >> "$INST"
chmod 755 "$INST"

echo
echo "built: $INST"
ls -lh "$INST"
