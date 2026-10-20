#!/usr/bin/env bash
#
# Build a self-contained RawTherapee tree inside the portable build image.
# Called by tools/build-rt.sh portable; not intended to run on the host.
#
# Outputs:
#   /out/stage/usr/local/...
#   /out/rawtherapee-<version>-portable-linux-x86_64.tar.xz
#   /out/rawtherapee-<version>_<version>_amd64.deb
#
set -euo pipefail

readonly SRC=/src
readonly BUILD=/tmp/rt-build
readonly OUT=/out
readonly PREFIX=/usr/local
readonly ROOT="$OUT/stage$PREFIX"
readonly RTLIB="$ROOT/lib/rawtherapee"
readonly DATA="$ROOT/share/rawtherapee"

format="${1:-both}"

case "$format" in
    tarball|deb|both) ;;
    *) printf 'invalid format: %s\n' "$format" >&2; exit 2 ;;
esac

info() {
    printf '==> %s\n' "$*"
}

VERSION="$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo 5.13)"
VERSION="${VERSION#v}"
COUNT="$(git -C "$SRC" rev-list --count HEAD 2>/dev/null || true)"
HASH="$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo local)"
PKG_VERSION="5.13+git${COUNT:-0}.${HASH}"
if [ "${VERSION%-dirty}" != "$VERSION" ]; then
    PKG_VERSION="${PKG_VERSION}.dirty"
fi
PKG_VERSION="${PKG_VERSION}-1"

info "configuring"
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPROC_TARGET_NUMBER=1 \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_BUNDLE=OFF \
    -DLENSFUNDBDIR="$PREFIX/share/lensfun" \
    -DWITH_OPENCL=ON \
    -DWITH_LTO=OFF

info "building"
cmake --build "$BUILD" --parallel "$(nproc)"

info "installing"
rm -rf "$OUT/stage"
DESTDIR="$OUT/stage" cmake --install "$BUILD"

info "relocating binaries"
mkdir -p "$RTLIB"
mv "$ROOT/bin/rawtherapee" "$RTLIB/rawtherapee"
mv "$ROOT/bin/rawtherapee-cli" "$RTLIB/rawtherapee-cli"

info "bundling libraries"
declare -A copied=()
pending=()

collect() {
    local binary="$1"
    while read -r name arrow path rest; do
        [ "$arrow" = "=>" ] || continue
        [ "$path" = "not" ] && continue
        [ -z "$path" ] && continue
        local base="${name}"
        case "$base" in
            linux-vdso*|ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|\
            librt.so*|libresolv.so*|libnss_*|libOpenCL.so*|libMesaOpenCL*|\
            libRusticlOpenCL*|libdrm*|libGL*.so*|libEGL*.so*|libgbm*|libvulkan*|\
            libnvidia*|libcuda*)
                continue
                ;;
        esac
        if [ -n "${copied[$base]:-}" ]; then
            continue
        fi
        if [ ! -f "$path" ]; then
            continue
        fi
        copied["$base"]=1
        cp -L "$path" "$RTLIB/$base"
        chmod 0644 "$RTLIB/$base"
        pending+=("$RTLIB/$base")
        printf '  %s\n' "$base"
    done < <(ldd "$binary" 2>/dev/null || true)
}

for binary in "$RTLIB/rawtherapee" "$RTLIB/rawtherapee-cli"; do
    collect "$binary"
done
for i in "${!pending[@]}"; do
    collect "${pending[$i]}"
done

for binary in "$RTLIB/rawtherapee" "$RTLIB/rawtherapee-cli"; do
    patchelf --set-rpath '$ORIGIN' "$binary"
