#!/bin/bash
# scripts/package.sh — Builds .xbps, .deb, and .rpm packages for crossfeed
set -euo pipefail

VERSION="${VERSION:-2.5.0}"
PKG_RELEASE="1"
PKG_NAME="crossfeed"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"
BIN="$ROOT_DIR/bin/crossfeed"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)
        DEB_ARCH="amd64"
        RPM_ARCH="x86_64"
        XBPS_ARCH="x86_64"
        ;;
    aarch64|arm64)
        DEB_ARCH="arm64"
        RPM_ARCH="aarch64"
        XBPS_ARCH="aarch64"
        ;;
    *)
        DEB_ARCH="$ARCH"
        RPM_ARCH="$ARCH"
        XBPS_ARCH="$ARCH"
        ;;
esac

ensure_bin() {
    if [ ! -f "$BIN" ]; then
        echo "==> Building binary $BIN first..."
        make -C "$ROOT_DIR"
    fi
}

setup_destdir() {
    local target_dir="$1"
    rm -rf "$target_dir"
    mkdir -p "$target_dir/usr/bin"
    mkdir -p "$target_dir/usr/share/applications"
    mkdir -p "$target_dir/usr/share/icons/hicolor/scalable/apps"
    mkdir -p "$target_dir/usr/share/licenses/crossfeed"

    install -m 755 "$BIN" "$target_dir/usr/bin/crossfeed"
    install -m 644 "$ROOT_DIR/data/crossfeed.desktop" "$target_dir/usr/share/applications/crossfeed.desktop"
    install -m 644 "$ROOT_DIR/data/crossfeed.svg" "$target_dir/usr/share/icons/hicolor/scalable/apps/crossfeed.svg"
    install -m 644 "$ROOT_DIR/LICENSE" "$target_dir/usr/share/licenses/crossfeed/LICENSE"
}

build_xbps() {
    echo "========================================"
    echo "  Building XBPS package (.xbps)...      "
    echo "========================================"
    if ! command -v xbps-create >/dev/null 2>&1; then
        echo "error: xbps-create command not found." >&2
        return 1
    fi

    local build_dir="$ROOT_DIR/build/pkg-xbps"
    setup_destdir "$build_dir"
    mkdir -p "$DIST_DIR"

    local pkgver="${PKG_NAME}-${VERSION}_${PKG_RELEASE}"
    local outfile="${pkgver}.${XBPS_ARCH}.xbps"

    echo "Running xbps-create..."
    xbps-create -A "$XBPS_ARCH" \
                -n "$pkgver" \
                -s "Standalone headphone crossfeed audio processor" \
                -S "High-performance crossfeed DSP with native GTK3 GUI, system tray, and universal sound server support." \
                -l "MIT" \
                -m "crossfeed maintainers" \
                -H "https://github.com/ikuu/pipewire-crossfeed" \
                -D "libpipewire>=0.3.0 libpulseaudio>=15.0 alsa-lib>=1.2.0 gtk+3>=3.24.0 libayatana-appindicator>=0.5.0" \
                "$build_dir"

    mv "$outfile" "$DIST_DIR/"
    echo "==> XBPS package created: $DIST_DIR/$outfile"
    rm -rf "$build_dir"
}

build_deb() {
    echo "========================================"
    echo "  Building Debian package (.deb)...     "
    echo "========================================"
    if ! command -v dpkg-deb >/dev/null 2>&1; then
        echo "error: dpkg-deb command not found." >&2
        return 1
    fi

    local build_dir="$ROOT_DIR/build/pkg-deb"
    setup_destdir "$build_dir"
    mkdir -p "$build_dir/DEBIAN"
    mkdir -p "$DIST_DIR"

    cat > "$build_dir/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}-${PKG_RELEASE}
Architecture: ${DEB_ARCH}
Maintainer: pipewire-crossfeed <https://github.com/ikuu/pipewire-crossfeed>
Section: sound
Priority: optional
Installed-Size: $(du -sk "$build_dir/usr" | cut -f1)
Depends: libc6, libpipewire-0.3-0 | libpipewire-0.3-0t64, libpulse0, libasound2 | libasound2t64, libgtk-3-0 | libgtk-3-0t64, libayatana-appindicator3-1
Suggests: pipewire, pulseaudio
Description: Standalone ultra-low-latency headphone crossfeed audio processor
 High-performance crossfeed filter with native GTK3 GUI, system tray,
 and support for PipeWire, PulseAudio, and ALSA without configuration files.
EOF

    local deb_name="${PKG_NAME}_${VERSION}-${PKG_RELEASE}_${DEB_ARCH}.deb"
    dpkg-deb --build --root-owner-group "$build_dir" "$DIST_DIR/$deb_name"
    echo "==> Debian package created: $DIST_DIR/$deb_name"
    rm -rf "$build_dir"
}

