# swifty.spec - the RPM of Swifty, a local music player for Wayland
# Maintainer: ImAlekzzz
#
# Built by tools/build-rpm.sh, which makes the source tarball from the checkout and
# passes the version of CMakeLists.txt as swifty_version. build it by hand with
#   rpmbuild -bb --define "swifty_version 0.3.3" packaging/rpm/swifty.spec
# after putting swifty-<version>.tar.gz in the SOURCES folder.
#
# it needs the network while it builds: cmake fetches Corrosion, cargo fetches the Rust
# crates, and the interface and icon fonts are downloaded (each checked against a hash).

%{!?swifty_version:%global swifty_version 0.3.3}

# the Rust core is not built with Fedora's lto and has no debuginfo of its own
%global _lto_cflags %{nil}
%global debug_package %{nil}

Name:           swifty
Version:        %{swifty_version}
Release:        1%{?dist}
Summary:        A local music player for Wayland, in Material 3
License:        GPL-3.0-or-later
URL:            https://github.com/ImAlekzzz/swifty
Source0:        %{name}-%{version}.tar.gz
Packager:       ImAlekzzz

BuildRequires:  cmake >= 3.24
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  git
BuildRequires:  rust
BuildRequires:  cargo
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6Sql)
BuildRequires:  cmake(Qt6Qml)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(Qt6ShaderTools)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(QCoro6Core)
BuildRequires:  cmake(QCoro6Network)
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  qt6-qtshadertools

# qt's sqlite driver is in qt6-qtbase, which the libraries pull in. the rest is what the
# app loads when it runs
Requires:       qt6-qtwayland
Requires:       qt6-qtimageformats
Requires:       hicolor-icon-theme
Recommends:     xdg-desktop-portal

%description
Swifty plays the music on your computer: folders and files, playlists, synced lyrics,
favourites, a spectrum, and Discord rich presence. It is a fork of Swifty
(https://github.com/swiftify-org/swiftify) with everything of Spotify removed, in
Material 3 Expressive after end4's dots-hyprland, and uses Qt Quick and a Rust audio core.

%prep
%autosetup -n %{name}-%{version}
# the checkout asks for a newer qt than some releases have: patch out what this one
# is too old for, as tools/build-debian.sh does
qt_minor="$(pkg-config --modversion Qt6Core | cut -d. -f2)"
if [ "$qt_minor" -lt 10 ]; then
    sed -i -E 's/Qt6 6\.(9|10)/Qt6 6.8/g; s/REQUIRES 6\.(9|10)/REQUIRES 6.8/g' CMakeLists.txt
    find qml -name '*.qml' -exec sed -i \
        -e 's/final readonly property/readonly property/g' \
        -e 's/final property/property/g' {} \;
    if [ "$qt_minor" -lt 9 ] && grep -q 'bottomPadding: 0' qml/Main.qml; then
        sed -i '/topPadding: 0/d; /leftPadding: 0/d; /rightPadding: 0/d; /bottomPadding: 0/d' qml/Main.qml
        sed -i '/since qt 6.9/,/clear itself/d' qml/Main.qml
    fi
fi

%build
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install

%files
%license %{_datadir}/licenses/swifty
%{_bindir}/swifty
%{_datadir}/swifty/
%{_datadir}/applications/swifty.desktop
%{_datadir}/icons/hicolor/*/apps/swifty.png

%changelog
* Sun Nov 15 2026 ImAlekzzz - 0.3.3-1
- Swifty as an RPM: local files, SQLite playlists and library cache, Discord rich presence
