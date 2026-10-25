#!/usr/bin/env bash
# Install Sticky Notes per-user into ~/.local (prefix overridable with PREFIX).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

BIN="$here/build/sticky-notes"
if [[ ! -x "$BIN" ]]; then
    echo "Binary not found: $BIN" >&2
    echo "Build it first with: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/applications"
ICON_DIR="$PREFIX/share/icons/hicolor/256x256/apps"
mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR"

install -m755 "$BIN" "$BIN_DIR/sticky-notes"
install -m644 "$here/resources/icons/sticky-notes.png" "$ICON_DIR/sticky-notes.png"

# .desktop with an absolute Exec (works even if ~/.local/bin is not in PATH)
sed "s|^Exec=.*|Exec=$BIN_DIR/sticky-notes|" \
    "$here/scripts/sticky-notes.desktop" > "$APP_DIR/sticky-notes.desktop"
sed -i "s|^Icon=.*|Icon=$ICON_DIR/sticky-notes.png|" "$APP_DIR/sticky-notes.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APP_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true

echo "Installed into $PREFIX"
echo "  binary  : $BIN_DIR/sticky-notes"
echo "  desktop : $APP_DIR/sticky-notes.desktop"
echo "  icon    : $ICON_DIR/sticky-notes.png"