build_rpm() {
    echo "========================================"
    echo "  Building RPM package (.rpm)...        "
    echo "========================================"
    if ! command -v rpmbuild >/dev/null 2>&1; then
        echo "error: rpmbuild command not found." >&2
        return 1
    fi

    local build_dir="$ROOT_DIR/build/pkg-rpm"
    rm -rf "$build_dir"
    mkdir -p "$build_dir/BUILD" "$build_dir/RPMS" "$build_dir/SOURCES" "$build_dir/SPECS" "$build_dir/SRPMS" "$build_dir/rpmdb"
    mkdir -p "$DIST_DIR"

    local date_str
    date_str=$(date +"%a %b %d %Y")
    local spec_file="$build_dir/SPECS/${PKG_NAME}.spec"
    cat > "$spec_file" <<EOF
Name:           ${PKG_NAME}
Version:        ${VERSION}
Release:        ${PKG_RELEASE}%{?dist}
Summary:        Standalone ultra-low-latency headphone crossfeed audio processor
License:        MIT
URL:            https://github.com/ikuu/pipewire-crossfeed
BuildArch:      ${RPM_ARCH}
AutoReqProv:    yes

%description
High-performance crossfeed filter with native GTK3 GUI, system tray,
and support for PipeWire, PulseAudio, and ALSA without configuration files.

%install
mkdir -p %{buildroot}/usr/bin
mkdir -p %{buildroot}/usr/share/applications
mkdir -p %{buildroot}/usr/share/icons/hicolor/scalable/apps
mkdir -p %{buildroot}/usr/share/licenses/crossfeed

install -m 755 "${BIN}" %{buildroot}/usr/bin/crossfeed
install -m 644 "${ROOT_DIR}/data/crossfeed.desktop" %{buildroot}/usr/share/applications/crossfeed.desktop
install -m 644 "${ROOT_DIR}/data/crossfeed.svg" %{buildroot}/usr/share/icons/hicolor/scalable/apps/crossfeed.svg
install -m 644 "${ROOT_DIR}/LICENSE" %{buildroot}/usr/share/licenses/crossfeed/LICENSE

%files
/usr/bin/crossfeed
/usr/share/applications/crossfeed.desktop
/usr/share/icons/hicolor/scalable/apps/crossfeed.svg
/usr/share/licenses/crossfeed/LICENSE

%changelog
* ${date_str} Maintainers <noreply@github.com> - ${VERSION}-${PKG_RELEASE}
- Standalone release with native GTK3 GUI and system tray
EOF

    rpmbuild --define "_topdir $build_dir" \
             --define "_dbpath $build_dir/rpmdb" \
             --define "%__os_install_post %{nil}" \
             --define "_build_id_links none" \
             -bb "$spec_file"
    find "$build_dir/RPMS" -name "*.rpm" -exec cp {} "$DIST_DIR/" \;
    echo "==> RPM package created in $DIST_DIR:"
    ls -la "$DIST_DIR"/*.rpm
    rm -rf "$build_dir"
}

main() {
    ensure_bin
    mkdir -p "$DIST_DIR"

    local target="${1:-all}"
    case "$target" in
        xbps)
            build_xbps
            ;;
        deb)
            build_deb
            ;;
        rpm)
            build_rpm
            ;;
        all|pkg)
            build_xbps
            build_deb
            build_rpm
            echo "========================================"
            echo "All packages generated successfully in $DIST_DIR:"
            ls -la "$DIST_DIR"
            echo "========================================"
            ;;
        *)
            echo "Usage: $0 [xbps|deb|rpm|all]" >&2
            exit 1
            ;;
    esac
}

main "$@"
