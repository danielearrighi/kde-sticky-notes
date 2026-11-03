#!/usr/bin/env bash
# Build every release artifact for the current version in one go:
#
#   tarball   release/sticky-notes-<version>-<arch>.tar.gz  (scripts/build_release.sh)
#   appimage  release/sticky-notes-<version>-<arch>.AppImage (scripts/build_appimage.sh)
#   flatpak   release/sticky-notes-<version>-<arch>.flatpak  (scripts/build_flatpak.sh)
#
# This is only a convenience wrapper: each script keeps its own requirements and
# its own options, and is still the one to use for a single artifact. The three
# builds are independent, so a failure does not stop the others with
# --keep-going; by default the script stops at the first failure.
#
# Usage: scripts/build_all.sh [options]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root
release_dir="$root/release"

all_targets=(tarball appimage flatpak)
targets=("${all_targets[@]}")

native=0
skip_build=0
keep_appdir=0
install_flatpak=0
keep_going=0
clean_only=0
only_list=""
skip_list=""

usage() {
    cat <<'EOF'
Usage: scripts/build_all.sh [options]

Builds the tarball, the AppImage and the Flatpak bundle for the version in
CMakeLists.txt, in that order.

Options:
  --only LIST        build only these targets (comma separated):
                     tarball, appimage, flatpak
  --skip LIST        skip these targets (comma separated)
  --native           build the tarball on the host instead of the container
                     (local testing only: the tarball may require a newer Qt)
  --skip-build       reuse the existing builds (passes --skip-build to each
                     script: build/sticky-notes, build-release/sticky-notes and
                     build-flatpak/repo must already exist)
  --keep-appdir      keep scripts/AppDir after the AppImage is created
  --install          install the Flatpak bundle after building it
  --keep-going       continue with the remaining targets when one fails
  --clean            run the --clean step of every target, then exit
  -h, --help         show this help

Examples:
  scripts/build_all.sh                      # all three
  scripts/build_all.sh --skip flatpak       # tarball + AppImage
  scripts/build_all.sh --only tarball --native
  scripts/build_all.sh --keep-going --install
EOF
}

die() { printf 'build_all: %s\n' "$*" >&2; exit 1; }
log() { printf '[build_all] %s\n' "$*"; }

while (($#)); do
    case "$1" in
        --only)         only_list="${2:?--only needs a list}"; shift ;;
        --skip)         skip_list="${2:?--skip needs a list}"; shift ;;
        --native)       native=1 ;;
        --skip-build)   skip_build=1 ;;
        --keep-appdir)  keep_appdir=1 ;;
        --install)      install_flatpak=1 ;;
        --keep-going)   keep_going=1 ;;
        --clean)        clean_only=1 ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

split_list() { tr ',' '\n' <<<"$1" | sed '/^[[:space:]]*$/d'; }

is_target() { # name
    local name="$1" t
    for t in "${all_targets[@]}"; do
        [[ "$t" == "$name" ]] && return 0
    done
    return 1
}

# --- target selection -------------------------------------------------------

if [[ -n "$only_list" ]]; then
    targets=()
    while read -r name; do
        is_target "$name" || die "Unknown target '$name' (choose from: ${all_targets[*]})"
        targets+=("$name")
    done < <(split_list "$only_list")
    ((${#targets[@]})) || die "--only needs at least one target"
fi

if [[ -n "$skip_list" ]]; then
    while read -r name; do
        is_target "$name" || die "Unknown target '$name' (choose from: ${all_targets[*]})"
    done < <(split_list "$skip_list")

    kept=()
    for t in "${targets[@]}"; do
        skipped=0
        while read -r name; do
            [[ "$name" == "$t" ]] && skipped=1
        done < <(split_list "$skip_list")
        ((skipped)) || kept+=("$t")
    done
    targets=("${kept[@]}")
    ((${#targets[@]})) || die "Every target was skipped: nothing to do"
fi

# --- inputs -----------------------------------------------------------------

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"

arch="$(uname -m)"
case "$arch" in
    x86_64|aarch64) ;;
    *) die "Unsupported architecture: $arch (need x86_64 or aarch64)" ;;
esac

script_for() {
    case "$1" in
        tarball)  printf '%s\n' "$here/build_release.sh" ;;
        appimage) printf '%s\n' "$here/build_appimage.sh" ;;
        flatpak)  printf '%s\n' "$here/build_flatpak.sh" ;;
    esac
}

asset_for() { # target -> release file
    case "$1" in
        tarball)  printf '%s\n' "$release_dir/sticky-notes-$version-$arch.tar.gz" ;;
        appimage) printf '%s\n' "$release_dir/sticky-notes-$version-$arch.AppImage" ;;
        flatpak)  printf '%s\n' "$release_dir/sticky-notes-$version-$arch.flatpak" ;;
    esac
}

for target in "${targets[@]}"; do
    script="$(script_for "$target")"
    [[ -f "$script" ]] || die "Missing $script"
    [[ -x "$script" ]] || die "Not executable: $script"
done

# --- clean ------------------------------------------------------------------

if ((clean_only)); then
    for target in "${targets[@]}"; do
        script="$(script_for "$target")"
        log "Cleaning $target ($(basename "$script") --clean)"
        "$script" --clean
    done
    log "Done"
    exit 0
fi

# --- build ------------------------------------------------------------------

log "Version: $version"
log "Architecture: $arch"
log "Targets: ${targets[*]}"

declare -A status=()
failed=()
start=$SECONDS

for target in "${targets[@]}"; do
    script="$(script_for "$target")"
    args=()
    case "$target" in
        tarball)
            if ((native)); then args+=(--native); fi
            ;;
        appimage)
            if ((keep_appdir)); then args+=(--keep-appdir); fi
            ;;
        flatpak)
            if ((install_flatpak)); then args+=(--install); fi
            ;;
    esac
    if ((skip_build)); then args+=(--skip-build); fi

    log "=== $target: $(basename "$script")${args[*]:+ ${args[*]}} ==="
    if "$script" "${args[@]}"; then
        status[$target]="ok"
    else
        rc=$?
        status[$target]="failed (exit $rc)"
        failed+=("$target")
        if ((keep_going)); then
            log "$target failed (exit $rc), continuing with the remaining targets"
        else
            log "$target failed (exit $rc); stopping (use --keep-going to build the rest)"
            break
        fi
    fi
done

# --- summary ----------------------------------------------------------------

printf '\n'
log "Summary for $version ($arch), $((SECONDS - start))s:"
for target in "${targets[@]}"; do
    asset="$(asset_for "$target")"
    state="${status[$target]:-not built}"
    if [[ "$state" == ok && -f "$asset" ]]; then
        printf '  %-9s %-14s %s (%s)\n' "$target" "$state" "$asset" "$(du -h "$asset" | cut -f1)"
    elif [[ -f "$asset" ]]; then
        # A failed rebuild leaves the previous bundle in place: do not let it
        # pass for the output of this run.
        printf '  %-9s %-14s %s (stale, not rebuilt)\n' "$target" "$state" "$asset"
    else
        printf '  %-9s %-14s %s (missing)\n' "$target" "$state" "$asset"
    fi
done

if ((${#failed[@]})); then
    printf '\n'
    log "Failed: ${failed[*]}"
    exit 1
fi

if ((${#targets[@]} == ${#all_targets[@]})); then
    printf '\n'
    log "All three artifacts are ready: scripts/create_release.sh uploads them"
fi
