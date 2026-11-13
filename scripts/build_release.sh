#!/usr/bin/env bash
# Build a Sticky Notes release tarball (the "recommended" release).
#
# Produces release/sticky-notes-<version>-<arch>.tar.gz containing the
# executable, the .desktop entry, the application icon, an install.sh for end
# users and a README.md with the runtime requirements. The intermediate folder
# is removed once the archive is created. Unlike the AppImage
# (scripts/build_appimage.sh) this bundle does NOT ship the Qt6/LayerShellQt
# libraries: the target machine must already have them, so it only runs on a
# KDE Plasma (Wayland) that also has the runtime packages.
#
# By default the binary is built by scripts/build_portable.sh inside a
# debian:13 container (Qt 6.8, LayerShellQt 6.3.4), the oldest baseline the app
# supports and the one every release artifact shares. That is not optional
# pedantry: Qt is only forward compatible, and a binary built on the rolling
# host (Qt 6.12) fails on Fedora (Qt 6.11) with "version `Qt_6.12' not found".
# Use --native only for a local test build.
#
# --arch selects the target architecture (default: the host one). A foreign
# architecture goes through scripts/build_portable.sh, which emulates it with
# QEMU; the archive is then named sticky-notes-<version>-<arch>.tar.gz.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

# shellcheck source=lib/arch.sh
source "$here/lib/arch.sh"

arch=""
skip_build=0
clean_only=0
native_build=0
binary_override=""

usage() {
    cat <<'EOF'
Usage: scripts/build_release.sh [options]

Options:
  --arch ARCH    target architecture: x86_64 or aarch64 (default: uname -m)
  --binary PATH  reuse an existing binary (implies --skip-build)
  --native       build on the host (build/sticky-notes) instead of the
                 container; only for the host architecture, and the resulting
                 tarball may require a newer Qt
  --skip-build   do not build, reuse the selected binary
  --clean        remove this architecture's tarballs from release/, then exit
  -h, --help     show this help
EOF
}

while (($#)); do
    case "$1" in
        --arch)       arch="${2:?--arch needs an architecture}"; shift ;;
        --binary)     binary_override="${2:?--binary needs a path}"; shift ;;
        --native)     native_build=1 ;;
        --skip-build) skip_build=1 ;;
        --clean)      clean_only=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

release_dir="$root/release"

arch="${arch:-$(sn_host_arch)}"
sn_known_arch "$arch" || { echo "Unsupported architecture: $arch (need x86_64 or aarch64)" >&2; exit 1; }

if ((clean_only)); then
    # Match the other target scripts: remove only this architecture's assets,
    # not the whole release directory (which may contain other targets and
    # versions).
    rm -f "$release_dir"/sticky-notes-*-"$arch".tar.gz
    rm -rf "$release_dir"/sticky-notes-*-"$arch"
    echo "Removed the $arch tarballs from $release_dir"
    exit 0
fi

log() { printf '[build_release] %s\n' "$*"; }

# --- inputs ----------------------------------------------------------------

desktop="$root/scripts/sticky-notes.desktop"
icon="$root/resources/icons/sticky-notes.png"
installer="$root/scripts/install-release.sh"

for f in "$desktop" "$icon" "$installer"; do
    [[ -f "$f" ]] || { echo "Missing required file: $f" >&2; exit 1; }
done

# --- 1. Release binary -----------------------------------------------------

if [[ -n "$binary_override" ]]; then
    binary="$binary_override"
    [[ -x "$binary" ]] || { echo "Binary not found: $binary" >&2; exit 1; }
    log "Using $binary"
elif ((native_build)); then
    sn_is_native "$arch" || {
        echo "--native builds on this host only; use the container for $arch" >&2
        exit 1
    }
    # Host build: convenient for a quick local test, but the tarball inherits
    # the host Qt and may not start on distributions with an older Qt.
    binary="$root/build/sticky-notes"
    if ((skip_build)); then
        [[ -x "$binary" ]] || { echo "Binary not found: $binary (drop --skip-build)" >&2; exit 1; }
        log "Reusing $binary"
    else
        log "Configuring and building (Release, host Qt)"
        cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build "$root/build"
    fi
else
    # Portable build against Qt 6.8 + LayerShellQt 6.3.4 (see build_portable.sh).
    binary="$root/build-release-$arch/sticky-notes"
    portable_args=(--arch "$arch")
    ((skip_build)) && portable_args+=(--skip-build)
    "$root/scripts/build_portable.sh" "${portable_args[@]}"
    log "Using $binary"
fi

# Guard against a --binary (or a leftover build tree) of the other
# architecture: the tarball name would silently lie.
if command -v readelf >/dev/null 2>&1; then
    binary_arch="$(LC_ALL=C readelf -h "$binary" 2>/dev/null | sed -n 's/^ *Machine: *//p' | head -1)"
    case "$arch:$binary_arch" in
        x86_64:*X86-64*|aarch64:*AArch64*) ;;
        *) echo "$binary is not a $arch binary (readelf: ${binary_arch:-unknown})" >&2; exit 1 ;;
    esac
fi

# The version is generated from project(... VERSION) in CMakeLists.txt. Do not
# execute the binary: --version constructs a QApplication, needs a display and
# would abort under QEMU when the binary is foreign.
version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || { echo "Could not determine the version" >&2; exit 1; }
log "Version: $version"
log "Architecture: $arch"

