#!/usr/bin/env bash
# Configure (first time or on demand), build and launch Sticky Notes for quick
# manual testing, so you do not have to type the CMake commands every time.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

build_dir="$root/build"
binary="$build_dir/sticky-notes"
build_type="Release"
do_build=1
do_clean=0
app_args=()

usage() {
    cat <<'EOF'
Usage: scripts/run.sh [options] [-- app arguments]

Options:
  --debug        build with CMAKE_BUILD_TYPE=Debug (default: Release)
  --no-build     launch the existing build/sticky-notes without rebuilding
  --clean        remove build/ first (full reconfigure and rebuild)
  -h, --help     show this help

Everything after "--" is forwarded to the app (e.g. -- --restart).
EOF
}

while (($#)); do
    case "$1" in
        --debug)    build_type="Debug" ;;
        --no-build) do_build=0 ;;
        --clean)    do_clean=1 ;;
        -h|--help)  usage; exit 0 ;;
        --)         shift; app_args=("$@"); break ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

log() { printf '[run] %s\n' "$*"; }

if ((do_clean)); then
    log "Removing $build_dir"
    rm -rf "$build_dir"
fi

if ((do_build)); then
    cached_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$build_dir/CMakeCache.txt" 2>/dev/null || true)"
    if [[ ! -f "$build_dir/build.ninja" || "$cached_type" != "$build_type" ]]; then
        log "Configuring ($build_type)"
        cmake -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type"
    fi

    log "Building"
    cmake --build "$build_dir"
fi

[[ -x "$binary" ]] || { echo "Binary not found: $binary" >&2; exit 1; }

log "Starting $binary"
exec "$binary" "${app_args[@]}"
