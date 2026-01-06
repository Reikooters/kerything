# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

pkgname=kerything
pkgver=1.2.0
pkgrel=1
pkgdesc="Fast NTFS file scanner using trigrams"
arch=('x86_64')
url="https://github.com/Reikooters/kerything"
license=('GPL-3.0-or-later')
depends=('qt6-base' 'kwidgetsaddons' 'kcoreaddons' 'kio' 'solid' 'onetbb' 'e2fsprogs')
makedepends=('cmake' 'extra-cmake-modules')

# Disable the creation of the -debug package
options=('!debug')

#source=("git+${url}.git#tag=v${pkgver}")
#sha256sums=('SKIP')

# By leaving source empty, makepkg expects to be run in the directory with the files
source=()
sha256sums=()

build() {
  cmake -B build -S "$startdir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -Wno-dev
  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build
}