#!/usr/bin/env bash
# Shared architecture helpers for the release scripts.
#
# Sourced, never executed: it only defines functions. Supported architectures
# are the ones whose Qt/Flatpak packages exist for KDE Plasma, x86_64 and
# aarch64.
#
# A build for an architecture different from the host one ("foreign" build) runs
# the target toolchain through QEMU user emulation: the kernel must have a
# binfmt_misc entry for it (qemu-aarch64, or qemu-x86_64 on an ARM host). A
# native aarch64 machine needs nothing special.

sn_die() { printf '%s\n' "$*" >&2; exit 1; }

sn_known_arch() {
    case "$1" in
        x86_64|aarch64) return 0 ;;
        *) return 1 ;;
    esac
}

# The host architecture, validated.
sn_host_arch() {
    local host
    host="$(uname -m)"
    sn_known_arch "$host" || sn_die "Unsupported architecture: $host (need x86_64 or aarch64)"
    printf '%s\n' "$host"
}

# The other supported architecture: the second target of build_all.sh.
sn_other_arch() {
    if [[ "$(sn_host_arch)" == x86_64 ]]; then
        printf 'aarch64\n'
    else
        printf 'x86_64\n'
    fi
}

sn_is_native() { [[ "$1" == "$(sn_host_arch)" ]]; }

# Container platform string for podman/docker (--platform).
sn_platform() {
    case "$1" in
        x86_64)  printf 'linux/amd64\n' ;;
        aarch64) printf 'linux/arm64\n' ;;
        *) sn_die "Unsupported architecture: $1 (need x86_64 or aarch64)" ;;
    esac
}

# Debian architecture name, as used by dpkg and in the .deb file name.
sn_deb_arch() {
    case "$1" in
        x86_64)  printf 'amd64\n' ;;
        aarch64) printf 'arm64\n' ;;
        *) sn_die "Unsupported architecture: $1 (need x86_64 or aarch64)" ;;
    esac
}

# RPM architecture name (same as the ELF/uname name).
sn_rpm_arch() {
    sn_known_arch "$1" || sn_die "Unsupported architecture: $1 (need x86_64 or aarch64)"
    printf '%s\n' "$1"
}

# Exit unless the ELF file $1 is built for architecture $2. Guards against a
# --binary (or a leftover build tree) of the other architecture: the package
# name would silently lie.
sn_verify_binary_arch() {
    local binary="$1" arch="$2" machine
    command -v readelf >/dev/null 2>&1 || return 0
    machine="$(LC_ALL=C readelf -h "$binary" 2>/dev/null \
        | sed -n 's/^ *Machine: *//p' | head -1)"
    case "$arch:$machine" in
        x86_64:*X86-64*|aarch64:*AArch64*) return 0 ;;
        *) sn_die "$binary is not a $arch binary (readelf: ${machine:-unknown})" ;;
    esac
}

sn_binfmt_ok() {
    [[ -e "/proc/sys/fs/binfmt_misc/qemu-$1" \
       || -e "/proc/sys/fs/binfmt_misc/qemu-$1-static" ]]
}

sn_binfmt_hint() {
    cat <<'EOF'
A build for another architecture runs the target toolchain through QEMU user
emulation. Install it and make sure binfmt_misc is registered:
  openSUSE:       sudo zypper install qemu-linux-user
  Fedora:         sudo dnf install qemu-user-static
  Debian/Ubuntu:  sudo apt install qemu-user-static
Then check that it is active:
  ls /proc/sys/fs/binfmt_misc/ | grep qemu-
If it is installed but not registered, reload the entries:
  sudo systemctl restart systemd-binfmt
EOF
}

# Exit unless the target architecture can be executed on this host.
sn_require_binfmt() {
    local arch="$1"
    sn_is_native "$arch" && return 0
    if ! sn_binfmt_ok "$arch"; then
        printf 'Missing QEMU user emulation for %s (no qemu-%s entry in /proc/sys/fs/binfmt_misc)\n' \
            "$arch" "$arch" >&2
        sn_binfmt_hint >&2
        exit 1
    fi
}

# Print the container engine from CONTAINER_ENGINE, podman or docker. Fails when
# none is usable.
sn_container_engine() {
    local engine candidate
    if [[ -n "${CONTAINER_ENGINE:-}" ]]; then
        command -v "$CONTAINER_ENGINE" >/dev/null 2>&1 || return 1
        printf '%s\n' "$CONTAINER_ENGINE"
        return 0
    fi
    for candidate in podman docker; do
        if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}