# --- 2. Staging folder -----------------------------------------------------

name="sticky-notes-$version-$arch"
stage="$release_dir/$name"

log "Preparing $stage"
rm -rf "$stage"
mkdir -p "$stage"

install -Dm755 "$binary"  "$stage/sticky-notes"
install -Dm644 "$desktop" "$stage/sticky-notes.desktop"
install -Dm644 "$icon"    "$stage/sticky-notes.png"
install -Dm755 "$installer" "$stage/install.sh"

# --- 3. README for the end user --------------------------------------------

cat > "$stage/README.md" <<EOF
# Sticky Notes $version

Desktop sticky notes for **KDE Plasma (Wayland)** — release $version ($arch).

## Contents

| File                  | Purpose                                                        |
| --------------------- | -------------------------------------------------------------- |
| \`install.sh\`          | Checks the dependencies, installs them if needed, installs the app. |
| \`sticky-notes\`        | The executable. Translations and icons are compiled into it.   |
| \`sticky-notes.desktop\`| Menu entry / launcher (uses \`Exec=sticky-notes\`).             |
| \`sticky-notes.png\`    | Application icon (hicolor, 256x256).                           |
| \`README.md\`           | This file.                                                     |

## Requirements

This bundle does **not** include the libraries: they must already be installed
on the target machine.

- A **Wayland** session with layer-shell support: KWin since **Plasma 6.6**, or
  a wlroots compositor. GNOME (Mutter) and X11 sessions do **not** implement the
  protocol and cannot run the app.
- Qt6 runtime libraries: Core, Gui, Widgets, Sql, Network — **6.8 or newer**.
- **LayerShellQt** (\`libLayerShellQtInterface.so\`) — **6.3.4 or newer**.

The binary is built against Qt 6.8 and LayerShellQt 6.3.4; newer ones work too.
On a distribution with an older Qt (e.g. an older Debian or Ubuntu) use the
self-contained **AppImage** or the **Flatpak**, which bundle their own Qt.

To check the missing libraries before launching:

\`\`\`bash
ldd ./sticky-notes | grep 'not found'
\`\`\`

A line like \`version 'Qt_6.12' not found\` means your Qt is older than the one
this bundle was built against: install the AppImage or the Flatpak instead.

## Run

From this folder:

\`\`\`bash
./sticky-notes
\`\`\`

To force the Wayland QPA and see the logs (Qt suppresses them when stderr is not
a terminal):

\`\`\`bash
QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 ./sticky-notes
\`\`\`

## Install into the user session

The easiest way is the bundled installer. It checks the runtime libraries,
offers to install the missing ones with your package manager (openSUSE,
Fedora, Debian/Ubuntu, Arch) and then copies the app into \`~/.local\`, so it also
appears in the KDE menu and the taskbar:

\`\`\`bash
./install.sh
\`\`\`

Options: \`--prefix DIR\` (default \`~/.local\`), \`--no-deps\` (skip the library
check), \`-y\` (non-interactive). Run \`./install.sh --help\` for details.

To do it by hand instead, copy the three files into \`~/.local\`. The
\`.desktop\` entry and the icon are looked up by file/name, so keep the names as
they are.

\`\`\`bash
install -Dm755 sticky-notes ~/.local/bin/sticky-notes
install -Dm644 sticky-notes.png ~/.local/share/icons/hicolor/256x256/apps/sticky-notes.png
sed "s|^Exec=.*|Exec=\$HOME/.local/bin/sticky-notes|" sticky-notes.desktop \\
    > ~/.local/share/applications/sticky-notes.desktop
\`\`\`

If you prefer, put \`sticky-notes\` in a directory already in your \`PATH\` and
leave \`Exec=sticky-notes\` unchanged.

To uninstall:

\`\`\`bash
rm -f ~/.local/bin/sticky-notes \\
      ~/.local/share/applications/sticky-notes.desktop \\
      ~/.local/share/icons/hicolor/256x256/apps/sticky-notes.png
\`\`\`

## Data

Notes are stored in \`~/.local/share/StickyNotes/notes.db\` (SQLite). Delete that
file to start from scratch.

## Notes

- The app is single-instance: launching it again just brings the hub to the
  front.
- To uninstall a previous copy before installing this version, see above.
- This is the plain binary release; the self-contained **AppImage** (bundles Qt6
  and LayerShellQt) and the **Flatpak** are published separately.

## License

Sticky Notes is licensed under the **GNU General Public License v3.0 or later**
(GPL-3.0-or-later). Runtime dependencies: Qt6 (LGPLv3 / GPL), LayerShellQt
(LGPL-2.1-or-later), SQLite (public domain).
EOF

# --- 4. Tarball ------------------------------------------------------------

tarball="$release_dir/$name.tar.gz"
log "Creating $tarball"
rm -f "$tarball"
tar -C "$release_dir" -czf "$tarball" "$name"

# The per-file folder was only a staging area: drop it so release/ holds the
# archive alone.
rm -rf "$stage"

# --- 5. Summary ------------------------------------------------------------

log "Done: $tarball ($(du -h "$tarball" | cut -f1))"
