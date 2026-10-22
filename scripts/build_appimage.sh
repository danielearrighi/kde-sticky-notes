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
# this script runs on a rolling release (glibc 2.44 on Tumbleweed), while the
# AppImageHub test runs on the oldest Ubuntu LTS still in standard support
# (glibc 2.35), and an AppImage must run on both.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

skip_build=0
keep_appdir=0
clean_only=0

usage() {
    cat <<'EOF'
Usage: scripts/build_appimage.sh [options]

Options:
  --skip-build    reuse the existing build/sticky-notes (do not run CMake)
  --keep-appdir   keep the temporary AppDir used to assemble the AppImage
  --clean         remove downloaded tools/ and AppDir/, then exit
  -h, --help      show this help
EOF
}

while (($#)); do
    case "$1" in
        --skip-build)  skip_build=1 ;;
        --keep-appdir) keep_appdir=1 ;;
        --clean)       clean_only=1 ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

tools_dir="$here/tools"
appdir="$here/AppDir"

if ((clean_only)); then
    rm -rf "$tools_dir" "$appdir"
    echo "Removed $tools_dir and $appdir"
    exit 0
fi

log() { printf '[build_appimage] %s\n' "$*"; }

# --- inputs ----------------------------------------------------------------

binary="$root/build/sticky-notes"
desktop="$root/packaging/sticky-notes.desktop"
icon="$root/resources/icons/sticky-notes.png"

for f in "$desktop" "$icon"; do
    [[ -f "$f" ]] || { echo "Missing required file: $f" >&2; exit 1; }
done

arch="$(uname -m)"
case "$arch" in
    x86_64|aarch64) ;;
    *) echo "Unsupported architecture: $arch (need x86_64 or aarch64)" >&2; exit 1 ;;
esac

# --- 1. Release binary -----------------------------------------------------

if ((skip_build)); then
    [[ -x "$binary" ]] || { echo "Binary not found: $binary (drop --skip-build)" >&2; exit 1; }
    log "Reusing $binary"
else
    log "Configuring and building (Release)"
    cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$root/build"
fi

# Version is read from the binary; fall back to CMakeLists.txt because
# `--version` still constructs a QApplication and thus needs a working display.
version="$("$binary" --version 2>/dev/null | awk '{print $2}' || true)"
if [[ -z "$version" ]]; then
    version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
fi
[[ -n "$version" ]] || { echo "Could not determine the version" >&2; exit 1; }
log "Version: $version"

# --- 2. Tools (linuxdeploy + Qt plugin, appimagetool, runtime) -------------

appimage_tool="$tools_dir/appimagetool-$arch.AppImage"
linuxdeploy="$tools_dir/linuxdeploy-$arch.AppImage"
qt_plugin="$tools_dir/linuxdeploy-plugin-qt-$arch.AppImage"
runtime="$tools_dir/runtime-$arch"

fetch() { # url destination
    local url="$1" dest="$2"
    if [[ -s "$dest" ]]; then
        log "Already present: $(basename "$dest")"
        return
    fi
    log "Downloading $(basename "$dest")"
    curl -fSL --retry 3 -o "$dest" "$url" \
        || { echo "Download failed: $url" >&2; rm -f "$dest"; exit 1; }
}

mkdir -p "$tools_dir"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage" "$linuxdeploy"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$arch.AppImage" "$qt_plugin"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage" "$appimage_tool"
fetch "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$arch" "$runtime"
chmod +x "$linuxdeploy" "$qt_plugin" "$appimage_tool" "$runtime"

# linuxdeploy resolves --plugin qt by looking for `linuxdeploy-plugin-qt` next to
# itself (also with an .AppImage suffix), so expose the downloaded file once.
# Use hard links, not symlinks: this directory lives under a Synology Drive sync
# folder, and the client cannot sync symbolic links (it retries forever with
# "Server response unexpected error 'System error'").
ln -f "$qt_plugin" "$tools_dir/linuxdeploy-plugin-qt.AppImage"
ln -f "$linuxdeploy" "$tools_dir/linuxdeploy.AppImage"
ln -f "$qt_plugin" "$tools_dir/linuxdeploy-plugin-qt"

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
[[ -n "$QMAKE" ]] || { echo "qmake6 not found; install qt6-base-devel or set QMAKE" >&2; exit 1; }

export ARCH="$arch"
export QMAKE
# Force the Wayland platform plugin (and with it liblayer-shell.so): the Qt
# plugin defaults to the X11 plugin only, which would make the app unlaunchable.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
# Pull in KDEPlasmaPlatformTheme6.so + breeze6.so, otherwise the AppImage looks
# light/Fusion even on a dark Plasma desktop.
export DEPLOY_PLATFORM_THEMES=1
# Without FUSE the AppImage tools cannot self-mount; extract and run instead.
if [[ ! -e /dev/fuse ]] && ! command -v fusermount >/dev/null 2>&1 \
        && ! command -v fusermount3 >/dev/null 2>&1; then
    log "FUSE not detected, using APPIMAGE_EXTRACT_AND_RUN=1"
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

log "Running linuxdeploy (this scans and copies the Qt/KDE dependencies)"
"$linuxdeploy" \
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
ARCH="$arch" "$appimage_tool" --runtime-file "$runtime" "$appdir" "$output"
chmod +x "$output"

if ((!keep_appdir)); then
    rm -rf "$appdir"
fi

size="$(du -h "$output" | cut -f1)"
log "Done: $output ($size)"
log "Verify with: \"$output\" --version"
log "Full test on KDE Plasma Wayland: QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \"$output\""
