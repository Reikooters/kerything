Install `rpm-build`:

```sh
sudo dnf install rpm-build dnf-plugins-core
```

Install dependencies:

```sh
sudo dnf builddep kerything.spec
```

Build KF6 variant:

```sh
./build-fedora-rpm.sh
```

Build Qt variant:

```sh
./build-fedora-rpm.sh --qt-only
```

Inspect RPM information:

```sh
rpm -qpi ../../../releases/kerything-*.rpm
```

Inspect RPM files:

```sh
rpm -qpl ../../../releases/kerything-*.rpm
```

Note: If both the `kerything` and `kerything-qt` builds are in the `/releases` folder, then `kerything-*.rpm` will output information for both files at once.
