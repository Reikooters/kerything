# Kerything 🔍

> [!NOTE]
> This branch contains the rewrite of the Kerything project.

Kerything is a fast Linux file search application inspired by the Windows utility “Everything” by Voidtools. It indexes supported Linux block devices and provides fast filename search using trigram indexes.

Kerything currently supports indexing the following file systems:

- EXT4
- NTFS

## Requirements

Kerything is designed for Linux systems using `systemd`.

The backend daemon uses `libudev` to monitor block device changes, such as devices
being created, removed, plugged in, unplugged, mounted, or unmounted. Because of
this, Kerything is intended for systemd-based Linux distributions.

Kerything consists of two executables:

- `kerything` — the graphical application
- `kerythingd` — the backend daemon responsible for device discovery and indexing

## Desktop integration

Kerything can be built in two modes:

| Build mode | CMake flag | Description |
|---|---:|---|
| KDE Frameworks 6 | `-DKERYTHING_WITH_KF6=ON` | Recommended for KDE Plasma users. Enables KDE integration such as richer file opening, “Show in File Manager”, “Open With”, and terminal launching. |
| Qt-only | `-DKERYTHING_WITH_KF6=OFF` | Recommended for non-KDE desktops. Uses Qt and freedesktop-compatible fallbacks where possible. |

The KDE build is recommended if you are using KDE Plasma.

## Quick start on Arch Linux

### 1. Install from the included PKGBUILD

From the project root:

```bash
makepkg -si -f
```

This builds Kerything with KDE Frameworks 6 integration enabled.

### 2. Set up socket permissions

To allow the Kerything daemon to bind to its socket and allow the GUI process to
communicate with it, create a `kerything` group and add your user to it:

```bash
sudo groupadd kerything
sudo usermod -aG kerything "$USER"
```

Restart your computer, or log out and back in, for the group change to take effect.

### 3. Run Kerything

After installing and refreshing your login session, launch Kerything from your
application menu or run:

```bash
kerything
```

On first run, Kerything will ask which detected devices you want to index.

## Building manually on Arch Linux

### KDE Frameworks 6 build

Recommended for KDE Plasma users.

```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  extra-cmake-modules \
  pkgconf \
  qt6-base \
  onetbb \
  e2fsprogs \
  util-linux-libs \
  systemd-libs \
  kcoreaddons \
  ki18n \
  kio \
  kxmlgui

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=ON

cmake --build build --parallel
sudo cmake --install build
```

### Qt-only build

Recommended for non-KDE desktops.

```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  pkgconf \
  qt6-base \
  onetbb \
  e2fsprogs \
  util-linux-libs \
  systemd-libs

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=OFF

cmake --build build --parallel
sudo cmake --install build
```

## Building on other distributions

Install the equivalent development packages for:

- CMake
- a C++26-capable compiler
- Qt 6 Core, Widgets, and Network
- oneTBB
- e2fsprogs development files, including `ext2fs` and `com_err`
- util-linux development files, including `blkid` and `mount`
- libudev development files
- pkg-config / pkgconf

For KDE Frameworks 6 integration, also install the development packages for:

- Extra CMake Modules
- KF6 CoreAddons
- KF6 I18n
- KF6 KIO
- KF6 XmlGui

Then build either the KDE version:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=ON

cmake --build build --parallel
sudo cmake --install build
```

Or the Qt-only version:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=OFF

cmake --build build --parallel
sudo cmake --install build
```

> [!IMPORTANT]
> Use `-DCMAKE_BUILD_TYPE=Release` for normal use. Debug builds are significantly
> slower and are intended for development only.

## Socket permissions

Kerything uses a local socket for communication between the GUI and backend daemon.

If you did not already do this during the quick start setup, create a `kerything`
group and add your user to it:

```bash
sudo groupadd kerything
sudo usermod -aG kerything "$USER"
```

Restart your computer, or log out and back in, for the group change to take effect.

## Build options

### KDE integration

Enable KDE Frameworks 6 integration:

```bash
-DKERYTHING_WITH_KF6=ON
```

Disable KDE Frameworks 6 integration and use the Qt-only fallback implementation:

```bash
-DKERYTHING_WITH_KF6=OFF
```

When no flag is specified, it defaults to `ON`.

### Verbose logging

Verbose logging is disabled by default.

To enable verbose console logging for debugging, add:

```bash
-DKERYTHING_ENABLE_LOGGING=ON
```

For example:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=ON \
  -DKERYTHING_ENABLE_LOGGING=ON

cmake --build build --parallel
```

## Credits

Kerything makes use of the following open-source libraries and frameworks:

- **[Qt](https://www.qt.io/):** Cross-platform application framework used for the GUI, IPC, and core application functionality.
- **[KDE Frameworks](https://develop.kde.org/products/frameworks/):** Optional KDE integration for file actions, application launching, file manager integration, and desktop behavior.
- **[oneTBB](https://github.com/oneapi-src/oneTBB):** Intel's oneAPI Threading Building Blocks library for parallelism.
- **[e2fsprogs](https://github.com/tytso/e2fsprogs):** Linux filesystem tools and libraries used for EXT4 support.
- **[util-linux](https://github.com/util-linux/util-linux):** Provides libraries such as `blkid` and `mount` used for block device and mount information.
- **[systemd / libudev](https://github.com/systemd/systemd):** Used by the daemon to monitor block device changes.
- **[utfcpp](https://github.com/nemtrif/utfcpp):** A simple, portable and lightweight library for handling UTF-8 encoded strings in C++.

## License

This project is licensed under the **GPL-3.0-or-later** License. See the [LICENSE](LICENSE) file for details.
