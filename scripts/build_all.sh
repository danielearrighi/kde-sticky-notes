#!/usr/bin/env bash
# Build every release artifact for the current version in one go.
#
# For each target architecture (x86_64 and aarch64 by default, the host one
# first):
#
#   tarball   release/sticky-notes-<version>-<arch>.tar.gz   (scripts/build_release.sh)
#   appimage  release/sticky-notes-<version>-<arch>.AppImage  (scripts/build_appimage.sh)
#   flatpak   release/sticky-notes-<version>-<arch>.flatpak   (scripts/build_flatpak.sh)
#   deb       release/sticky-notes_<version>-1_amd64.deb      (scripts/build_deb.sh)
#   rpm       release/sticky-notes-<version>-1.x86_64.rpm     (scripts/build_rpm.sh)
#
# The tarball, AppImage and Flatpak are published for x86_64 and aarch64; the
# .deb and .rpm are x86_64 only (the distributions they target run on PCs), so
# they are skipped for the other architectures.
#
# The architecture that is not the host one ("foreign") is built through QEMU
# user emulation (binfmt_misc): the tarball and the AppImage run the target
# toolchain in an Ubuntu 26.04 container, the Flatpak builds inside the KDE
# runtime of that architecture. An aarch64 host needs nothing special, an
# x86_64 host can produce the ARM artifacts once QEMU is installed (the scripts
# print the package name per distribution when it is missing). The AppImage
# tools are not ELF-compatible with binfmt_misc, so build_appimage.sh mounts the
# host's static qemu interpreter and runs them through it explicitly.
#
# When the tarball is selected it runs first: the portable binary it builds is
# passed to the AppImage and rpm scripts with --binary, so they package the same
# binary instead of recompiling it. The deb is the exception: it compiles in its
# own Debian 13 container (Qt 6.8), because Debian 13 ships an older Qt than the
# tarball baseline. The Flatpak keeps its own build inside the SDK.
#
# This is only a convenience wrapper: each script keeps its own requirements and
# its own options, and is still the one to use for a single artifact. The
# builds are independent, so a failure does not stop the others with
# --keep-going; by default the script stops at the first failure.
#
# A successful run then deletes the per-target build trees (build-release-*,
# build-appimage-*, build-flatpak-*, build-deb-*, build-rpm-*): they are caches,
# the published files are all in release/, and the next run recreates a missing
# tree from scratch. A failed run keeps them so the error can be inspected and
# a rerun stays incremental.
#
# Usage: scripts/build_all.sh [options]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

release_dir="$root/release"

all_targets=(tarball appimage flatpak deb rpm)
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

Builds the tarball, the AppImage, the Flatpak bundle, the .deb and the .rpm
for the version in CMakeLists.txt, in that order, for every requested
architecture. The .deb and .rpm are only built for x86_64.

Options:
  --archs LIST       architectures to build (comma separated): x86_64, aarch64
                     (default: this host plus the other one, host first)
  --only LIST        build only these targets (comma separated):
                     tarball, appimage, flatpak, deb, rpm
  --skip LIST        skip these targets (comma separated)
  --native           build the tarball and AppImage on the host instead of the
                     container (host architecture only, local testing: the
                     artifacts may require a newer Qt/glibc)
  --skip-build       do not compile again, reuse the existing build trees
                     (passes --skip-build to each script)
  --keep-appdir      keep scripts/AppDir after the AppImage is created
  --install          install the Flatpak bundle after building it (host
                     architecture only)
  --keep-going       continue with the remaining targets when one fails
  --clean            run the --clean step of every target, then exit
  -h, --help         show this help

When the tarball is selected it runs first and its portable binary is reused by
the AppImage and rpm targets; the deb always builds inside its Debian 13
container and the Flatpak inside its SDK.

After a successful run the build trees (build-release-*, build-appimage-*,
build-flatpak-*, build-deb-*, build-rpm-*) are removed: they are only caches,
the artifacts stay in release/. A failed run keeps them for debugging.

