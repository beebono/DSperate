#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CI's build dependencies, per job kind: tools/ci_deps.sh native|armv7
#
# libvulkan-dev: the headers only; nothing links libvulkan (it is dlopen'd on
# the device). Without them the build silently has no GPU 3D raster and no
# Vulkan present -- the jobs' configure check refuses that.
#
# armv7: SDL2 and the Vulkan headers come from Ubuntu's armhf ports, which the
# runner's own package sources do not carry: they are held to amd64 and the
# ports added beside them.
set -euo pipefail
case ${1:-} in
  native)
    sudo apt-get update
    sudo apt-get install -y ninja-build libsdl2-dev libvulkan-dev ;;
  armv7)
    sudo dpkg --add-architecture armhf
    sudo sed -i '/^Types:/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
    printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-security\nComponents: main universe\nArchitectures: armhf\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' | sudo tee /etc/apt/sources.list.d/armhf.sources
    sudo apt-get update
    sudo apt-get install -y ninja-build g++-arm-linux-gnueabihf qemu-user-static libsdl2-dev:armhf libvulkan-dev:armhf ;;
  *) echo "usage: tools/ci_deps.sh native|armv7" >&2; exit 2 ;;
esac
