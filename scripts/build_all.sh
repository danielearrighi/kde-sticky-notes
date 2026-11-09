#!/usr/bin/env bash
# Build every release artifact for the current version in one go.
#
# For each target architecture (x86_64 and aarch64 by default, the host one
# first):
#
#   tarball   release/sticky-notes-<version>-<arch>.tar.gz  (scripts/build_release.sh)
#   appimage  release/sticky-notes-<version>-<arch>.AppImage (scripts/build_appimage.sh)
#   flatpak   release/sticky-notes-<version>-<arch>.flatpak  (scripts/build_flatpak.sh)
#
# The architecture that is not the host one ("foreign") is built through QEMU
# user emulation (binfmt_misc): the tarball and the AppImage run the target
# toolchain in an Ubuntu 26.04 container, the Flatpak builds inside the KDE
# runtime of that architecture. An aarch64 host needs nothing special, an
# x86_64 host can produce the ARM artifacts once QEMU is installed (the scripts
# print the package name per distribution when it is missing).
#
# This is only a convenience wrapper: each script keeps its own requirements and
# its own options, and is still the one to use for a single artifact. The
# builds are independent, so a failure does not stop the others with
# --keep-going; by default the script stops at the first failure.
#
# Usage: scripts/build_all.sh [options]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

release_dir="$root/release"

all_targets=(tarball appimage flatpak)
targets=("${all_targets[@]}")
host_arch="$(sn_host_arch)"
all_archs=(x86_64 aarch64)
archs=("$host_arch" "$(sn_other_arch)")

native=0
skip_build=0
keep_appdir=0
install_flatpak=0
keep_going=0
clean_only=0
only_list=""
skip_list=""
arch_list=""

usage() {
    cat <<'EOF'
Usage: scripts/build_all.sh [options]

Builds the tarball, the AppImage and the Flatpak bundle for the version in
CMakeLists.txt, in that order, for every requested architecture.

Options:
  --archs LIST       architectures to build (comma separated): x86_64, aarch64
                     (default: this host plus the other one, host first)
  --only LIST        build only these targets (comma separated):
                     tarball, appimage, flatpak
  --skip LIST        skip these targets (comma separated)
  --native           build the tarball on the host instead of the container
                     (host architecture only, local testing: the tarball may
                     require a newer Qt)
  --skip-build       reuse the existing builds (passes --skip-build to each
                     script: build-release-<arch>/sticky-notes,
                     build-appimage-<arch>/sticky-notes and
                     build-flatpak-<arch>/repo must already exist)
  --keep-appdir      keep scripts/AppDir after the AppImage is created
  --install          install the Flatpak bundle after building it (host
                     architecture only)
  --keep-going       continue with the remaining targets when one fails
  --clean            run the --clean step of every target, then exit
  -h, --help         show this help

Examples:
  scripts/build_all.sh                       # x86_64 + aarch64, all targets
  scripts/build_all.sh --archs x86_64        # only the host architecture
  scripts/build_all.sh --skip flatpak        # tarballs + AppImages
  scripts/build_all.sh --only tarball --native
  scripts/build_all.sh --keep-going --install
EOF
}

die() { printf 'build_all: %s\n' "$*" >&2; exit 1; }
log() { printf '[build_all] %s\n' "$*"; }

while (($#)); do
    case "$1" in
        --archs)        arch_list="${2:?--archs needs a list}"; shift ;;
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

split_list() { tr ',' '\n' <<<"$1" | tr -s '[:space:]' '\n' | sed '/^$/d'; }

is_target() { # name
    local name="$1" t
    for t in "${all_targets[@]}"; do
        [[ "$t" == "$name" ]] && return 0
    done
    return 1
}

is_arch() { # name
    local name="$1" a
    for a in "${all_archs[@]}"; do
        [[ "$a" == "$name" ]] && return 0
    done
    return 1
}

# --- selection --------------------------------------------------------------

if [[ -n "$arch_list" ]]; then
    archs=()
    while read -r name; do
        is_arch "$name" || die "Unknown architecture '$name' (choose from: ${all_archs[*]})"
        already=0
        for a in "${archs[@]}"; do [[ "$a" == "$name" ]] && already=1; done
        ((already)) || archs+=("$name")
    done < <(split_list "$arch_list")
    ((${#archs[@]})) || die "--archs needs at least one architecture"
fi

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

script_for() {
    case "$1" in
        tarball)  printf '%s\n' "$here/build_release.sh" ;;
        appimage) printf '%s\n' "$here/build_appimage.sh" ;;
        flatpak)  printf '%s\n' "$here/build_flatpak.sh" ;;
    esac
}

asset_for() { # target arch -> release file
    case "$1" in
        tarball)  printf '%s\n' "$release_dir/sticky-notes-$version-$2.tar.gz" ;;
        appimage) printf '%s\n' "$release_dir/sticky-notes-$version-$2.AppImage" ;;
        flatpak)  printf '%s\n' "$release_dir/sticky-notes-$version-$2.flatpak" ;;
    esac
}

