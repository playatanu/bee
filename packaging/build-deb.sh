#!/usr/bin/env bash
#
# Build a Debian/Ubuntu .deb package for the Bee language.
# Produces dist/bee_<version>_<arch>.deb  — users install it with:
#     sudo apt install ./bee_<version>_<arch>.deb        (or: sudo dpkg -i ...)
#
# Re-runnable. Requires: g++, make, LLVM (llvm-config-17/18), dpkg-deb, fakeroot.
# The packaged binary is built WITH the native LLVM JIT enabled.
#
set -euo pipefail

VERSION="${VERSION:-0.2.0}"
ARCH="$(dpkg --print-architecture)"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="bee"
STAGE="$ROOT/dist/${PKG}_${VERSION}_${ARCH}"
DEB="$ROOT/dist/${PKG}_${VERSION}_${ARCH}.deb"

echo "[deb] building JIT-enabled binary (LLVM)..."
mkdir -p "$ROOT/dist"
# Build via the Makefile: it auto-detects llvm-config and links the native JIT
# (handling -fno-rtti for the LLVM translation unit correctly).
make -C "$ROOT" clean >/dev/null
make -C "$ROOT" VERSION="$VERSION"

# Fail loudly if the JIT did not get linked -- this package must ship with it.
if ! ldd "$ROOT/bee" | grep -qi 'libLLVM'; then
    echo "[deb] error: bee was built WITHOUT the LLVM JIT." >&2
    echo "       Install LLVM dev headers first, e.g.: sudo apt install llvm-18-dev" >&2
    exit 1
fi
cp "$ROOT/bee" "$ROOT/dist/bee"
strip "$ROOT/dist/bee"
# hive (the package manager) links no LLVM -- it just rides along.
cp "$ROOT/hive" "$ROOT/dist/hive"
strip "$ROOT/dist/hive"

# Auto-detect the runtime packages the binary links against, so Depends is right
# on whatever LLVM version this machine has (e.g. libllvm18).
DEPS="libc6, libstdc++6"
LLVM_SO="$(ldd "$ROOT/dist/bee" | awk '/libLLVM/{print $3; exit}')"
LLVM_PKG="$(dpkg -S "$(readlink -f "$LLVM_SO")" 2>/dev/null | cut -d: -f1 | head -n1)"
if [ -n "$LLVM_PKG" ]; then
    DEPS="$DEPS, $LLVM_PKG"
    echo "[deb] JIT runtime dependency: $LLVM_PKG"
fi

echo "[deb] staging file tree at $STAGE ..."
rm -rf "$STAGE"
install -Dm0755 "$ROOT/dist/bee"                 "$STAGE/usr/bin/bee"
install -Dm0755 "$ROOT/dist/hive"                "$STAGE/usr/bin/hive"
for f in "$ROOT"/examples/*.bee; do
    install -Dm0644 "$f" "$STAGE/usr/share/bee/examples/$(basename "$f")"
done
install -Dm0644 "$ROOT/README.md"                "$STAGE/usr/share/doc/bee/README.md"

# copyright (Debian expects one)
cat > "$STAGE/usr/share/doc/bee/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: BeeLang
Files: *
Copyright: Atanu Debnath
License: MIT
EOF

INSTALLED_KB="$(du -ks "$STAGE" | cut -f1)"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: Atanu Debnath <playatanu@gmail.com>
Section: devel
Priority: optional
Homepage: https://github.com/playatanu/beelang
Depends: $DEPS
Installed-Size: $INSTALLED_KB
Description: BeeLang - a small dynamically-typed scripting language
 BeeLang (the "bee" language) has first-class functions and closures, classes
 with single inheritance, lists and dicts, a module system, and a standard
 library. Hot numeric functions are compiled to native code by a built-in LLVM
 JIT. Run a program with: bee script.bee
 .
 Example programs are installed under /usr/share/bee/examples.
 .
 The package also installs "hive", the BeeLang package manager:
 hive install <package> fetches a package into ./hive_modules so that
 "import <package>" finds it.
EOF

echo "[deb] building package ..."
fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB" >/dev/null
rm -rf "$STAGE"

echo "[deb] done: $DEB"
dpkg-deb --info "$DEB" | sed 's/^/       /'
