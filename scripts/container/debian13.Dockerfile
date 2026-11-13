# Build environment for the .deb package.
#
# The package is compiled on the oldest distribution it supports, because Qt is
# forward compatible but NOT backward compatible: Debian 13 (trixie) ships
# Qt 6.8.2 and LayerShellQt 6.3.4, so the binary built here also runs on
# Ubuntu 26.04+ / Debian forky (Qt 6.10 + LayerShellQt 6.6). Two LayerShellQt
# calls introduced after 6.3 are compiled out in NoteWindow.cpp (see
# CMakeLists.txt), which is why the older library is enough.
#
# The tarball and the AppImage keep their Ubuntu 26.04 baseline
# (scripts/container/ubuntu26.04.Dockerfile): this image only builds and
# packages the .deb, so it does not need the AppImage tooling.
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

WORKDIR /work
