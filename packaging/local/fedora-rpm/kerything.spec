%bcond_without kf6

%if %{with kf6}
Name:           kerything
%global kerything_kf6 ON
%else
Name:           kerything-qt
%global kerything_kf6 OFF
%endif
Version:        2.8.0
Release:        1%{?dist}
Summary:        Fast file search for Linux block devices
License:        GPL-3.0-or-later
URL:            https://github.com/Reikooters/kerything
Source0:        kerything-%{version}.tar.gz

BuildRequires:  cmake >= 4.2
BuildRequires:  cmake-rpm-macros
BuildRequires:  gcc-c++
BuildRequires:  pkgconf-pkg-config
BuildRequires:  qt6-qtbase-devel
%if %{with kf6}
BuildRequires:  extra-cmake-modules
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kio-devel
BuildRequires:  kf6-kxmlgui-devel
Conflicts:      kerything-qt
%else
Conflicts:      kerything
%endif
BuildRequires:  tbb-devel
BuildRequires:  re2-devel
BuildRequires:  e2fsprogs-devel
BuildRequires:  libblkid-devel
BuildRequires:  libmount-devel
BuildRequires:  systemd-devel
BuildRequires:  systemd-rpm-macros

Requires:       systemd

%description
Kerything indexes Linux block devices and searches file names using bigram and
trigram indexes. It includes a graphical client and a socket-activated daemon
that scans devices with elevated privileges.

%prep
%autosetup -n kerything-%{version}

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DKERYTHING_WITH_KF6=%{kerything_kf6} \
    -DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=%{_unitdir}
%cmake_build

%install
%cmake_install
install -Dpm 0644 packaging/kerything.sysusers \
    %{buildroot}%{_sysusersdir}/kerything.conf

# RPM installs license files through %%license.
rm -f %{buildroot}%{_datadir}/licenses/kerything/LICENSE
rmdir %{buildroot}%{_datadir}/licenses/kerything

%post
%systemd_post kerythingd.socket

%preun
%systemd_preun kerythingd.socket

%postun
%systemd_postun_with_restart kerythingd.service

%files
%license LICENSE
%{_bindir}/kerything
%{_bindir}/kerythingd
%{_unitdir}/kerythingd.service
%{_unitdir}/kerythingd.socket
%{_sysusersdir}/kerything.conf
%{_datadir}/applications/net.reikooters.kerything.desktop
%{_datadir}/icons/hicolor/*/apps/kerything.png
