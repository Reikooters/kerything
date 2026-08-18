# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

pkgname=kerything
pkgver=2.4.0
pkgrel=1
pkgdesc='Fast Linux block device file scanner and search application using trigrams'
arch=('x86_64')
url='https://github.com/Reikooters/kerything'
license=('GPL-3.0-or-later')
install='kerything.install'

depends=(
  'qt6-base' # Qt6::Widgets, Qt6::Core, Qt6::Network
  'onetbb' # TBB::tbb
  'e2fsprogs' # ext2fs, com_err
  'util-linux-libs' # blkid, mount
  'systemd-libs' # libsystemd, libudev
  'kcoreaddons' # KF6::CoreAddons
  'ki18n' # KF6::I18n
  'kio' # KF6::KIO*, KF6::Service
  'kxmlgui' # KF6::XmlGui
)

makedepends=(
  'cmake'
  'extra-cmake-modules'
  'pkgconf'
  'systemd'
)

# Disable the creation of the -debug package
# Add link time optimization
# Stop Arch Linux from injecting its default build flags
options=(
  '!debug'
  'lto'
  '!buildflags'
)

# For release packaging, use this:
# source=("git+${url}.git#tag=v${pkgver}")
# sha256sums=('SKIP')

# For local makepkg builds from the project directory:
source=()
sha256sums=()

build() {
  # Omit `-Wp,-D_GLIBCXX_ASSERTIONS` as this security flag can significantly
  # reduce application performance.
  local my_flags='-march=x86-64 -mtune=generic -O2 -flto=auto -DNDEBUG -fno-plt -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fstack-protector-strong -fstack-clash-protection -fcf-protection -fexceptions -Wp,-D_FORTIFY_SOURCE=3'

  cmake -B build -S "$startdir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="$my_flags" \
    -DCMAKE_C_FLAGS_RELEASE="$my_flags" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE='-Wl,-O1,--sort-common,--as-needed' \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=/usr/lib/systemd/system \
    -DKERYTHING_WITH_KF6=ON \
    -Wno-author

  cmake --build build --parallel "$(nproc)"
}

package() {
  DESTDIR="$pkgdir" cmake --install build

  # Create kerything group if it does not already exist
  install -Dm644 /dev/stdin "$pkgdir/usr/lib/sysusers.d/kerything.conf" <<'EOF'
g kerything - - -
EOF
}