# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

pkgname=kerything
pkgver=1.4.0
pkgrel=1
pkgdesc="Fast NTFS and EXT4 file scanner using trigrams"
arch=('x86_64')
url="https://github.com/Reikooters/kerything"
license=('GPL-3.0-or-later')
depends=('qt6-base' 'kwidgetsaddons' 'kcoreaddons' 'kio' 'solid' 'onetbb' 'e2fsprogs')
makedepends=('cmake' 'extra-cmake-modules')

# Disable the creation of the -debug package
# Add link time optimization
# Stop Arch Linux from injecting its default build flags
options=('!debug' 'lto' '!buildflags')

#source=("git+${url}.git#tag=v${pkgver}")
#sha256sums=('SKIP')

# By leaving source empty, makepkg expects to be run in the directory with the files
source=()
sha256sums=()

build() {
  # Omit `-Wp,-D_GLIBCXX_ASSERTIONS` as this security flag decreases application performance by 10x
  local my_flags="-march=x86-64 -mtune=generic -O2 -flto=auto -DNDEBUG -fno-plt -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fstack-protector-strong -fstack-clash-protection -fcf-protection -fexceptions -Wp,-D_FORTIFY_SOURCE=3"

  cmake -B build -S "$startdir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="$my_flags" \
    -DCMAKE_C_FLAGS_RELEASE="$my_flags" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="-Wl,-O1,--sort-common,--as-needed" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -Wno-dev

  cmake --build build --parallel $(nproc)
}

package() {
  DESTDIR="$pkgdir" cmake --install build
}