Examples:
  scripts/build_all.sh                       # x86_64 + aarch64, all targets
  scripts/build_all.sh --archs x86_64        # only the host architecture
  scripts/build_all.sh --skip flatpak        # everything but the Flatpak
  scripts/build_all.sh --only deb,rpm        # just the distribution packages
  scripts/build_all.sh --only tarball --native
  scripts/build_all.sh --keep-going --install
EOF
}

die() { printf 'build_all: %s\n' "$*" >&2; exit 1; }
log() { printf '[build_all] %s\n' "$*"; }

# Remove the per-target build trees left by the scripts. They are pure caches:
# release/ holds every artifact and a missing tree is recreated by the next
# build. build/ is the ordinary development build and is deliberately not
# matched (the globs are the build-<target> names, not build-*).
clean_build_trees() {
    local dir size
    local -a trees=()
    for dir in "$root"/build-release* "$root"/build-appimage* \
               "$root"/build-flatpak* "$root"/build-deb* "$root"/build-rpm*; do
        if [[ -d "$dir" ]]; then
            trees+=("$dir")
        fi
    done
    ((${#trees[@]})) || return 0
    size="$(du -shc "${trees[@]}" 2>/dev/null | tail -1 | cut -f1 || true)"
    [[ "$size" == "0" ]] && size=""
    log "Removing the build trees${size:+ ($size)}; the artifacts stay in $release_dir"
    rm -rf "${trees[@]}"
}

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

# Canonical order: the tarball runs first, so the portable binary it builds can
# be reused by the other targets instead of compiling the same sources again.
ordered=()
for t in "${all_targets[@]}"; do
    for selected in "${targets[@]}"; do
        if [[ "$t" == "$selected" ]]; then
            ordered+=("$t")
            break
        fi
    done
done
targets=("${ordered[@]}")

# --- inputs -----------------------------------------------------------------

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"

script_for() {
    case "$1" in
        tarball)  printf '%s\n' "$here/build_release.sh" ;;
        appimage) printf '%s\n' "$here/build_appimage.sh" ;;
        flatpak)  printf '%s\n' "$here/build_flatpak.sh" ;;
        deb)      printf '%s\n' "$here/build_deb.sh" ;;
        rpm)      printf '%s\n' "$here/build_rpm.sh" ;;
    esac
}

asset_for() { # target arch -> release file
    case "$1" in
        tarball)  printf '%s\n' "$release_dir/sticky-notes-$version-$2.tar.gz" ;;
        appimage) printf '%s\n' "$release_dir/sticky-notes-$version-$2.AppImage" ;;
        flatpak)  printf '%s\n' "$release_dir/sticky-notes-$version-$2.flatpak" ;;
        deb)      printf '%s\n' "$release_dir/sticky-notes_${version}-1_$(sn_deb_arch "$2").deb" ;;
        rpm)      printf '%s\n' "$release_dir/sticky-notes-${version}-1.$(sn_rpm_arch "$2").rpm" ;;
    esac
}

# .deb and .rpm are built for x86_64 only: that is the architecture published
# for the target distributions.
target_archs() {
    case "$1" in
        deb|rpm) printf 'x86_64\n' ;;
        *)       printf 'x86_64 aarch64\n' ;;
    esac
}

target_supports_arch() { # target arch
    local a
    for a in $(target_archs "$1"); do
        [[ "$a" == "$2" ]] && return 0
    done
    return 1
}

targets_for_arch() { # arch -> the selected targets built for it
    local t
    for t in "${targets[@]}"; do
        target_supports_arch "$t" "$1" && printf '%s\n' "$t"
    done
}

for target in "${targets[@]}"; do
    script="$(script_for "$target")"
    [[ -f "$script" ]] || die "Missing $script"
    [[ -x "$script" ]] || die "Not executable: $script"
