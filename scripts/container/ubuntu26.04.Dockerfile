# Build environment for the release artifacts that are built in a container:
# the plain (tarball) binary and the AppImage.
#
# Ubuntu 26.04 ships Qt 6.10.2 and LayerShellQt 6.6.4: the baseline the tarball
# and the AppImage are built against. The .deb is built separately on Debian 13
# (Qt 6.8 + LayerShellQt 6.3.4, see debian13.Dockerfile), where the two
# LayerShellQt calls added in 6.4/6.5 are compiled out.
#
# Qt is forward compatible but NOT backward compatible: a binary built against
# Qt 6.12 (Tumbleweed) refuses to start on a system with Qt 6.11 (Fedora 44)
# with "version `Qt_6.12' not found". Building here keeps the tarball runnable
# on every distribution with Qt >= 6.10 and LayerShellQt >= 6.6.
#
# The same image is used for the aarch64 builds (scripts/build_*.sh --arch
# aarch64) through QEMU user emulation, so it also carries what the AppImage
# tooling needs: curl to download linuxdeploy/appimagetool, file/patchelf and
# binutils for their ELF inspection. The AppImages are run with
# APPIMAGE_EXTRACT_AND_RUN=1 (the scripts set it), so FUSE is not required.
FROM docker.io/library/ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        qt6-base-dev \
        qt6-wayland-dev \
        liblayershellqtinterface-dev \
        curl \
        ca-certificates \
        file \
        patchelf \
    && rm -rf /var/lib/apt/lists/*

# Optional: the KDE platform theme and the Breeze style, so the AppImage built
# in this container looks native on Plasma instead of falling back to Qt's
# Fusion style. They are not needed to compile the app, so a missing package
# must not break the image.
RUN apt-get update \
    && (apt-get install -y --no-install-recommends plasma-integration breeze \
        || echo "plasma-integration/breeze not available: the AppImage will use the Qt default style") \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
