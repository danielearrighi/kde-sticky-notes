#!/usr/bin/env bash
# Build the Flatpak bundle (release/sticky-notes-<version>-<arch>.flatpak).
#
# The manifest (packaging/io.github.danielearrighi.StickyNotes.yml) builds the
# app against org.kde.Platform 6.10 / org.kde.Sdk 6.10, which carry Qt 6.10.3,
# and compiles LayerShellQt 6.6 from source because the KDE runtime does not
# ship it. The runtime is part of the bundle's dependencies, not of the host:
# Flatpak downloads it on install, so Fedora 44, Ubuntu, Debian, etc. all run
# the same Qt.
#
# Requirements:
#   - flatpak
#   - flatpak-builder, or the org.flatpak.Builder app from Flathub
#   - org.kde.Sdk//6.10 for the target architecture:
#       flatpak install flathub org.kde.Sdk//6.10
#       flatpak install --arch=aarch64 flathub org.kde.Sdk//6.10
#   - for --arch of another architecture: QEMU user mode (binfmt_misc), because
#     flatpak-builder runs the target toolchain to compile LayerShellQt and the
#     app inside the runtime of that architecture
#
# Usage: scripts/build_flatpak.sh [--arch ARCH] [--install] [--skip-build] [--clean]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

app_id="io.github.danielearrighi.StickyNotes"
branch="stable"
manifest="$root/packaging/$app_id.yml"
release_dir="$root/release"

arch=""
runtime_version=""
install_bundle=0
skip_build=0
clean_only=0

usage() {
    cat <<'EOF'
Usage: scripts/build_flatpak.sh [options]

Builds release/sticky-notes-<version>-<arch>.flatpak from the current working
tree, against org.kde.Platform 6.10 (Qt 6.10) and LayerShellQt 6.6.

Options:
  --arch ARCH      target architecture: x86_64 or aarch64 (default: uname -m).
                   A foreign architecture needs the SDK for that architecture
                   and QEMU user mode (binfmt_misc)
  --install        install the bundle with "flatpak install --user --reinstall"
                   (host architecture only)
  --skip-build     reuse the build/repo from a previous run, only rebuild the bundle
  --clean          remove the build tree of this architecture and the generated
                   .flatpak, then exit
  -h, --help       show this help

Environment:
  FLATPAK_BUILDER  path of a flatpak-builder binary to use
EOF
}

while (($#)); do
    case "$1" in
        --arch)          arch="${2:?--arch needs an architecture}"; shift ;;
        --install)       install_bundle=1 ;;
        --skip-build)    skip_build=1 ;;
        --clean)         clean_only=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || { echo "Unsupported architecture: $arch (need x86_64 or aarch64)" >&2; exit 1; }

work="$root/build-flatpak-$arch"
build_dir="$work/build"
repo_dir="$work/repo"
state_dir="$work/state"

log() { printf '[build_flatpak] %s\n' "$*"; }
die() { printf 'build_flatpak: %s\n' "$*" >&2; exit 1; }

if ((clean_only)); then
    rm -rf "$work"
    rm -f "$release_dir"/*-"$arch".flatpak
    log "Removed $work and the .flatpak bundles for $arch in $release_dir"
    exit 0
fi

command -v flatpak >/dev/null 2>&1 || die "flatpak is not installed"

# --- architecture and manifest ---------------------------------------------

[[ -f "$manifest" ]] || die "Missing manifest: $manifest"

# Keep the check and the messages in sync with the manifest instead of carrying
# a second copy of the runtime branch.
runtime_version="$(sed -n "s/^runtime-version:[[:space:]]*['\"]\{0,1\}\([^'\"]*\)['\"]\{0,1\}[[:space:]]*$/\1/p" "$manifest" | head -1)"
[[ -n "$runtime_version" ]] || die "Could not read runtime-version from $manifest"

# --- builder ----------------------------------------------------------------

builder=()
if [[ -n "${FLATPAK_BUILDER:-}" ]]; then
    command -v "$FLATPAK_BUILDER" >/dev/null 2>&1 \
        || die "FLATPAK_BUILDER is not executable: $FLATPAK_BUILDER"
    builder=("$FLATPAK_BUILDER")
elif command -v flatpak-builder >/dev/null 2>&1; then
    builder=(flatpak-builder)
elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    # Convenience for systems that only ship flatpak (the Builder is a Flatpak
    # app itself and has the host access needed to build and install).
    #
    # The Builder calls the host flatpak CLI for the SDK/runtime lookups and
    # passes its sandbox environment along. There XDG_DATA_HOME points inside
    # ~/.var/app/org.flatpak.Builder, so the host flatpak would look for user
    # runtimes in the Builder's own data directory. Point FLATPAK_USER_DIR at
    # the real user installation (the same one this script checks below).
    flatpak_user_dir="${FLATPAK_USER_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/flatpak}"
    builder=(flatpak run --env=FLATPAK_USER_DIR="$flatpak_user_dir"
             --command=flatpak-builder org.flatpak.Builder)
else
    die "flatpak-builder not found: install it (e.g. dnf install flatpak-builder) or org.flatpak.Builder from Flathub"
fi

# A foreign architecture is compiled inside the runtime through QEMU user mode.
sn_require_binfmt "$arch"

flatpak info --arch="$arch" "org.kde.Sdk//$runtime_version" >/dev/null 2>&1 \
    || die "Missing org.kde.Sdk//$runtime_version for $arch: flatpak install --arch=$arch flathub org.kde.Sdk//$runtime_version"

# --- version ----------------------------------------------------------------

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"

# --- build ------------------------------------------------------------------

if ((skip_build)); then
    [[ -d "$repo_dir" ]] || die "No previous build in $repo_dir (drop --skip-build)"
    log "Reusing $repo_dir"
else
    mkdir -p "$work"
    log "Building $app_id $version ($arch) against org.kde.Platform $runtime_version"
    "${builder[@]}" \
        --user \
        --force-clean \
        --arch="$arch" \
        --repo="$repo_dir" \
        --state-dir="$state_dir" \
        "$build_dir" \
        "$manifest"
fi

# --- bundle -----------------------------------------------------------------

mkdir -p "$release_dir"
bundle="$release_dir/sticky-notes-$version-$arch.flatpak"
rm -f "$bundle"

log "Creating $bundle"
flatpak build-bundle \
    --arch="$arch" \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$repo_dir" \
    "$bundle" \
    "$app_id" \
    "$branch"

[[ -f "$bundle" ]] || die "Build did not produce $bundle"
log "Built $bundle ($(du -h "$bundle" | cut -f1))"
log "Requires the Flatpak runtime org.kde.Platform/$arch/$runtime_version"

if ((install_bundle)); then
    if sn_is_native "$arch"; then
        log "Installing $bundle"
        flatpak install --user --noninteractive --reinstall "$bundle"
        log "Run it with: flatpak run $app_id"
    else
        log "Not installing $bundle: it is built for $arch, not for this host"
    fi
fi
