#!/usr/bin/env bash
# Build a Debian package (.deb) for Sticky Notes.
#
# The binary is the portable release build (scripts/build_portable.sh, Debian 13
# "trixie": Qt 6.8.2 + LayerShellQt 6.3.4) — the same file the tarball, the
# AppImage and the .rpm ship. Building on the oldest supported distribution
# matters because Qt is forward compatible but NOT backward compatible: this
# single package also runs on Ubuntu 26.04+ / Debian forky (Qt 6.10 +
# LayerShellQt 6.6). NoteWindow.cpp compiles out the two LayerShellQt calls
# added after 6.3, and a tripwire below refuses a binary that would require a
# newer Qt than trixie's.
#
# dpkg-shlibdeps generates the runtime dependencies against those libraries, so
# apt installs the package where Qt 6.8 and LayerShellQt 6.3.4 exist and refuses
# it on an older distribution instead of installing a binary that would then
# fail to start. The .deb does not bundle Qt: use the AppImage or the Flatpak on
# distributions with an older Qt.
#
# Produces release/sticky-notes_<version>-1_<debarch>.deb (amd64 for x86_64).
# Only x86_64 is published, but the script accepts --arch aarch64 too (the
# build then goes through QEMU and produces arm64).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

release_dir="$root/release"
desktop="$root/scripts/sticky-notes.desktop"
icon_dir="$root/packaging/icons"
metainfo="$root/packaging/io.github.danielearrighi.StickyNotes.metainfo.xml"

arch=""
binary_override=""
skip_build=0
clean_only=0
engine=""

usage() {
    cat <<'EOF'
Usage: scripts/build_deb.sh [options]

Builds release/sticky-notes_<version>-1_<debarch>.deb in the Debian 13
("trixie") build container, packaging the portable binary built there against
the system Qt 6.8 + LayerShellQt 6.3.4. The package uses the system Qt6 and
LayerShellQt and is installable on Debian 13 and newer.

Options:
  --arch ARCH    target architecture: x86_64 or aarch64 (default: uname -m);
                 only x86_64 (amd64) is published, aarch64 needs QEMU
  --binary PATH  reuse an existing binary built in the Debian 13 container
                 (implies --skip-build)
  --engine NAME  container engine to use (podman or docker; default: auto)
  --skip-build   do not compile, reuse build-release-<arch>/sticky-notes
  --clean        remove build-deb-<arch> and this architecture's .deb, then exit
  -h, --help     show this help

Environment:
  CONTAINER_ENGINE          same as --engine
  STICKY_NOTES_BUILD_IMAGE  base image tag (default: localhost/sticky-notes-build:debian13;
                            the architecture is appended; an unqualified name
                            is treated as local)
EOF
}

while (($#)); do
    case "$1" in
        --arch)       arch="${2:?--arch needs an architecture}"; shift ;;
        --binary)     binary_override="${2:?--binary needs a path}"; shift ;;
        --engine)     engine="${2:?--engine needs a name}"; shift ;;
        --skip-build) skip_build=1 ;;
        --clean)      clean_only=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

log() { printf '[build_deb] %s\n' "$*"; }
die() { printf 'build_deb: %s\n' "$*" >&2; exit 1; }

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || die "Unsupported architecture: $arch (need x86_64 or aarch64)"
debarch="$(sn_deb_arch "$arch")"

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"
debian_version="$version-1"

pkgname="sticky-notes_${debian_version}_${debarch}"
build_dir="$root/build-deb-$arch"
stage="$build_dir/$pkgname"
asset="$release_dir/$pkgname.deb"

if ((clean_only)); then
    rm -rf "$build_dir"
    rm -f "$release_dir/sticky-notes_"*"_${debarch}.deb"
    log "Removed $build_dir and the $debarch .deb packages from $release_dir"
    exit 0
fi

for f in "$desktop" "$metainfo"; do
    [[ -f "$f" ]] || die "Missing required file: $f"
done
for size in 16 22 32 48 64 128 256; do
    [[ -f "$icon_dir/sticky-notes-$size.png" ]] \
        || die "Missing required file: $icon_dir/sticky-notes-$size.png"
done

# --- 1. Container -----------------------------------------------------------

if [[ -z "$engine" ]]; then
    engine="${CONTAINER_ENGINE:-}"
fi
if [[ -z "$engine" ]]; then
    engine="$(sn_container_engine)" || die "No container engine found: install podman or docker"
