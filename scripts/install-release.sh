#!/usr/bin/env bash
# Sticky Notes release installer.
#
# This is the user-facing installer shipped inside the release folder (as
# install.sh). It checks the runtime dependencies, offers to install the missing
# ones with the system package manager, then installs the app into the user's
# ~/.local (binary, menu entry and icon), like scripts/install.sh does from the
# source tree.
#
# It expects to sit next to: sticky-notes, sticky-notes.desktop, sticky-notes.png
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-$HOME/.local}"
install_deps=1
assume_yes=0

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Options:
  --prefix DIR    install into DIR (default: ~/.local, or $PREFIX)
  --no-deps       do not check/install the runtime libraries
  -y, --yes       assume "yes" (non-interactive)
  -h, --help      show this help
EOF
}

while (($#)); do
    case "$1" in
        --prefix)   PREFIX="${2:?--prefix needs a directory}"; shift ;;
        --prefix=*) PREFIX="${1#*=}" ;;
        --no-deps)  install_deps=0 ;;
        -y|--yes)   assume_yes=1 ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

# Resolve to an absolute path so the generated .desktop Exec stays valid.
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"

log()  { printf '[install] %s\n' "$*"; }
warn() { printf '[install] warning: %s\n' "$*" >&2; }

confirm() { # "question" -> 0 = yes
    ((assume_yes)) && return 0
    local reply
    if ! read -r -p "$1 [Y/n] " reply; then
        return 1
    fi
    [[ -z "$reply" || "$reply" =~ ^[Yy] ]]
}

run_root() { # run a command as root
    if [[ "$EUID" -eq 0 ]]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        warn "need root (or sudo) to install packages"
        return 1
    fi
}

# --- 1. Sanity checks ------------------------------------------------------

binary="$here/sticky-notes"
desktop="$here/sticky-notes.desktop"
icon="$here/sticky-notes.png"

for f in "$binary" "$desktop" "$icon"; do
    [[ -f "$f" ]] || { echo "Missing $f (run ./install.sh from the release folder)" >&2; exit 1; }
done
[[ -x "$binary" ]] || { echo "$binary is not executable (chmod +x sticky-notes)" >&2; exit 1; }

if [[ "${XDG_SESSION_TYPE:-}" != wayland ]]; then
    warn "this is not a Wayland session: Sticky Notes cannot run until you log into a KDE Plasma (Wayland) session"
elif [[ "${XDG_CURRENT_DESKTOP:-}" != *KDE* ]]; then
    warn "the current desktop does not look like KDE: Sticky Notes needs KWin (Plasma 6.6+) with layer-shell"
fi
if [[ "$EUID" -eq 0 && -n "${SUDO_USER:-}" ]]; then
    warn "running with sudo: the app will be installed into $PREFIX (root's home)."
    warn "for a user install, exit sudo and run ./install.sh again."
fi

# --- 2. Runtime dependencies -----------------------------------------------

# Paths where the Qt Wayland platform plugin may live (loaded at runtime, so it
# does not show up in the ldd output).
plugin_dirs=()
if command -v qtpaths6 >/dev/null 2>&1; then
    plugin_dirs+=("$(qtpaths6 --plugin-dir)")
elif command -v qtpaths >/dev/null 2>&1; then
    plugin_dirs+=("$(qtpaths --plugin-dir)")
fi
plugin_dirs+=(/usr/lib64/qt6/plugins /usr/lib/qt6/plugins
              /usr/lib/x86_64-linux-gnu/qt6/plugins /usr/lib/aarch64-linux-gnu/qt6/plugins)

missing_libs=()
missing_plugin=0

check_deps() {
    missing_libs=()
    local line
    while read -r line; do
        [[ -n "$line" ]] && missing_libs+=("$line")
    done < <(ldd "$binary" 2>/dev/null | awk '/not found/ {print $1}')

    missing_plugin=0
    if [[ "${#missing_libs[@]}" -gt 0 || "${install_deps}" -eq 1 ]]; then
        local dir found=0
        for dir in "${plugin_dirs[@]}"; do
            if [[ -e "$dir/platforms/libqwayland.so" ]]; then
                found=1
                break
            fi
        done
        ((found)) || missing_plugin=1
    fi
    return 0
}

print_missing() {
    local lib
    for lib in "${missing_libs[@]}"; do
        warn "missing library: $lib"
    done
    ((missing_plugin)) && warn "missing Qt Wayland platform plugin: libqwayland.so"
    return 0
}

# Package manager family (from /etc/os-release) and the packages that provide
# Qt6 (Core/Gui/Widgets/Sql/Network + wayland plugin) and LayerShellQt.
family="unknown"
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case " ${ID:-} ${ID_LIKE:-} " in
        *" suse "*|*" opensuse "*)              family="suse" ;;
        *" fedora "*|*" rhel "*|*" centos "*)   family="fedora" ;;
        *" debian "*|*" ubuntu "*)              family="debian" ;;
        *" arch "*)                             family="arch" ;;
    esac
