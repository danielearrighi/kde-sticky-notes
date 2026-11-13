# Build environment for every release artifact that links or bundles Qt: the
# tarball, the AppImage, the .deb and (through the portable binary) the .rpm.
#
# The single baseline is the oldest supported distribution, because Qt is
# forward compatible but NOT backward compatible: Debian 13 (trixie) ships
# Qt 6.8.2 and LayerShellQt 6.3.4, so a binary built here also runs on
# Ubuntu 26.04+ / Debian forky / Fedora 44 (Qt 6.10 + LayerShellQt 6.6). The two
# LayerShellQt calls introduced after 6.3 (setDesiredSize(), setActivateOnShow())
# are compiled out in NoteWindow.cpp (see CMakeLists.txt), which is what makes
# the older library enough.
#
# One image for everything: mixing baselines means shipping artifacts that
# disagree about the oldest Qt they support, and the AppImage/linuxdeploy
# tooling is the only extra it needs on top of the compilation deps.
FROM docker.io/library/debian:13

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        dpkg-dev \
        qt6-base-dev \
        qt6-wayland-dev \
        liblayershellqtinterface-dev \
    && rm -rf /var/lib/apt/lists/*

# AppImage tooling: curl to download linuxdeploy/appimagetool, file, patchelf
# and binutils for their ELF inspection. The AppImages are run with
# APPIMAGE_EXTRACT_AND_RUN=1 (the scripts set it), so FUSE is not required.
# qt6-wayland carries the Wayland platform plugin that the AppImage must bundle
# (Qt's own package is not a dependency of the -dev one on Debian, and the
# tarball/.deb get it from the target system instead).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        file \
        patchelf \
        binutils \
        qt6-wayland \
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
