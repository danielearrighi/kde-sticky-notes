#!/usr/bin/env bash
# Build an RPM package (.rpm) for Sticky Notes (Fedora, openSUSE).
#
# Packaging needs no compiler: the binary is the portable build made by
# scripts/build_portable.sh (Qt 6.8 + LayerShellQt 6.3.4, glibc from Debian 13),
# which runs on Fedora 44 and openSUSE Tumbleweed. This script stages
# it and runs rpmbuild inside scripts/container/fedora44.Dockerfile, so the host
# distribution does not matter.
#
# Produces release/sticky-notes-<version>-1.<rpmarch>.rpm (x86_64). Only x86_64
# is published, but the script accepts --arch aarch64 too (the build then goes
# through QEMU and produces an aarch64 package).
#
# The binary does not bundle Qt: it is the plain release, like the tarball.
# Use the AppImage or the Flatpak on distributions with an older Qt.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

release_dir="$root/release"
desktop="$root/scripts/sticky-notes.desktop"
icon_dir="$root/packaging/icons"
metainfo="$root/packaging/io.github.danielearrighi.StickyNotes.metainfo.xml"
spec="$root/packaging/sticky-notes.spec"

arch=""
binary_override=""
skip_build=0
clean_only=0
rebuild_image=0
engine=""

usage() {
    cat <<'EOF'
Usage: scripts/build_rpm.sh [options]

Builds release/sticky-notes-<version>-1.<rpmarch>.rpm in the Fedora 44
container (built on first use). The package uses the system Qt6 and
LayerShellQt, like the tarball.

Options:
  --arch ARCH      target architecture: x86_64 or aarch64 (default: uname -m);
                   only x86_64 is published, aarch64 needs QEMU
  --binary PATH    reuse an existing binary (implies --skip-build)
  --engine NAME    container engine to use (podman or docker; default: auto)
  --image TAG      build image tag (default: localhost/sticky-notes-rpmbuild:fedora44;
                   the architecture is appended; an unqualified name is treated
                   as local)
  --rebuild-image  rebuild the image from scratch (--no-cache)
  --skip-build     do not build the binary, reuse build-release-<arch>/sticky-notes
  --clean          remove build-rpm-<arch> and this architecture's .rpm, then exit
  -h, --help       show this help

Environment:
  CONTAINER_ENGINE          same as --engine
  STICKY_NOTES_RPMBUILD_IMAGE  same as --image
EOF
}

while (($#)); do
    case "$1" in
        --arch)          arch="${2:?--arch needs an architecture}"; shift ;;
        --binary)        binary_override="${2:?--binary needs a path}"; shift ;;
        --engine)        engine="${2:?--engine needs a name}"; shift ;;
        --image)         image_base="${2:?--image needs a tag}"; shift ;;
        --rebuild-image) rebuild_image=1 ;;
        --skip-build)    skip_build=1 ;;
        --clean)         clean_only=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

log() { printf '[build_rpm] %s\n' "$*"; }
die() { printf 'build_rpm: %s\n' "$*" >&2; exit 1; }

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || die "Unsupported architecture: $arch (need x86_64 or aarch64)"
rpmarch="$(sn_rpm_arch "$arch")"

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"

build_dir="$root/build-rpm-$arch"
topdir="$build_dir/rpmbuild"
asset="$release_dir/sticky-notes-${version}-1.${rpmarch}.rpm"

if ((clean_only)); then
    rm -rf "$build_dir"
    rm -f "$release_dir/sticky-notes-"*"-1.${rpmarch}.rpm"
    log "Removed $build_dir and the $rpmarch .rpm packages from $release_dir"
    exit 0
fi

for f in "$desktop" "$metainfo" "$spec"; do
    [[ -f "$f" ]] || die "Missing required file: $f"
done
for size in 16 22 32 48 64 128 256; do
    [[ -f "$icon_dir/sticky-notes-$size.png" ]] \
        || die "Missing required file: $icon_dir/sticky-notes-$size.png"
done

# --- 1. Release binary ------------------------------------------------------

if [[ -n "$binary_override" ]]; then
    binary="$binary_override"
    [[ -x "$binary" ]] || die "Binary not found: $binary"
    log "Using $binary"