fi
command -v "$engine" >/dev/null 2>&1 || die "Container engine not found: $engine"

# STICKY_NOTES_DEB_IMAGE is the old name of the image variable, still honored
# so an existing environment keeps working instead of silently falling back.
image_base="${STICKY_NOTES_BUILD_IMAGE:-${STICKY_NOTES_DEB_IMAGE:-localhost/sticky-notes-build:debian13}}"
image="$(sn_local_image "$image_base-$arch")"
dockerfile="$here/container/debian13.Dockerfile"

sn_require_binfmt "$arch"
platform_args=()
sn_is_native "$arch" || platform_args=(--platform "$(sn_platform "$arch")")

if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    [[ -f "$dockerfile" ]] || die "Missing $dockerfile"
    log "Building $image (first run only)"
    "$engine" build "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
fi

run_args=(--rm --pull=never "${platform_args[@]}" -v "$root:/work:Z" -w /work -e HOME=/tmp)
if [[ "$engine" == "podman" ]]; then
    run_args+=(--userns=keep-id)
else
    run_args+=(--user "$(id -u):$(id -g)")
fi

# --- 2. Release binary ------------------------------------------------------

# The same portable binary every other artifact ships: built once by
# build_portable.sh in this very Debian 13 image, then packaged here.
binary="$root/build-release-$arch/sticky-notes"

if [[ -n "$binary_override" ]]; then
    binary="$binary_override"
    log "Using $binary"
elif ((skip_build)); then
    [[ -x "$binary" ]] || die "Binary not found: $binary (drop --skip-build)"
    log "Reusing $binary"
else
    log "Building the portable binary (Debian 13: Qt 6.8 + LayerShellQt 6.3)"
    portable_args=(--arch "$arch" --image "$image_base")
    [[ -n "$engine" ]] && portable_args+=(--engine "$engine")
    "$here/build_portable.sh" "${portable_args[@]}"
    log "Using $binary"
fi
[[ -x "$binary" ]] || die "Binary not found: $binary"
sn_verify_binary_arch "$binary" "$arch"
log "Version: $version"
log "Architecture: $arch (Debian: $debarch)"

# The package must install on Debian 13: a binary built against a newer Qt fails
# there with "version 'Qt_6.x' not found". The highest Qt_x.y version tag in the
# binary is the floor, so check it before packaging (this also catches --binary
# pointing at a non-trixie build).
max_qt="$(readelf -V "$binary" 2>/dev/null | grep -o 'Qt_6\.[0-9][0-9]*' | sort -Vu | tail -1 || true)"
if [[ "$max_qt" != "Qt_6.8" ]]; then
    die "Binary requires ${max_qt:-an unknown Qt version}, expected Qt_6.8: build it in the Debian 13 container"
fi

# --- 3. Staging folder ------------------------------------------------------

log "Preparing $stage"
rm -rf "$stage"
mkdir -p "$stage/DEBIAN"

install -Dm755 "$binary"  "$stage/usr/bin/sticky-notes"
install -Dm644 "$desktop" "$stage/usr/share/applications/sticky-notes.desktop"

for size in 16 22 32 48 64 128 256; do
    install -Dm644 "$icon_dir/sticky-notes-$size.png" \
        "$stage/usr/share/icons/hicolor/${size}x${size}/apps/sticky-notes.png"
done

# The AppStream metadata keeps its reverse-DNS component id, but the desktop
# file is called sticky-notes.desktop (main.cpp sets that name outside Flatpak),
# so the launchable has to point at it.
mkdir -p "$stage/usr/share/metainfo"
sed 's|io\.github\.danielearrighi\.StickyNotes\.desktop|sticky-notes.desktop|g' \
    "$metainfo" > "$stage/usr/share/metainfo/io.github.danielearrighi.StickyNotes.metainfo.xml"

docdir="$stage/usr/share/doc/sticky-notes"
mkdir -p "$docdir"
cat > "$docdir/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sticky-notes
Source: https://github.com/danielearrighi/kde-sticky-notes

Files: *
Copyright: Daniele Arrighi
License: GPL-3.0-or-later
 On Debian systems the complete text of the GNU General Public License
 version 3 is in /usr/share/common-licenses/GPL-3.
EOF
cat > "$docdir/changelog.Debian" <<EOF
sticky-notes ($debian_version) unstable; urgency=medium

  * Release $version.

 -- Daniele Arrighi <daniele.arrighi@gmail.com>  $(date -R)