done

# deb/rpm exist for x86_64 only: with --only deb and aarch64 there may be
# nothing left to do.
have_work=0
for arch in "${archs[@]}"; do
    mapfile -t arch_targets < <(targets_for_arch "$arch")
    ((${#arch_targets[@]})) && have_work=1
done
((have_work)) || die "Nothing to build: ${targets[*]} is built for x86_64 only"

# --- clean ------------------------------------------------------------------

if ((clean_only)); then
    for arch in "${archs[@]}"; do
        mapfile -t arch_targets < <(targets_for_arch "$arch")
        ((${#arch_targets[@]})) || continue
        for target in "${arch_targets[@]}"; do
            script="$(script_for "$target")"
            log "Cleaning $target for $arch ($(basename "$script") --clean)"
            "$script" --arch "$arch" --clean
        done
    done
    log "Done"
    exit 0
fi

# The foreign architectures run their toolchain through QEMU: fail early with
# the install command instead of failing inside the first container run. Only
# architectures that actually have work are checked: e.g. --only deb,rpm on
# x86_64 must not ask for aarch64 emulation when no aarch64 target is built.
for arch in "${archs[@]}"; do
    mapfile -t arch_targets < <(targets_for_arch "$arch")
    ((${#arch_targets[@]})) || continue
    sn_require_binfmt "$arch"
done

# --- build ------------------------------------------------------------------

log "Version: $version"
log "Host architecture: $host_arch"
log "Architectures: ${archs[*]}"
log "Targets: ${targets[*]}"

declare -A status=()
declare -A portable_binary=()
failed=()
start=$SECONDS

for arch in "${archs[@]}"; do
    mapfile -t arch_targets < <(targets_for_arch "$arch")
    ((${#arch_targets[@]})) || continue
    for target in "${targets[@]}"; do
        target_supports_arch "$target" "$arch" \
            || log "Skipping $arch/$target: $target is built for x86_64 only"
    done
    for target in "${arch_targets[@]}"; do
        script="$(script_for "$target")"
        label="$arch/$target"
        args=(--arch "$arch")
        case "$target" in
            tarball|appimage)
                if ((native)) && [[ "$arch" == "$host_arch" ]]; then
                    args+=(--native)
                elif [[ -n "${portable_binary[$arch]:-}" ]]; then
                    # Do not compile the same sources again: package the
                    # portable binary already built for this architecture.
                    args+=(--binary "${portable_binary[$arch]}")
                elif ((native)); then
                    log "Skipping --native for $target on $arch (built in the container)"
                fi
                if [[ "$target" == appimage ]] && ((keep_appdir)); then
                    args+=(--keep-appdir)
                fi
                ;;
            rpm)
                if [[ -n "${portable_binary[$arch]:-}" ]]; then
                    args+=(--binary "${portable_binary[$arch]}")
                fi
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
            case "$target" in
                tarball)
                    # A native tarball uses the host Qt and cannot feed the
                    # container-packaged artifacts.
                    if ((!native)) || [[ "$arch" != "$host_arch" ]]; then
                        if [[ -x "$root/build-release-$arch/sticky-notes" ]]; then
                            portable_binary[$arch]="$root/build-release-$arch/sticky-notes"
                        fi
                    fi
                    ;;
                rpm)
                    if [[ -x "$root/build-release-$arch/sticky-notes" ]]; then
                        portable_binary[$arch]="$root/build-release-$arch/sticky-notes"
                    fi
                    ;;
            esac
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
    mapfile -t arch_targets < <(targets_for_arch "$arch")
    ((${#arch_targets[@]})) || continue
    for target in "${arch_targets[@]}"; do
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
    log "Keeping the build trees: fix the error and rerun to reuse them"
    exit 1
fi

clean_build_trees

printf '\n'
log "Every requested artifact is ready: scripts/create_release.sh uploads what is in release/"
