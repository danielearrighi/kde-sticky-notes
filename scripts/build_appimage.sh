#!/usr/bin/env bash
# Build a Sticky Notes AppImage.
#
# Produces release/sticky-notes-<version>-<arch>.AppImage (same folder as the
# plain-binary release from scripts/build_release.sh).
# It follows docs/APPIMAGE.md: the Qt Wayland platform plugin and the LayerShellQt
# shell integration are forced in (the app only runs on KDE Plasma / wlroots
# Wayland), and the KDE Plasma platform theme and Breeze style are deployed so the
# app does not fall back to a light Fusion look.
#
# The C library, its loader and libstdc++ are bundled (X-AppImage-Libc=bundled):
# the AppImage must run on the oldest Ubuntu LTS still in standard support
# (glibc 2.35) as well as on current distributions, so its payload cannot rely
# on the build host's C library.
#
# --arch selects the target architecture (default: the host one). Both
# architectures are built by default inside the Ubuntu 26.04 build container
# (scripts/container/ubuntu26.04.Dockerfile), against the same Qt 6.10/glibc
# baseline: linuxdeploy, the Qt plugin and appimagetool only exist as binaries
# for their own architecture, and they must run next to the Qt they deploy.
# A foreign architecture additionally needs QEMU user emulation; its AppImage
# tools are executed through the host's static qemu interpreter because the
# type-2 header does not match binfmt_misc's ELF magic. --native is a host-only
# escape hatch for local testing and may require a newer Qt/glibc.
set -euo pipefail

# linuxdeploy may run the application while scanning it; without a display the
# app aborts, and a foreign binary would leave a core dump in the working tree.
# Keep build runs clean: the build does not need those cores.
ulimit -c 0 2>/dev/null || true

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

dockerfile="$here/container/ubuntu26.04.Dockerfile"
image_base="${STICKY_NOTES_BUILD_IMAGE:-localhost/sticky-notes-build:ubuntu26.04}"

arch=""
engine=""
binary_override=""
native_build=0
skip_build=0
keep_appdir=0
clean_only=0
rebuild_image=0

usage() {
    cat <<'EOF'
Usage: scripts/build_appimage.sh [options]

Options:
  --arch ARCH      target architecture: x86_64 or aarch64 (default: uname -m).
                   Both architectures default to the Ubuntu 26.04 container; a
                   foreign one additionally needs QEMU user mode (binfmt_misc)
  --engine NAME    container engine used for the container build
                   (podman or docker; default: auto)
  --image TAG      base build image tag (default: localhost/sticky-notes-build:ubuntu26.04;
                   the architecture is appended, so each arch has its own image;
                   an unqualified name is treated as local)
  --rebuild-image  rebuild the container image from scratch (--no-cache)
  --binary PATH    reuse an existing binary instead of compiling (path relative
                   to the repository root, or absolute inside it; implies
                   --skip-build)
  --native         build on the host with the host Qt instead of the container
                   (host architecture only; local testing, the AppImage may
                   require a newer Qt/glibc)
  --skip-build     reuse the existing build-appimage-<arch>/sticky-notes
                   (with --native: build-appimage-native-<arch>/sticky-notes)
  --keep-appdir    keep the temporary AppDir used to assemble the AppImage
  --clean          remove tools/, AppDir/, this architecture's build trees and
                   AppImages, then exit
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
        --binary)        binary_override="${2:?--binary needs a path}"; shift ;;
        --rebuild-image) rebuild_image=1 ;;
        --native)        native_build=1 ;;
        --skip-build)    skip_build=1 ;;
        --keep-appdir)   keep_appdir=1 ;;
        --clean)         clean_only=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

tools_dir="$here/tools"
appdir="$here/AppDir"

log() { printf '[build_appimage] %s\n' "$*"; }
die() { printf 'build_appimage: %s\n' "$*" >&2; exit 1; }

# In an emulated container the AppImage tools must go through the mounted qemu
# interpreter instead of exec'ing them directly (see the container branch).
run_tool() {
    if [[ -n "${STICKY_NOTES_QEMU:-}" ]]; then
        "$STICKY_NOTES_QEMU" "$@"
    else
        "$@"
    fi
}