done
for library in "$RTLIB"/*; do
    [ -f "$library" ] && patchelf --set-rpath '$ORIGIN' "$library"
done

if [ -d /usr/share/lensfun ]; then
    info "bundling lensfun data"
    mkdir -p "$ROOT/share/lensfun"
    cp -a /usr/share/lensfun/. "$ROOT/share/lensfun/"
fi

info "bundling GTK runtime data"
GDK_LOADERS="$RTLIB/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "$GDK_LOADERS"
cp -L /usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/*.so \
    "$GDK_LOADERS/" 2>/dev/null || true
GDK_QUERY="/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"
if [ -x "$GDK_QUERY" ]; then
    "$GDK_QUERY" "$GDK_LOADERS"/*.so > "$GDK_LOADERS/loaders.cache"
    sed -i "s|$GDK_LOADERS|$PREFIX/lib/rawtherapee/gdk-pixbuf-2.0/2.10.0/loaders|g" \
        "$GDK_LOADERS/loaders.cache"
fi

GTK_IM="$RTLIB/gtk-3.0/3.0.0/immodules"
mkdir -p "$GTK_IM"
cp -L /usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules/*.so \
    "$GTK_IM/" 2>/dev/null || true
GTK_QUERY="/usr/lib/x86_64-linux-gnu/libgtk-3-0/gtk-query-immodules-3.0"
if [ -x "$GTK_QUERY" ]; then
    "$GTK_QUERY" > "$RTLIB/gtk-3.0/3.0.0/immodules.cache" 2>/dev/null || true
    sed -i "s|/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules|$PREFIX/lib/rawtherapee/gtk-3.0/3.0.0/immodules|g" \
        "$RTLIB/gtk-3.0/3.0.0/immodules.cache"
fi

SCHEMAS="$ROOT/share/glib-2.0/schemas"
mkdir -p "$SCHEMAS"
cp -L /usr/share/glib-2.0/schemas/*.gschema.xml "$SCHEMAS/" 2>/dev/null || true
glib-compile-schemas "$SCHEMAS" 2>/dev/null || true

make_wrapper() {
    local destination="$1"
    local target="$2"
    cat > "$destination" <<EOF
#!/bin/sh
RT=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
export GDK_PIXBUF_MODULE_FILE="\$RT/lib/rawtherapee/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GDK_PIXBUF_MODULEDIR="\$RT/lib/rawtherapee/gdk-pixbuf-2.0/2.10.0/loaders"
export GTK_IM_MODULE_FILE="\$RT/lib/rawtherapee/gtk-3.0/3.0.0/immodules.cache"
export GSETTINGS_SCHEMA_DIR="\$RT/share/glib-2.0/schemas"
exec "\$RT/lib/rawtherapee/$target" "\$@"
EOF
    chmod 0755 "$destination"
}

info "creating wrappers"
make_wrapper "$ROOT/bin/rawtherapee" rawtherapee
make_wrapper "$ROOT/bin/rawtherapee-cli" rawtherapee-cli

if [ "$format" = "tarball" ] || [ "$format" = "both" ]; then
    info "creating tarball"
    tar -C "$OUT/stage" -czf \
        "$OUT/rawtherapee-${VERSION}-portable-linux-x86_64.tar.gz" \
        "usr/local"
fi

if [ "$format" = "deb" ] || [ "$format" = "both" ]; then
    info "creating deb"
    DEB="$OUT/deb"
    rm -rf "$DEB"
    mkdir -p "$DEB/DEBIAN" "$DEB/usr/local" \
        "$DEB/usr/share/applications" "$DEB/usr/share/icons/hicolor/scalable/apps"
    cp -a "$ROOT/." "$DEB/usr/local/"

    cat > "$DEB/usr/share/applications/rawtherapee-5.13.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Version=1.0
Name=RawTherapee 5.13
GenericName=Raw Photo Editor
Comment=Advanced raw photo development program
Icon=rawtherapee-5.13
TryExec=/usr/local/bin/rawtherapee
Exec=/usr/local/bin/rawtherapee %f
Terminal=false
MimeType=image/jpeg;image/png;image/tiff;image/x-adobe-dng;image/x-canon-cr2;image/x-canon-cr3;image/x-fuji-raf;image/x-nikon-nef;image/x-sony-arw;image/x-raw;inode/directory;
Categories=Graphics;Photography;2DGraphics;RasterGraphics;GTK;
Keywords=raw;photo;photography;develop;pp3;graphics;
StartupNotify=true
EOF

    cp "$ROOT/share/icons/hicolor/scalable/apps/rawtherapee.svg" \
        "$DEB/usr/share/icons/hicolor/scalable/apps/rawtherapee-5.13.svg" 2>/dev/null || true

    cat > "$DEB/DEBIAN/control" <<EOF
Package: rawtherapee-5.13
Version: $PKG_VERSION
Architecture: amd64
Maintainer: Local build <root@localhost>
Section: graphics
Priority: optional
Depends: libc6 (>= 2.35)
Recommends: ocl-icd-libopencl1
Suggests: mesa-opencl-icd | nvidia-opencl-icd | intel-opencl-icd
Installed-Size: $(du -sk "$DEB" | cut -f1)
Description: RawTherapee 5.13 portable build
 Self-contained RawTherapee build under /usr/local with bundled libraries.
 OpenCL acceleration uses the host ICD loader and vendor driver when available.
EOF

    cat > "$DEB/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
[ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database -q /usr/share/applications || true
[ -x /usr/bin/gtk-update-icon-cache ] && /usr/bin/gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
exit 0
EOF
    cat > "$DEB/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
[ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database -q /usr/share/applications || true
[ -x /usr/bin/gtk-update-icon-cache ] && /usr/bin/gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
exit 0
EOF
    chmod 0755 "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/postrm"

    strip --strip-unneeded "$DEB/usr/local/lib/rawtherapee/rawtherapee" \
        "$DEB/usr/local/lib/rawtherapee/rawtherapee-cli" 2>/dev/null || true
    sed -i "s/^Installed-Size: .*/Installed-Size: $(du -sk "$DEB" | cut -f1)/" \
        "$DEB/DEBIAN/control"

    find "$DEB" -type d -exec chmod 0755 {} +
    find "$DEB" -type f -exec chmod 0644 {} +
    chmod 0755 "$DEB/usr/local/bin/rawtherapee" \
        "$DEB/usr/local/bin/rawtherapee-cli" \
        "$DEB/usr/local/lib/rawtherapee/rawtherapee" \
        "$DEB/usr/local/lib/rawtherapee/rawtherapee-cli" \
        "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/postrm"

    dpkg-deb -Zxz --build --root-owner-group "$DEB" \
        "$OUT/rawtherapee-${VERSION}_${PKG_VERSION}_amd64.deb"
fi

info "done"
ls -lh "$OUT"/*.tar.gz "$OUT"/*.deb 2>/dev/null || true
