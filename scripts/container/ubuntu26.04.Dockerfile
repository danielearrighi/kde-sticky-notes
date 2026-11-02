# Build environment for the plain (tarball) release binary.
#
# Ubuntu 26.04 ships Qt 6.10.2 and LayerShellQt 6.6.4: the oldest versions that
# still provide the layer-shell API the notes use (setDesiredSize() and
# setActivateOnShow(), added in LayerShellQt 6.6, which itself needs Qt 6.10).
#
# Qt is forward compatible but NOT backward compatible: a binary built against
# Qt 6.12 (Tumbleweed) refuses to start on a system with Qt 6.11 (Fedora 44)
# with "version `Qt_6.12' not found". Building here keeps the tarball runnable
# on every distribution with Qt >= 6.10 and LayerShellQt >= 6.6.
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
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