# --binary must be visible inside the container bind mount too: normalize it to
# an absolute path under the repository and keep a relative form for the
# re-executed script inside /work.
if [[ -n "$binary_override" ]]; then
    [[ "$binary_override" == /* ]] || binary_override="$root/$binary_override"
    binary_abs="$(realpath -m "$binary_override")"
    root_canon="$(realpath -m "$root")"
    case "$binary_abs" in
        "$root_canon"/*) ;;
        *) die "--binary must be inside the repository ($root)" ;;
    esac
    binary_rel="${binary_abs#"$root_canon"/}"
    binary_override="$binary_abs"
    skip_build=1
fi

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || die "Unsupported architecture: $arch (need x86_64 or aarch64)"
platform="$(sn_platform "$arch")"

if ((clean_only)); then
    rm -rf "$tools_dir" "$appdir"
    rm -rf "$root/build-appimage-$arch" "$root/build-appimage-native-$arch"
    rm -f "$root/release"/sticky-notes-*-"$arch".AppImage
    echo "Removed $tools_dir, $appdir, the AppImage build trees and the $arch AppImages"
    exit 0
fi

if ((native_build)) && ! sn_is_native "$arch"; then
    die "--native builds on this host only; use the container for $arch"
fi

# --- container baseline -----------------------------------------------------

if [[ -z "${STICKY_NOTES_IN_CONTAINER:-}" ]] && ((!native_build)); then
    [[ -f "$dockerfile" ]] || die "Missing $dockerfile"

    # Only the foreign architecture needs QEMU; the native one runs natively
    # in its container.
    sn_is_native "$arch" || sn_require_binfmt "$arch"

    if [[ -z "$engine" ]]; then
        engine="$(sn_container_engine || true)"
    fi
    [[ -n "$engine" ]] || die "No container engine found: install podman or docker to build the $arch AppImage"
    command -v "$engine" >/dev/null 2>&1 || die "Container engine not found: $engine"

    platform_args=()
    sn_is_native "$arch" || platform_args=(--platform "$platform")

    image="$(sn_local_image "$image_base-$arch")"
    if ((rebuild_image)); then
        log "Rebuilding $image from scratch"
        "$engine" build --no-cache "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
    elif ! "$engine" image inspect "$image" >/dev/null 2>&1; then
        log "Building $image (first run only)"
        "$engine" build "${platform_args[@]}" -t "$image" -f "$dockerfile" "$here/container"
    else
        log "Reusing image $image"
    fi

    # A foreign AppImage tool cannot be executed through binfmt_misc: the
    # type-2 AppImage header ("AI\x02" at offset 8) does not match the ELF
    # magic the kernel registered for qemu-aarch64. Mount the host's static
    # qemu interpreter and run the AppImage tools through it explicitly.
    qemu_args=()
    if ! sn_is_native "$arch"; then
        qemu_bin="$(command -v "qemu-$arch" || command -v "qemu-$arch-static" || true)"
        [[ -n "$qemu_bin" ]] || die "Missing QEMU user emulation for $arch"
        qemu_args=(-v "$qemu_bin:/run/qemu-target:ro"
                   -e STICKY_NOTES_QEMU=/run/qemu-target)
    fi

    # Run as the invoking user so the build tree is not left owned by root, and
    # force APPIMAGE_EXTRACT_AND_RUN: inside the container the AppImage tools
    # cannot mount themselves.
    run_args=(--rm --pull=never "${platform_args[@]}" "${qemu_args[@]}"
              -v "$root:/work:Z" -w /work
              -e HOME=/tmp -e APPIMAGE_EXTRACT_AND_RUN=1
              -e STICKY_NOTES_IN_CONTAINER=1)
    if [[ "$engine" == "podman" ]]; then
        run_args+=(--userns=keep-id)
    else
        run_args+=(--user "$(id -u):$(id -g)")
    fi

    forward=(--arch "$arch")
    if [[ -n "$binary_override" ]]; then
        forward+=(--binary "$binary_rel")
    elif ((skip_build)); then
        forward+=(--skip-build)
    fi
    ((keep_appdir)) && forward+=(--keep-appdir)

    log "Building the $arch AppImage inside $image"
    "$engine" run "${run_args[@]}" "$image" ./scripts/build_appimage.sh "${forward[@]}"
    exit
fi

# --- inputs ----------------------------------------------------------------

if [[ -n "$binary_override" ]]; then
    binary="$binary_override"
else
    if ((native_build)); then
        build_dir="$root/build-appimage-native-$arch"
    else
        build_dir="$root/build-appimage-$arch"
    fi
    binary="$build_dir/sticky-notes"
fi
desktop="$root/scripts/sticky-notes.desktop"
icon="$root/resources/icons/sticky-notes.png"

for f in "$desktop" "$icon"; do
    [[ -f "$f" ]] || { echo "Missing required file: $f" >&2; exit 1; }
done

# --- 1. Release binary -----------------------------------------------------

if [[ -n "$binary_override" ]]; then
    [[ -x "$binary" ]] || die "Binary not found: $binary"
    sn_verify_binary_arch "$binary" "$arch"
    # The container packages the Ubuntu 26.04 Qt. A binary built against a
    # newer host Qt cannot be mixed with it (Qt is forward compatible only).
    if [[ -n "${STICKY_NOTES_IN_CONTAINER:-}" ]] && ! ((native_build)); then
        required_qt="$(readelf -V "$binary" 2>/dev/null \
            | grep -o 'Qt_6\.[0-9][0-9]*' | sort -Vu | tail -1 || true)"
        if [[ -n "$required_qt" ]] \
                && [[ "$(printf '%s\n' Qt_6.10 "$required_qt" | sort -V | tail -1)" != Qt_6.10 ]]; then
            die "$binary requires $required_qt, newer than the container's Qt 6.10; use --native or build it with build_portable.sh"
        fi
    fi
    log "Using $binary"
elif ((skip_build)); then
    [[ -x "$binary" ]] || { echo "Binary not found: $binary (drop --skip-build)" >&2; exit 1; }
    if [[ -n "${STICKY_NOTES_IN_CONTAINER:-}" && ! -f "$build_dir/.container-build" ]]; then
        die "$binary was not built in the container baseline; drop --skip-build"
    fi
    log "Reusing $binary"
else
    # A build tree left by an older host build uses a different Qt/glibc
    # baseline: discard it once instead of silently mixing the two.
    if [[ -n "${STICKY_NOTES_IN_CONTAINER:-}" && -f "$build_dir/CMakeCache.txt" \
            && ! -f "$build_dir/.container-build" ]]; then
        log "Discarding the previous host build in $build_dir"
        rm -rf "$build_dir"
    fi
    log "Configuring and building (Release)"
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build_dir"
    if [[ -n "${STICKY_NOTES_IN_CONTAINER:-}" ]]; then
        touch "$build_dir/.container-build"
    fi
fi

# The version is generated from project(... VERSION) in CMakeLists.txt. Do not
# execute the binary: --version constructs a QApplication, and a foreign binary
# aborts under QEMU without a display, leaving a core dump and slowing the build.
version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || { echo "Could not determine the version" >&2; exit 1; }
log "Version: $version ($arch)"

# --- 2. Tools (linuxdeploy + Qt plugin, appimagetool, runtime) -------------

appimage_tool="$tools_dir/appimagetool-$arch.AppImage"
linuxdeploy="$tools_dir/linuxdeploy-$arch.AppImage"
runtime="$tools_dir/runtime-$arch"

# Keep the real plugin AppImage under a name that linuxdeploy's
# `linuxdeploy-plugin-qt*` search cannot pick up, and expose only the current
# architecture under the names it searches. Mixing the x86_64 and aarch64
# plugins in the same tools directory otherwise makes linuxdeploy run the wrong
# one.
qt_plugin_real="$tools_dir/qt-plugin-$arch.AppImage"
rm -f "$tools_dir/linuxdeploy-plugin-qt-$arch.AppImage.real"

fetch() { # url destination
    local url="$1" dest="$2" tmp
    if [[ -s "$dest" ]]; then
        log "Already present: $(basename "$dest")"
        return
    fi
    log "Downloading $(basename "$dest")"
    # Download into the container's /tmp and install the file afterwards.
    # Inside the aarch64 container curl cannot open a new file on the /work
    # bind mount (it fails with EACCES), while shell redirection and install
    # work; writing to /tmp avoids the problem for both architectures.
    tmp="$(mktemp /tmp/sticky-notes-download.XXXXXX)" \
        || { echo "Could not create a temporary download file" >&2; exit 1; }
    if ! curl -fSL --retry 3 -o "$tmp" "$url"; then
        echo "Download failed: $url" >&2
        rm -f "$tmp" "$dest"
        exit 1
    fi
    install -m755 "$tmp" "$dest" \
        || { echo "Could not install $dest" >&2; rm -f "$tmp" "$dest"; exit 1; }
    rm -f "$tmp"
}

mkdir -p "$tools_dir"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage" "$linuxdeploy"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$arch.AppImage" "$qt_plugin_real"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage" "$appimage_tool"
fetch "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$arch" "$runtime"
chmod +x "$linuxdeploy" "$qt_plugin_real" "$appimage_tool" "$runtime"

# linuxdeploy resolves --plugin qt by looking for `linuxdeploy-plugin-qt` next to
# itself (also with an .AppImage suffix), so expose the downloaded file once.
# Remove any exposed names left by the other architecture first.
rm -f "$tools_dir"/linuxdeploy-plugin-qt \
      "$tools_dir"/linuxdeploy-plugin-qt.AppImage \
      "$tools_dir"/linuxdeploy-plugin-qt-*

# In an emulated container linuxdeploy execs the plugin itself; a type-2 AppImage
# does not match the qemu binfmt magic, so the exposed names must be wrappers
# that go through the mounted qemu interpreter. Otherwise use hard links: this
# directory lives under a Synology Drive sync folder, and the client cannot sync
# symbolic links (it retries forever with "Server response unexpected error
# 'System error'").
if [[ -n "${STICKY_NOTES_QEMU:-}" ]]; then
    for name in linuxdeploy-plugin-qt \
                linuxdeploy-plugin-qt.AppImage \
                "linuxdeploy-plugin-qt-$arch.AppImage" \
                "linuxdeploy-plugin-qt-$arch"; do
        cat > "$tools_dir/$name" <<EOF
#!/bin/sh
exec "$STICKY_NOTES_QEMU" "$qt_plugin_real" "\$@"
EOF
        chmod +x "$tools_dir/$name"
    done
else
    for name in linuxdeploy-plugin-qt \
                linuxdeploy-plugin-qt.AppImage \
                "linuxdeploy-plugin-qt-$arch.AppImage" \
                "linuxdeploy-plugin-qt-$arch"; do
        ln -f "$qt_plugin_real" "$tools_dir/$name"
    done
fi
ln -f "$linuxdeploy" "$tools_dir/linuxdeploy.AppImage"

# --- 3. AppDir -------------------------------------------------------------

log "Preparing AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" \
         "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/256x256/apps"

install -Dm755 "$binary"  "$appdir/usr/bin/sticky-notes"
cp "$desktop" "$appdir/sticky-notes.desktop"
cp "$icon"    "$appdir/sticky-notes.png"
cp "$desktop" "$appdir/usr/share/applications/"
cp "$icon"    "$appdir/usr/share/icons/hicolor/256x256/apps/"

# --- 4. Dependencies (Qt, Wayland, LayerShellQt, Plasma theme) -------------

if [[ -z "${QMAKE:-}" ]]; then
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
fi
# Ubuntu and other distributions keep qmake6 out of PATH when only the -dev
# packages are installed (the container build hits this).
if [[ -z "$QMAKE" ]]; then
    for candidate in /usr/lib/qt6/bin/qmake6 /usr/lib64/qt6/bin/qmake6 \
                     /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake; do
        [[ -x "$candidate" ]] && { QMAKE="$candidate"; break; }
    done
fi
[[ -n "$QMAKE" ]] || { echo "qmake6 not found; install qt6-base-devel or set QMAKE" >&2; exit 1; }

export ARCH="$arch"
export QMAKE
# Force the Wayland platform plugin (and with it liblayer-shell.so): the Qt
# plugin defaults to the X11 plugin only, which would make the app unlaunchable.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
# Pull in KDEPlasmaPlatformTheme6.so + breeze6.so, otherwise the AppImage looks
# light/Fusion even on a dark Plasma desktop. The aarch64 build container may
# not have the theme (it is optional there), so only enable the deployment when
# the plugin is actually present.
if find /usr -name KDEPlasmaPlatformTheme6.so -print -quit 2>/dev/null | grep -q .; then
    export DEPLOY_PLATFORM_THEMES=1
else
    log "No KDE platform theme plugin found: the AppImage will use the Qt default style"
fi
# Without FUSE the AppImage tools cannot self-mount; extract and run instead.
if [[ ! -e /dev/fuse ]] && ! command -v fusermount >/dev/null 2>&1 \
        && ! command -v fusermount3 >/dev/null 2>&1; then
    log "FUSE not detected, using APPIMAGE_EXTRACT_AND_RUN=1"
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

log "Running linuxdeploy (this scans and copies the Qt/KDE dependencies)"
run_tool "$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/sticky-notes" \
    --desktop-file "$appdir/sticky-notes.desktop" \
    --icon-file "$appdir/sticky-notes.png" \
    --plugin qt

# Optional trim (see docs/APPIMAGE.md): the GTK3 theme is useless on KDE.
rm -f "$appdir/usr/plugins/platformthemes/libqgtk3.so"

# --- 4b. C library, loader and C++ runtime ---------------------------------
# The build host decides which glibc the payload needs, and it is the newest
# (Tumbleweed). Bundling libc together with its loader makes the AppImage
# independent of the host's C library: AppImageHub then reports
# X-AppImage-Libc=bundled instead of rejecting it as "glibc too new".
# libstdc++ and libgcc_s must go with it because this GCC is newer than the
# target's (libicu needs CXXABI_1.3.15), and linuxdeploy never bundles them.

log "Bundling glibc, the dynamic loader and libstdc++"
glibc_dir="$(dirname "$(readlink -f "$(ldd "$binary" | awk '/libc\.so\.6/ { print $3; exit }')")")"
loader="$(ldd "$binary" | awk '/ld-linux/ { print $1; exit }')"
[[ -d "$glibc_dir" ]] || { echo "Could not locate the C library" >&2; exit 1; }
[[ -n "$loader" && -e "$loader" ]] || { echo "Could not locate the dynamic loader" >&2; exit 1; }

for lib in libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 \
           librt.so.1 libutil.so.1 libanl.so.1 libresolv.so.2 libnsl.so.1; do
    [[ -e "$glibc_dir/$lib" ]] && cp -Lf "$glibc_dir/$lib" "$appdir/usr/lib/"
done
# Name resolution: ld.so looks these up by soname through --library-path.
for lib in "$glibc_dir"/libnss_*.so.*; do
    [[ -e "$lib" ]] || continue
    cp -Lf "$lib" "$appdir/usr/lib/"
done
for lib in libstdc++.so.6 libgcc_s.so.1; do
    path="$(ldd "$binary" | awk -v want="$lib" '$1 == want { print $3; exit }')"
    [[ -n "$path" ]] && cp -Lf "$path" "$appdir/usr/lib/"
done
cp -Lf "$loader" "$appdir/usr/lib/$(basename "$loader")"

# linuxdeploy's AppRun is a symlink to the binary. Replace it with a script that
# starts the bundled loader, otherwise the loader of the host would be used and
# the bundled C library ignored.
rm -f "$appdir/AppRun"
cat > "$appdir/AppRun" <<EOF
#!/bin/sh
here="\${APPDIR:-\$(dirname "\$(readlink -f "\$0")")}"
exec "\$here/usr/lib/$(basename "$loader")" \\
     --library-path "\$here/usr/lib" \\
     "\$here/usr/bin/sticky-notes" "\$@"
EOF
chmod +x "$appdir/AppRun"

# --- 5. Package ------------------------------------------------------------

output="$root/release/sticky-notes-$version-$arch.AppImage"
mkdir -p "$root/release"
rm -f "$output"
log "Creating $output"
ARCH="$arch" run_tool "$appimage_tool" --runtime-file "$runtime" "$appdir" "$output"
chmod +x "$output"

if ((!keep_appdir)); then
    rm -rf "$appdir"
fi

size="$(du -h "$output" | cut -f1)"
log "Done: $output ($size)"
log "Verify with: \"$output\" --version"
log "Full test on KDE Plasma Wayland: QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \"$output\""
