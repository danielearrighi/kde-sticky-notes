#!/usr/bin/env bash
# Build the plain (tarball) release binary against an older Qt baseline.
#
# The tarball does not bundle Qt: it runs against the system libraries. Qt is
# forward compatible (a binary built against 6.10 runs on 6.11+) but NOT
# backward compatible, so building on the maintainer's rolling distribution
# (Tumbleweed, Qt 6.12) makes the tarball fail on Fedora 44 (Qt 6.11.2) with:
#
#   .local/bin/sticky-notes: /lib64/libQt6Core.so.6: version `Qt_6.12' not found
#
# This script builds inside scripts/container/ubuntu26.04.Dockerfile
# (Qt 6.10, LayerShellQt 6.6: the oldest versions the app supports) and writes
# build-release-<arch>/sticky-notes. The image is built once and cached.
#
# --arch selects the target: the default is the host architecture. A foreign
# architecture is emulated with QEMU user mode (binfmt_misc), so an x86_64 host
# can produce the aarch64 tarball binary; the image is built per architecture.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

dockerfile="$here/container/ubuntu26.04.Dockerfile"
image_base="${STICKY_NOTES_BUILD_IMAGE:-sticky-notes-build:ubuntu26.04}"

arch=""
engine=""
skip_build=0
rebuild_image=0

usage() {
    cat <<'EOF'
Usage: scripts/build_portable.sh [options]

Builds build-release-<arch>/sticky-notes against Qt 6.10 + LayerShellQt 6.6 in a
container (Ubuntu 26.04), so the release tarball also runs on distributions
with a newer Qt than the build machine.

Options:
  --arch ARCH      target architecture: x86_64 or aarch64 (default: uname -m).
                   A foreign architecture needs QEMU user mode (binfmt_misc)
  --engine NAME    container engine to use (podman or docker; default: auto)
  --image TAG      base build image tag (default: sticky-notes-build:ubuntu26.04;
                   the architecture is appended, so each arch has its own image)
  --rebuild-image  rebuild the image from scratch (--no-cache)
  --skip-build     do not build, only check build-release-<arch>/sticky-notes
  -h, --help       show this help

Environment:
  CONTAINER_ENGINE          same as --engine
  STICKY_NOTES_BUILD_IMAGE  same as --image
EOF
}

while (($#)); do
    case "$1" in
        --arch)          arch="${2:?--arch needs an architecture}"; shift ;;
        --engine)        engine="${2:?--engine needs a name}"; shift ;;
        --image)         image_base="${2:?--image needs a tag}"; shift ;;
        --rebuild-image) rebuild_image=1 ;;
        --skip-build)    skip_build=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

log() { printf '[build_portable] %s\n' "$*"; }
die() { printf 'build_portable: %s\n' "$*" >&2; exit 1; }

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || die "Unsupported architecture: $arch (need x86_64 or aarch64)"

platform="$(sn_platform "$arch")"
image="$image_base-$arch"
build_dir_name="build-release-$arch"
build_dir="$root/$build_dir_name"
binary="$build_dir/sticky-notes"

if ((skip_build)); then
    [[ -x "$binary" ]] || die "Binary not found: $binary (drop --skip-build)"
    log "Reusing $binary ($arch)"
    exit 0
fi

[[ -f "$dockerfile" ]] || die "Missing $dockerfile"

# A foreign architecture runs the container's toolchain through QEMU.
sn_require_binfmt "$arch"
platform_args=()
sn_is_native "$arch" || platform_args=(--platform "$platform")

# --- container engine -------------------------------------------------------

if [[ -z "$engine" ]]; then
    engine="${CONTAINER_ENGINE:-}"
fi
if [[ -z "$engine" ]]; then
    for candidate in podman docker; do
        if command -v "$candidate" >/dev/null 2>&1; then
            engine="$candidate"
            break
        fi
    done
fi
[[ -n "$engine" ]] || die "No container engine found: install podman or docker, or use build_release.sh --native"
command -v "$engine" >/dev/null 2>&1 || die "Container engine not found: $engine"
log "Container engine: $engine"

# --- image ------------------------------------------------------------------

if ((rebuild_image)); then
    log "Rebuilding $image from scratch"
    "$engine" build --no-cache "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
elif ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    log "Building $image (first run only)"
    "$engine" build "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
else
    log "Reusing image $image"
fi

# --- build ------------------------------------------------------------------

mkdir -p "$build_dir"
if [[ ! -w "$build_dir" ]]; then
    # A previous rootful docker run can leave the directory owned by root.
    log "Fixing ownership of $build_dir"
    "$engine" run --rm "${platform_args[@]}" -v "$root:/work:Z" -w /work "$image" \
        chown -R "$(id -u):$(id -g)" "$build_dir_name"
fi

# Run as the invoking user so the build tree is not left owned by root. podman
# needs keep-id (its default rootless mapping would turn the host user into a
# subuid); docker (rootful) just needs --user.
run_args=(--rm "${platform_args[@]}" -v "$root:/work:Z" -w /work -e HOME=/tmp)
if [[ "$engine" == "podman" ]]; then
    run_args+=(--userns=keep-id)
else
    run_args+=(--user "$(id -u):$(id -g)")
fi

log "Building $arch in $image"
"$engine" run "${run_args[@]}" "$image" bash -lc \
    "cmake -S . -B $build_dir_name -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $build_dir_name"

[[ -x "$binary" ]] || die "Build did not produce $binary"

required_qt="$(readelf -V "$binary" 2>/dev/null | grep -o 'Qt_6\.[0-9][0-9]*' | sort -Vu | tail -1 || true)"
log "Built $binary ($arch)"
if [[ -n "$required_qt" ]]; then
    log "Requires Qt ${required_qt#Qt_} or newer"
    # Tripwire: the whole point of this script is the Qt 6.10 baseline. If the
    # container's Qt moves forward (or the app starts using newer APIs), the
    # tarball silently becomes less portable.
    expected_qt="Qt_6.10"
    if [[ "$(printf '%s\n' "$expected_qt" "$required_qt" | sort -V | tail -1)" != "$expected_qt" ]]; then
        die "the built binary requires $required_qt, newer than the $expected_qt baseline"
    fi
fi