EOF
gzip -9n "$docdir/changelog.Debian"

cat > "$stage/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF
cat > "$stage/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF
chmod 755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/postrm"

# --- 4. Dependencies --------------------------------------------------------

log "Resolving runtime dependencies (dpkg-shlibdeps)"
stage_rel="${stage#"$root/"}"
# dpkg-shlibdeps insists on a debian/control (for the package name and
# architecture) even with -O, so give it a minimal one next to the staging tree.
shlib_dir="$build_dir/shlibs"
mkdir -p "$shlib_dir/debian"
cat > "$shlib_dir/debian/control" <<'EOF'
Source: sticky-notes
Section: utils
Priority: optional
Maintainer: Daniele Arrighi <daniele.arrighi@gmail.com>
Standards-Version: 4.6.2

Package: sticky-notes
Architecture: any
Description: desktop sticky notes for KDE Plasma (Wayland)
 Sticky Notes.
EOF
shlib_dir_rel="${shlib_dir#"$root/"}"
deps_out="$build_dir/shlibs-depends"
if ! "$engine" run "${run_args[@]}" "$image" bash -lc \
        "cd '/work/$shlib_dir_rel' && dpkg-shlibdeps -O '/work/$stage_rel/usr/bin/sticky-notes'" \
        > "$deps_out"; then
    die "dpkg-shlibdeps failed (see $deps_out)"
fi
depends="$(sed -n 's/^shlibs:Depends=//p' "$deps_out" | tail -1)"
[[ -n "$depends" ]] \
    || die "dpkg-shlibdeps did not produce a Depends line (see $deps_out)"

# Plugins are loaded at runtime, so dpkg-shlibdeps cannot see them: the Wayland
# QPA plugin (qt6-wayland, separate from QtGui on Debian/Ubuntu) and the SQLite
# driver that Storage needs.
depends+=", qt6-wayland, libqt6sql6-sqlite"

# dpkg-shlibdeps only sees the LayerShellQt soname, and the trixie shlibs entry
# reports the ABI baseline (>= 6.1.0), not the API version the app is built
# against. Pin 6.3.4 (the version in Debian 13): older releases are untested and
# the two APIs added after 6.3 are compiled out (see CMakeLists.txt).
if [[ "$depends" == *"liblayershellqtinterface6"* ]]; then
    depends="$(sed -E 's/liblayershellqtinterface6( \(>= [^)]*\))?/liblayershellqtinterface6 (>= 6.3.4)/' <<<"$depends")"
else
    depends+=", liblayershellqtinterface6 (>= 6.3.4)"
fi

installed_size="$(du -sk --exclude=DEBIAN "$stage" | cut -f1)"
(
    cd "$stage"
    find . -type f ! -path './DEBIAN/*' | sed 's|^\./||' | LC_ALL=C sort \
        | xargs -r md5sum > DEBIAN/md5sums
)

cat > "$stage/DEBIAN/control" <<EOF
Package: sticky-notes
Version: $debian_version
Section: utils
Priority: optional
Architecture: $debarch
Depends: $depends
Recommends: plasma-integration | breeze
Installed-Size: $installed_size
Maintainer: Daniele Arrighi <daniele.arrighi@gmail.com>
Homepage: https://github.com/danielearrighi/kde-sticky-notes
Description: desktop sticky notes for KDE Plasma (Wayland)
 Sticky Notes places colored notes directly on the KDE Plasma desktop as
 Wayland layer-shell surfaces: they stay under (or above) normal windows and
 out of the taskbar and of Alt+Tab.
 .
 Each note supports Markdown formatting, a per-note theme, opacity, the layer
 and the size. A hub window manages the notes (grid, trash, settings, backup
 and restore), the system tray and the start at login.
EOF

# --- 5. Package -------------------------------------------------------------

mkdir -p "$release_dir"
log "Building $asset"
if ! "$engine" run "${run_args[@]}" "$image" bash -lc \
        "dpkg-deb --root-owner-group -Zxz --build '$stage_rel' 'release/$pkgname.deb'"; then
    die "dpkg-deb failed"
fi
[[ -f "$asset" ]] || die "dpkg-deb did not produce $asset"

# The staging folder is only the package tree: drop it so build-deb-<arch>/
# keeps the shlibs output and nothing else.
rm -rf "$stage"

log "Done: $asset ($(du -h "$asset" | cut -f1))"
