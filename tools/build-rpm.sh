#!/usr/bin/env bash
# Swifty, a fork of Swiftify
# Copyright (C) 2026  ImAlekz
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# build-rpm.sh - Build the Swifty RPM on Fedora (and other rpm based systems)
#
# Makes the source tarball from this checkout and builds packaging/rpm/swifty.spec
# with rpmbuild, in a folder of its own: nothing in your home is touched. the rpm goes
# in dist/ (or --out). it needs the network, for cmake, cargo and the fonts.
#
# Usage:
#   ./tools/build-rpm.sh
#   ./tools/build-rpm.sh --install-deps      # also install build dependencies (sudo)
#   ./tools/build-rpm.sh --out ~/rpms --jobs 8
#   sudo dnf install dist/swifty-*.rpm

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[*]${NC} $1"; }
warn()  { echo -e "${YELLOW}[!]${NC} $1"; }
error() { echo -e "${RED}[x]${NC} $1" >&2; exit 1; }

INSTALL_DEPS=false
OUT="dist"
JOBS="$(nproc 2>/dev/null || echo 4)"
REPO_DIR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --out) OUT="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --install-deps    Install build dependencies with dnf (requires sudo)"
            echo "  --out PATH        Where the rpm goes (default: dist)"
            echo "  --jobs N          Parallel build jobs (default: all cores)"
            echo "  -h, --help        Show this help"
            exit 0
            ;;
        *) REPO_DIR="$1"; shift ;;
    esac
done

# Find repo root
if [[ -n "$REPO_DIR" ]]; then
    cd "$REPO_DIR"
elif [[ -f "CMakeLists.txt" ]] && grep -q "project(Swifty" CMakeLists.txt 2>/dev/null; then
    : # already in repo root
elif [[ -f "../CMakeLists.txt" ]] && grep -q "project(Swifty" ../CMakeLists.txt 2>/dev/null; then
    cd ..
else
    error "Run this script from the swifty repository root, or pass the path as an argument"
fi
REPO_DIR="$(pwd)"
[[ -f packaging/rpm/swifty.spec ]] || error "packaging/rpm/swifty.spec is missing"

# The version is the one of CMakeLists.txt, so the rpm, the app and the PKGBUILD agree
VERSION="$(sed -n '/^project(Swifty/,/)/ s/.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n1)"
[[ -n "$VERSION" ]] || error "could not read the version from CMakeLists.txt"
log "Building swifty $VERSION as an rpm, from $REPO_DIR"

# Install dependencies if requested. no qtkeychain and no openssl: swifty keeps no
# login. qt's sqlite driver is part of qt6-qtbase on Fedora
if $INSTALL_DEPS; then
    command -v dnf >/dev/null 2>&1 || error "dnf not found: install the BuildRequires of packaging/rpm/swifty.spec yourself"
    log "Installing build dependencies..."
    sudo dnf install -y rpm-build rpmdevtools git cmake ninja-build gcc-c++ rust cargo pkgconf-pkg-config \
        pulseaudio-libs-devel alsa-lib-devel \
        qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qtshadertools-devel \
        qt6-qtshadertools qt6-qtwayland qt6-qtimageformats qcoro-qt6-devel hicolor-icon-theme
fi

command -v rpmbuild >/dev/null 2>&1 || error "rpmbuild not found. Install rpm-build or run with --install-deps"
command -v cmake >/dev/null 2>&1 || error "cmake not found. Install it or run with --install-deps"
command -v cargo >/dev/null 2>&1 || error "cargo not found. Install rust and cargo or run with --install-deps"

TOP="$(mktemp -d "${TMPDIR:-/tmp}/swifty-rpm.XXXXXX")"
trap 'rm -rf "$TOP"' EXIT
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,RPMS,SRPMS,BUILDROOT}

# The source tarball: what git tracks (your uncommitted changes too), or, without git,
# the folder without its build and dist folders
log "Making the source tarball..."
TARBALL="$TOP/SOURCES/swifty-$VERSION.tar.gz"
if git -C "$REPO_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    (cd "$REPO_DIR" && git ls-files -z --cached --others --exclude-standard \
        | while IFS= read -r -d '' f; do
            # a build or a finished rpm in the folder does not go in the source
            [[ -e "$f" && "$f" != build/* && "$f" != dist/* ]] && printf '%s\0' "$f"
          done \
        | tar --null -T - --transform "s,^,swifty-$VERSION/," -czf "$TARBALL")
else
    warn "not a git checkout: taking the whole folder, without build/ and dist/"
    tar --exclude=./build --exclude=./dist --exclude=./.git --transform "s,^\./,swifty-$VERSION/," \
        -czf "$TARBALL" -C "$REPO_DIR" .
fi

cp packaging/rpm/swifty.spec "$TOP/SPECS/swifty.spec"

log "Building the rpm (this takes a few minutes)..."
rpmbuild -bb "$TOP/SPECS/swifty.spec" \
    --define "_topdir $TOP" \
    --define "swifty_version $VERSION" \
    --define "_smp_mflags -j$JOBS"

mkdir -p "$OUT"
find "$TOP/RPMS" -name '*.rpm' -exec cp -v {} "$OUT/" \;

log "Done! The rpm is in $OUT/"
echo ""
echo "Install it with:"
echo "  sudo dnf install $OUT/swifty-*.rpm"
echo ""
echo "Before first use:"
echo "  1. Add your music: a folder or files, from the Home screen or the settings"
echo "  2. (optional) A Discogs token in the settings, for artist pictures"
echo "  3. (optional) A Discord Application ID in the settings, for rich presence"