for target in "${targets[@]}"; do
    script="$(script_for "$target")"
    [[ -f "$script" ]] || die "Missing $script"
    [[ -x "$script" ]] || die "Not executable: $script"
done

# --- clean ------------------------------------------------------------------

if ((clean_only)); then
    for arch in "${archs[@]}"; do
        for target in "${targets[@]}"; do
            script="$(script_for "$target")"
            log "Cleaning $target for $arch ($(basename "$script") --clean)"
            "$script" --arch "$arch" --clean
        done
    done
    log "Done"
    exit 0
fi

# The foreign architectures run their toolchain through QEMU: fail early with
# the install command instead of failing inside the first container run.
for arch in "${archs[@]}"; do
    sn_require_binfmt "$arch"
done

# --- build ------------------------------------------------------------------

log "Version: $version"
log "Host architecture: $host_arch"
log "Architectures: ${archs[*]}"
log "Targets: ${targets[*]}"

declare -A status=()
failed=()
start=$SECONDS

for arch in "${archs[@]}"; do
    for target in "${targets[@]}"; do
        script="$(script_for "$target")"
        label="$arch/$target"
        args=(--arch "$arch")
        case "$target" in
            tarball)
                if ((native)); then
                    if [[ "$arch" == "$host_arch" ]]; then
                        args+=(--native)
                    else
                        log "Skipping --native for $target on $arch (built in the container)"
                    fi
                fi
                ;;
            appimage)
                ((keep_appdir)) && args+=(--keep-appdir)
                ;;
            flatpak)
                if ((install_flatpak)); then
                    if [[ "$arch" == "$host_arch" ]]; then
                        args+=(--install)
                    else
                        log "Skipping --install for $arch: the bundle is not for this host"
                    fi
                fi
                ;;
        esac
        if ((skip_build)); then args+=(--skip-build); fi

        log "=== $label: $(basename "$script")${args[*]:+ ${args[*]}} ==="
        if "$script" "${args[@]}"; then
            status[$label]="ok"
        else
            rc=$?
            status[$label]="failed (exit $rc)"
            failed+=("$label")
            if ((keep_going)); then
                log "$label failed (exit $rc), continuing with the remaining builds"
            else
                log "$label failed (exit $rc); stopping (use --keep-going to build the rest)"
                break 2
            fi
        fi
    done
done

# --- summary ----------------------------------------------------------------

printf '\n'
log "Summary for $version (${archs[*]}), $((SECONDS - start))s:"
for arch in "${archs[@]}"; do
    for target in "${targets[@]}"; do
        label="$arch/$target"
        asset="$(asset_for "$target" "$arch")"
        state="${status[$label]:-not built}"
        if [[ "$state" == ok && -f "$asset" ]]; then
            printf '  %-17s %-14s %s (%s)\n' "$label" "$state" "$asset" "$(du -h "$asset" | cut -f1)"
        elif [[ -f "$asset" ]]; then
            # A failed rebuild leaves the previous bundle in place: do not let it
            # pass for the output of this run.
            printf '  %-17s %-14s %s (stale, not rebuilt)\n' "$label" "$state" "$asset"
        else
            printf '  %-17s %-14s %s (missing)\n' "$label" "$state" "$asset"
        fi
    done
done

if ((${#failed[@]})); then
    printf '\n'
    log "Failed: ${failed[*]}"
    exit 1
fi

printf '\n'
log "Every requested artifact is ready: scripts/create_release.sh uploads what is in release/"
