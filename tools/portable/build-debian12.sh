#!/usr/bin/env bash
#
# Build a Debian bookworm-compatible RawTherapeeGraph package.
# Called by tools/build-rt.sh portable; not intended to run on the host.
#
# Output: /out/rawtherapee-graph-<version>_<version>_amd64.deb
#
set -euo pipefail

readonly SRC=/src
readonly BUILD=/tmp/rt-build
readonly OUT=/out
readonly STAGE="$OUT/stage"
readonly PREFIX=/usr/local

info() {
    printf '==> %s\n' "$*"
}

VERSION="${RT_PACKAGE_VERSION:-$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo 5.13)}"
VERSION="${VERSION#v}"
BASE_VERSION="${RT_PACKAGE_BASE_VERSION:-$(git -C "$SRC" describe --tags --abbrev=0 2>/dev/null || true)}"
BASE_VERSION="${BASE_VERSION#v}"
BASE_VERSION="${BASE_VERSION:-5.13}"
COUNT="${RT_PACKAGE_COMMIT_COUNT:-$(git -C "$SRC" rev-list --count HEAD 2>/dev/null || true)}"
HASH="${RT_PACKAGE_COMMIT_HASH:-$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo local)}"
PKG_VERSION="${BASE_VERSION}+git${COUNT:-0}.${HASH}"
if [ "${VERSION%-dirty}" != "$VERSION" ]; then
    PKG_VERSION="${PKG_VERSION}.dirty"
fi
PKG_VERSION="${PKG_VERSION}-1"
PKG_NAME="rawtherapee-graph-${BASE_VERSION}"

info "configuring"
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPROC_TARGET_NUMBER=1 \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_BUNDLE=OFF \
    -DWITH_OPENCL=ON \
    -DWITH_LTO=OFF

info "building"
cmake --build "$BUILD" --parallel "$(nproc)"

info "installing"
rm -rf "$STAGE"
DESTDIR="$STAGE" cmake --install "$BUILD"

info "computing dependencies"
DEB="$OUT/deb"
rm -rf "$DEB"
mkdir -p "$DEB/DEBIAN" "$DEB/usr"
cp -a "$STAGE/usr/local" "$DEB/usr/"

# exiv2 and JPEG XL have different sonames on Debian 13 than on bookworm.
# Bundle those two libraries so one package runs on bookworm and newer,
# while GTK and the rest of the desktop stack come from the host.
RTLIB="$DEB/usr/local/lib/rawtherapee-graph"
mkdir -p "$RTLIB"
cp -a /usr/lib/x86_64-linux-gnu/libexiv2.so.* "$RTLIB/"
cp -a /usr/lib/x86_64-linux-gnu/libjxl.so.0.7* "$RTLIB/"
cp -a /usr/lib/x86_64-linux-gnu/libjxl_threads.so.0.7* "$RTLIB/"
for lib in "$RTLIB"/*.so.*; do
    [ -L "$lib" ] && continue
    patchelf --set-rpath '$ORIGIN' "$lib"
done
for bin in "$DEB/usr/local/bin/rawtherapee-graph" "$DEB/usr/local/bin/rawtherapee-graph-cli"; do
    patchelf --set-rpath '$ORIGIN/../lib/rawtherapee-graph' "$bin"
done

mkdir -p "$DEB/debian"
cat > "$DEB/debian/control" <<'EOF'
Source: rawtherapee-graph
Section: graphics
Priority: optional
Maintainer: Local build <root@localhost>
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.6.2
Package: rawtherapee-graph
Architecture: any
Description: RawTherapeeGraph 5.13
 Advanced raw photo development program (graph pipeline).
EOF
cat > "$DEB/debian/changelog" <<EOF
rawtherapee-graph (${PKG_VERSION}) bookworm; urgency=medium

  * Local build.

 -- Local build <root@localhost>  $(date -R)
EOF

SHLIBS="$(cd "$DEB" && dpkg-shlibdeps -O \
    "$STAGE/usr/local/bin/rawtherapee-graph" "$STAGE/usr/local/bin/rawtherapee-graph-cli" 2>/dev/null \
    | sed -n 's/^shlibs:Depends=//p' \
    | tr ',' '\n' | sed 's/^ *//; s/ *$//' \
    | grep -vE '^lib(exiv2-27|jxl|expat1)' | paste -sd, -)"
[ -n "$SHLIBS" ] || { echo "error: dpkg-shlibdeps produced no dependencies" >&2; exit 1; }
SHLIBS="$SHLIBS, libbrotli1, libexpat1, libhwy1"

# The shlibdeps working files must not be installed on the target.
rm -rf "$DEB/debian"

cat > "$DEB/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $PKG_VERSION
Architecture: amd64
Maintainer: Local build <root@localhost>
Section: graphics
Priority: optional
Depends: $SHLIBS
Recommends: liblensfun-data-v1, librsvg2-common, ocl-icd-libopencl1, mesa-opencl-icd
Suggests: nvidia-opencl-icd | intel-opencl-icd
Installed-Size: $(du -sk "$DEB/usr" | cut -f1)
Description: RawTherapeeGraph 5.13
 Advanced raw photo development program, built for Debian bookworm and newer.
 OpenCL acceleration uses the host ICD loader and vendor driver when available.
EOF

cat > "$DEB/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
[ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database -q /usr/local/share/applications || true
[ -x /usr/bin/gtk-update-icon-cache ] && /usr/bin/gtk-update-icon-cache -q -t -f /usr/local/share/icons/hicolor || true
exit 0
EOF
cat > "$DEB/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
[ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database -q /usr/local/share/applications || true
[ -x /usr/bin/gtk-update-icon-cache ] && /usr/bin/gtk-update-icon-cache -q -t -f /usr/local/share/icons/hicolor || true
exit 0
EOF
chmod 0755 "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/postrm"

info "stripping"
strip --strip-unneeded "$DEB/usr/local/bin/rawtherapee-graph" \
    "$DEB/usr/local/bin/rawtherapee-graph-cli" 2>/dev/null || true
for lib in "$RTLIB"/*.so.*; do
    [ -L "$lib" ] && continue
    strip --strip-unneeded "$lib" 2>/dev/null || true
done

find "$DEB/usr/local" -type d -exec chmod 0755 {} +
find "$DEB/usr/local" -type f -exec chmod 0644 {} +
chmod 0755 "$DEB/usr/local/bin/rawtherapee-graph" "$DEB/usr/local/bin/rawtherapee-graph-cli"
sed -i "s/^Installed-Size: .*/Installed-Size: $(du -sk "$DEB/usr" | cut -f1)/" "$DEB/DEBIAN/control"

(cd "$DEB" && find usr -type f -print0 | xargs -0 md5sum > DEBIAN/md5sums)

dpkg-deb -Zxz --build --root-owner-group "$DEB" \
    "$OUT/rawtherapee-graph-${VERSION}_${PKG_VERSION}_amd64.deb"

info "done"
ls -lh "$OUT"/*.deb