else
    # Portable build against Qt 6.8 + LayerShellQt 6.3.4 (see build_portable.sh).
    binary="$root/build-release-$arch/sticky-notes"
    portable_args=(--arch "$arch")
    ((skip_build)) && portable_args+=(--skip-build)
    [[ -n "$engine" ]] && portable_args+=(--engine "$engine")
    "$here/build_portable.sh" "${portable_args[@]}"
    log "Using $binary"
fi
[[ -x "$binary" ]] || die "Binary not found: $binary"
sn_verify_binary_arch "$binary" "$arch"
log "Version: $version"
log "Architecture: $arch (RPM: $rpmarch)"

# --- 2. Container -----------------------------------------------------------

if [[ -z "$engine" ]]; then
    engine="${CONTAINER_ENGINE:-}"
fi
if [[ -z "$engine" ]]; then
    engine="$(sn_container_engine)" || die "No container engine found: install podman or docker"
fi
command -v "$engine" >/dev/null 2>&1 || die "Container engine not found: $engine"

dockerfile="$here/container/fedora44.Dockerfile"
image_base="${image_base:-${STICKY_NOTES_RPMBUILD_IMAGE:-localhost/sticky-notes-rpmbuild:fedora44}}"
image="$(sn_local_image "$image_base-$arch")"

sn_require_binfmt "$arch"
platform_args=()
sn_is_native "$arch" || platform_args=(--platform "$(sn_platform "$arch")")

if ((rebuild_image)); then
    log "Rebuilding $image from scratch"
    "$engine" build --no-cache "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
elif ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    log "Building $image (first run only)"
    "$engine" build "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
else
    log "Reusing image $image"
fi

run_args=(--rm --pull=never "${platform_args[@]}" -v "$root:/work:Z" -w /work -e HOME=/tmp)
if [[ "$engine" == "podman" ]]; then
    run_args+=(--userns=keep-id)
else
    run_args+=(--user "$(id -u):$(id -g)")
fi

# --- 3. Source tree ---------------------------------------------------------

log "Preparing $topdir"
rm -rf "$build_dir"
mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

srcname="sticky-notes-$version"
srcroot="$topdir/SOURCES/$srcname"
mkdir -p "$srcroot/icons"

install -Dm755 "$binary"  "$srcroot/sticky-notes"
install -Dm644 "$desktop" "$srcroot/sticky-notes.desktop"
for size in 16 22 32 48 64 128 256; do
    install -Dm644 "$icon_dir/sticky-notes-$size.png" \
        "$srcroot/icons/sticky-notes-$size.png"
done
# The AppStream metadata keeps its reverse-DNS component id, but the desktop
# file is called sticky-notes.desktop (main.cpp sets that name outside Flatpak),
# so the launchable has to point at it.
sed 's|io\.github\.danielearrighi\.StickyNotes\.desktop|sticky-notes.desktop|g' \
    "$metainfo" > "$srcroot/sticky-notes.metainfo.xml"

source_tar="$topdir/SOURCES/$srcname.tar.gz"
tar --owner=0 --group=0 --numeric-owner \
    -C "$topdir/SOURCES" -czf "$source_tar" "$srcname"
rm -rf "$srcroot"

# --- 4. Package -------------------------------------------------------------

date_rfc="$(LC_ALL=C date '+%a %b %d %Y')"
build_dir_name="build-rpm-$arch"

log "Building $asset"
if ! "$engine" run "${run_args[@]}" "$image" bash -lc \
        "rpmbuild -bb --target '$rpmarch' \
            --define '_topdir /work/$build_dir_name/rpmbuild' \
            --define 'pkg_version $version' \
            --define 'sn_date $date_rfc' \
            /work/packaging/sticky-notes.spec"; then
    die "rpmbuild failed"
fi

built="$topdir/RPMS/$rpmarch/sticky-notes-${version}-1.${rpmarch}.rpm"
[[ -f "$built" ]] || die "rpmbuild did not produce $built"

mkdir -p "$release_dir"
install -m644 "$built" "$asset"
log "Done: $asset ($(du -h "$asset" | cut -f1))"
