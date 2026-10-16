#!/usr/bin/env bash
# Build a Sticky Notes AppImage.
#
# Produces sticky-notes-<version>-<arch>.AppImage in this directory (scripts/).
# It follows docs/APPIMAGE.md: the Qt Wayland platform plugin and the LayerShellQt
# shell integration are forced in (the app only runs on KDE Plasma / wlroots
# Wayland), and the KDE Plasma platform theme and Breeze style are deployed so the
# app does not fall back to a light Fusion look.
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
ln -sf "$(basename "$qt_plugin")" "$tools_dir/linuxdeploy-plugin-qt.AppImage"
ln -sf "$(basename "$linuxdeploy")" "$tools_dir/linuxdeploy.AppImage"
ln -sf "$(basename "$qt_plugin")" "$tools_dir/linuxdeploy-plugin-qt"

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

# --- 5. Package ------------------------------------------------------------

output="$here/sticky-notes-$version-$arch.AppImage"
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
