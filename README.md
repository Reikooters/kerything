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
- `kerythingd` — the privileged backend daemon responsible for device discovery and indexing

Kerything also installs two systemd system units:

- `kerythingd.socket` — creates the local IPC socket and starts the daemon on demand
- `kerythingd.service` — runs the backend daemon when activated by the socket

The daemon is socket-activated. This means `kerythingd.service` does not need to
be enabled to start at boot. Instead, `kerythingd.socket` should be enabled. When
the GUI connects to `/run/kerythingd/kerythingd.sock`, systemd starts the daemon
automatically if it is not already running.

The daemon exits automatically after it has had no GUI clients connected for 5
minutes. The socket remains active, so the next GUI connection starts the daemon
again.

## Desktop integration

Kerything can be built in two modes:

| Build mode | CMake flag | Description |
|---|---:|---|
| KDE Frameworks 6 | `-DKERYTHING_WITH_KF6=ON` | Recommended for KDE Plasma users. Enables KDE integration such as richer file opening, “Show in File Manager”, “Open With”, and terminal launching. |
| Qt-only | `-DKERYTHING_WITH_KF6=OFF` | Recommended for non-KDE desktops. Uses Qt and freedesktop-compatible fallbacks where possible. |

The KDE build is recommended if you are using KDE Plasma.

## Quick start on Arch Linux

If you already have Kerything installed and you are updating to a new version, please see [Updating](#Updating).

### 1. Install from the included PKGBUILD

From the project root:

```bash
makepkg -si -f -c
```

This builds Kerything with KDE Frameworks 6 integration enabled.

### 2. Set up daemon socket access

Kerything uses a privileged daemon for low-level device access. The GUI talks to
the daemon over a Unix socket at:

```text
/run/kerythingd/kerythingd.sock
```

Access to this socket is controlled by the `kerything` group. Create the group,
add your user to it, and enable the socket unit:

```bash
sudo groupadd -r kerything
sudo usermod -aG kerything "$USER"
sudo systemctl enable --now kerythingd.socket
```

Restart your computer, or log out and back in, for the group change to take effect.

> [!IMPORTANT]
> Enable `kerythingd.socket`, not `kerythingd.service`. The service is started
> automatically by systemd when the GUI connects to the socket.

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

sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
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

sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
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
- libsystemd development files
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

sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
```

Or the Qt-only version:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=OFF

cmake --build build --parallel
sudo cmake --install build

sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
```

> [!IMPORTANT]
> Use `-DCMAKE_BUILD_TYPE=Release` for normal use. Debug builds are significantly
> slower and are intended for development only.

## systemd unit installation directory

By default, Kerything installs its systemd system units to:

```text
${CMAKE_INSTALL_LIBDIR}/systemd/system
```

With `-DCMAKE_INSTALL_PREFIX=/usr`, this is usually:

```text
/usr/lib/systemd/system
```

Some distributions use a different systemd system unit directory, such as:

```text
/lib/systemd/system
```

or:

```text
/usr/local/lib/systemd/system
```

You can override the unit installation directory with:

```bash
-DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=/path/to/systemd/system
```

For example:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=/usr/lib/systemd/system \
  -DKERYTHING_WITH_KF6=ON
```

After installing changed systemd units, reload systemd:

```bash
sudo systemctl daemon-reload
```

## Socket permissions and daemon activation

Kerything uses systemd socket activation.

The GUI does not start the daemon directly and should not need administrator
authentication during normal use. Instead:

1. systemd creates `/run/kerythingd/kerythingd.sock`
2. the socket is owned by `root:kerything`
3. members of the `kerything` group can connect to it
4. connecting to the socket starts `kerythingd.service` on demand

If you did not already do this during setup, create the `kerything` group and add
your user to it:

```bash
sudo groupadd -r kerything
sudo usermod -aG kerything "$USER"
```

Restart your computer, or log out and back in, for the group change to take effect.

Then enable and start the socket:

```bash
sudo systemctl enable --now kerythingd.socket
```

Check the socket status with:

```bash
systemctl status kerythingd.socket
```

Check the daemon status with:

```bash
systemctl status kerythingd.service
```

It is normal for `kerythingd.service` to be inactive when the GUI is not running.
The socket should remain active.

## Updating

### Update with makepkg on Arch Linux

Be sure to close the GUI app first. If you update while the GUI is open, the
socket may re-activate the daemon during installation.

```bash
# Stop both the daemon service and socket before installing the new build
sudo systemctl stop kerythingd.service
sudo systemctl stop kerythingd.socket
sudo rm -rf /run/kerythingd

# Rebuild and install
makepkg -si -f -c

# Reload systemd and start the socket again
sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
```

You usually do not need to manually start `kerythingd.service`. It will start
automatically the next time the GUI connects to the socket.

For a Qt-only build, use:

```bash
-DKERYTHING_WITH_KF6=OFF
```

instead of:

```bash
-DKERYTHING_WITH_KF6=ON
```

### Manual update using CMAKE

Be sure to close the GUI app first. If you update while the GUI is open, the
socket may re-activate the daemon during installation.

```bash
# Stop both the daemon service and socket before installing the new build
sudo systemctl stop kerythingd.service
sudo systemctl stop kerythingd.socket
sudo rm -rf /run/kerythingd

# Rebuild and install
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=ON

cmake --build build --parallel
sudo cmake --install build

# Reload systemd and start the socket again
sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
```

You usually do not need to manually start `kerythingd.service`. It will start
automatically the next time the GUI connects to the socket.

For a Qt-only build, use:

```bash
-DKERYTHING_WITH_KF6=OFF
```

instead of:

```bash
-DKERYTHING_WITH_KF6=ON
```

## Uninstalling



## Development daemon usage

For development, you can run `kerythingd` manually instead of through systemd.
When no systemd socket is passed to the daemon, it falls back to creating and
listening on `/run/kerythingd/kerythingd.sock` itself.

The daemon still needs sufficient privileges to inspect block devices and create
the socket under `/run`, so development runs commonly require root privileges:

```bash
sudo ./build/kerythingd
```

Make sure the systemd socket is stopped first, otherwise the socket path may
already be in use:

```bash
sudo systemctl stop kerythingd.service
sudo systemctl stop kerythingd.socket
```

When switching back to normal installed usage:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now kerythingd.socket
```

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
