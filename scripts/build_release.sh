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
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

skip_build=0
clean_only=0

usage() {
    cat <<'EOF'
Usage: scripts/build_release.sh [options]

Options:
  --skip-build   reuse the existing build/sticky-notes (do not run CMake)
  --clean        remove release/, then exit
  -h, --help     show this help
EOF
}

while (($#)); do
    case "$1" in
        --skip-build) skip_build=1 ;;
        --clean)      clean_only=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

release_dir="$root/release"

if ((clean_only)); then
    rm -rf "$release_dir"
    echo "Removed $release_dir"
    exit 0
fi

log() { printf '[build_release] %s\n' "$*"; }

# --- inputs ----------------------------------------------------------------

binary="$root/build/sticky-notes"
desktop="$root/packaging/sticky-notes.desktop"
icon="$root/resources/icons/sticky-notes.png"
installer="$root/packaging/install-release.sh"

for f in "$desktop" "$icon" "$installer"; do
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
- Qt6 runtime libraries: Core, Gui, Widgets, Sql, Network.
- **LayerShellQt** (\`libLayerShellQtInterface.so\`).

To check the missing libraries before launching:

\`\`\`bash
ldd ./sticky-notes | grep 'not found'
\`\`\`

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
  and LayerShellQt) is published separately.

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