fi

pkgs=()
case "$family" in
    suse)   pkgs=(libQt6Core6 libQt6Gui6 libQt6Widgets6 libQt6Sql6 libQt6Network6 libQt6WaylandClient6 libLayerShellQtInterface6) ;;
    fedora) pkgs=(qt6-qtbase qt6-qtwayland layer-shell-qt) ;;
    debian) pkgs=(libqt6core6 libqt6gui6 libqt6widgets6 libqt6sql6 libqt6network6 qt6-wayland liblayershellqtinterface6) ;;
    arch)   pkgs=(qt6-base qt6-wayland layer-shell-qt) ;;
esac

pkg_available() { # package name -> 0 if the repo knows it
    case "$family" in
        suse)   zypper --no-refresh info "$1" >/dev/null 2>&1 ;;
        fedora) dnf -q info "$1" >/dev/null 2>&1 ;;
        debian) apt-cache show "$1" >/dev/null 2>&1 ;;
        arch)   pacman -Si "$1" >/dev/null 2>&1 ;;
        *)      return 1 ;;
    esac
}

pkg_installed() {
    case "$family" in
        suse|fedora) rpm -q "$1" >/dev/null 2>&1 ;;
        debian)      dpkg -s "$1" >/dev/null 2>&1 ;;
        arch)        pacman -Qi "$1" >/dev/null 2>&1 ;;
        *)           return 1 ;;
    esac
}

install_packages() { # package...
    case "$family" in
        suse)   run_root zypper --non-interactive install "$@" ;;
        fedora) run_root dnf install -y "$@" ;;
        debian) run_root apt-get install -y "$@" ;;
        arch)   run_root pacman -S --noconfirm "$@" ;;
        *)      return 1 ;;
    esac
}

manual_hint() {
    warn "install them manually, for example:"
    case "$family" in
        suse)   warn "  sudo zypper install ${pkgs[*]}" ;;
        fedora) warn "  sudo dnf install ${pkgs[*]}" ;;
        debian) warn "  sudo apt install ${pkgs[*]}" ;;
        arch)   warn "  sudo pacman -S ${pkgs[*]}" ;;
        *)      warn "  packages needed: Qt6 (Core, Gui, Widgets, Sql, Network), the Qt Wayland plugin and LayerShellQt" ;;
    esac
}

if ((install_deps)); then
    log "Checking runtime dependencies"
    check_deps
    if [[ "${#missing_libs[@]}" -eq 0 && "${missing_plugin}" -eq 0 ]]; then
        log "All required libraries are present"
    else
        print_missing
        if [[ "$family" == "unknown" || "${#pkgs[@]}" -eq 0 ]]; then
            warn "unknown distribution: cannot detect the packages automatically"
            manual_hint
        else
            # Keep only the packages the repo knows and that are not installed yet.
            to_install=()
            for p in "${pkgs[@]}"; do
                if ! pkg_installed "$p" && pkg_available "$p"; then
                    to_install+=("$p")
                fi
            done
            if [[ "${#to_install[@]}" -eq 0 ]]; then
                warn "no matching package found for the missing libraries"
                manual_hint
            elif confirm "Install the missing packages with ${family} (${to_install[*]})?"; then
                log "Installing: ${to_install[*]}"
                if install_packages "${to_install[@]}"; then
                    check_deps
                    if [[ "${#missing_libs[@]}" -eq 0 && "${missing_plugin}" -eq 0 ]]; then
                        log "Dependencies installed"
                    else
                        print_missing
                        manual_hint
                    fi
                else
                    warn "package installation failed"
                    manual_hint
                fi
            else
                warn "skipping package installation"
                manual_hint
            fi
        fi
        if [[ "${#missing_libs[@]}" -gt 0 || "${missing_plugin}" -gt 0 ]]; then
            warn "the app may not start until the libraries above are installed"
        fi
    fi
fi

# --- 3. Install the app ----------------------------------------------------

BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/applications"
ICON_DIR="$PREFIX/share/icons/hicolor/256x256/apps"
mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR"

log "Installing into $PREFIX"
install -m755 "$binary"  "$BIN_DIR/sticky-notes"
install -m644 "$icon"    "$ICON_DIR/sticky-notes.png"

# .desktop with an absolute Exec (works even if $BIN_DIR is not in PATH)
sed "s|^Exec=.*|Exec=$BIN_DIR/sticky-notes|" "$desktop" > "$APP_DIR/sticky-notes.desktop"
sed -i "s|^Icon=.*|Icon=$ICON_DIR/sticky-notes.png|" "$APP_DIR/sticky-notes.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APP_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true

log "Done"
log "  binary  : $BIN_DIR/sticky-notes"
log "  desktop : $APP_DIR/sticky-notes.desktop"
log "  icon    : $ICON_DIR/sticky-notes.png"
log "Start it from the KDE menu, or run: $BIN_DIR/sticky-notes"
