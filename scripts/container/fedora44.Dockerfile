# Build environment for the .rpm package (scripts/build_rpm.sh).
#
# Packaging needs no compiler: the binary is the portable x86_64 build made by
# scripts/build_portable.sh in the Ubuntu 26.04 image, and this image only runs
# rpmbuild. Fedora 44 is the current baseline the package targets (the same
# binary also installs on openSUSE Tumbleweed, which has a newer glibc and Qt).
#
# The image is built once and cached, per architecture. For an aarch64 package
# on an x86_64 host, QEMU user emulation must be registered (binfmt_misc) and
# podman/docker run the image with --platform linux/arm64.
FROM docker.io/library/fedora:44

RUN dnf -y install rpm-build \
    && dnf clean all

WORKDIR /work
