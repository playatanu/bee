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

VERSION="${VERSION:-0.3.7}"
ARCH="$(dpkg --print-architecture)"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="bee"
STAGE="$ROOT/dist/${PKG}-${VERSION}-${ARCH}"
# Hyphens, not Debian's usual name_version_arch.deb, to match the naming of the
# Windows installer. dpkg reads the package name, version and architecture from
# DEBIAN/control, not the filename, so `apt install ./bee-0.3.3-amd64.deb`
# installs exactly the same package either way.
DEB="$ROOT/dist/${PKG}-${VERSION}-${ARCH}.deb"

echo "[deb] building JIT-enabled binary (LLVM)..."
mkdir -p "$ROOT/dist"
# Build via the Makefile: it auto-detects llvm-config and builds the JIT backend
# as libbee_jit.so (handling -fPIC/-fno-rtti for the LLVM translation unit).
make -C "$ROOT" clean >/dev/null
# Bake the *installed* locations into beec so the AOT compiler finds its headers
# (/usr/include/bee), its runtime archive (/usr/lib/bee/libbee_runtime.a) and a
# C++ compiler on the user's machine -- not this build tree's paths.
make -C "$ROOT" VERSION="$VERSION" \
     AOT_INCDIR=/usr/include/bee \
     AOT_RUNTIME_LIB=/usr/lib/bee/libbee_runtime.a \
     AOT_CXX=c++

# The JIT now lives in libbee_jit.so, dlopen'd on first compile -- `bee` itself
# links no LLVM. Fail loudly if the backend did not get built, and confirm it is
# the piece that carries libLLVM.
if [ ! -f "$ROOT/libbee_jit.so" ] || ! ldd "$ROOT/libbee_jit.so" | grep -qi 'libLLVM'; then
    echo "[deb] error: libbee_jit.so was not built (no LLVM JIT)." >&2
    echo "       Install LLVM dev headers first, e.g.: sudo apt install llvm-18-dev" >&2
    exit 1
fi
cp "$ROOT/bee" "$ROOT/dist/bee"
strip "$ROOT/dist/bee"
# The JIT backend. bee finds it via /usr/lib/bee (its own dir's ../lib/bee).
cp "$ROOT/libbee_jit.so" "$ROOT/dist/libbee_jit.so"
strip "$ROOT/dist/libbee_jit.so"
# hive (the package manager) links no LLVM -- it just rides along.
cp "$ROOT/hive" "$ROOT/dist/hive"
strip "$ROOT/dist/hive"
# beegen (the binding generator) finds libclang at run time, so it adds no
# package dependency: without libclang installed it simply says so.
cp "$ROOT/beegen" "$ROOT/dist/beegen"
strip "$ROOT/dist/beegen"
# beec (the AOT compiler) shells out to a C++ compiler at run time and links the
# program against libbee_runtime.a. Both ride along; the headers it needs are the
# same ones installed under /usr/include/bee for native modules.
cp "$ROOT/beec" "$ROOT/dist/beec"
strip "$ROOT/dist/beec"
cp "$ROOT/libbee_runtime.a" "$ROOT/dist/libbee_runtime.a"

# Auto-detect the runtime packages the JIT backend links against, so Depends is
# right on whatever LLVM version this machine has (e.g. libllvm18).
DEPS="libc6, libstdc++6"
LLVM_SO="$(ldd "$ROOT/dist/libbee_jit.so" | awk '/libLLVM/{print $3; exit}')"
LLVM_PKG="$(dpkg -S "$(readlink -f "$LLVM_SO")" 2>/dev/null | cut -d: -f1 | head -n1)"
if [ -n "$LLVM_PKG" ]; then
    DEPS="$DEPS, $LLVM_PKG"
    echo "[deb] JIT runtime dependency: $LLVM_PKG"
fi

echo "[deb] staging file tree at $STAGE ..."
rm -rf "$STAGE"
install -Dm0755 "$ROOT/dist/bee"                 "$STAGE/usr/bin/bee"
install -Dm0755 "$ROOT/dist/libbee_jit.so"       "$STAGE/usr/lib/bee/libbee_jit.so"
install -Dm0755 "$ROOT/dist/hive"                "$STAGE/usr/bin/hive"
install -Dm0755 "$ROOT/dist/beegen"              "$STAGE/usr/bin/beegen"
install -Dm0755 "$ROOT/dist/beec"                "$STAGE/usr/bin/beec"
# The runtime archive beec links AOT-compiled programs against (paths baked into
# beec above point here).
install -Dm0644 "$ROOT/dist/libbee_runtime.a"    "$STAGE/usr/lib/bee/libbee_runtime.a"
for f in "$ROOT"/examples/*.bee; do
    install -Dm0644 "$f" "$STAGE/usr/share/bee/examples/$(basename "$f")"
done
install -Dm0644 "$ROOT/README.md"                "$STAGE/usr/share/doc/bee/README.md"
# Native modules are compiled against these headers (see docs/BINDINGS.md).
# Both extensions: bee_buffer.h is a plain C header, and bee_native.hpp includes
# it, so shipping only *.hpp breaks every native module built against an
# installed Bee.
for h in "$ROOT"/src/*.hpp "$ROOT"/src/*.h; do
    [ -e "$h" ] || continue
    install -Dm0644 "$h" "$STAGE/usr/include/bee/$(basename "$h")"
done

# Fail the build rather than ship headers that can't compile a native module.
for required in bee_native.hpp bee_buffer.h interpreter.hpp value.hpp bee_aot.hpp; do
    if [ ! -f "$STAGE/usr/include/bee/$required" ]; then
        echo "[deb] error: $required missing from the header set" >&2
        exit 1
    fi
done

# copyright (Debian expects one)
cat > "$STAGE/usr/share/doc/bee/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Bee
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
Homepage: https://github.com/playatanu/bee
Depends: $DEPS
Installed-Size: $INSTALLED_KB
Description: Bee - a small dynamically-typed scripting language
 Bee (the "bee" language) has first-class functions and closures, classes
 with single inheritance, lists and dicts, a module system, and a standard
 library. Hot numeric functions are compiled to native code by a built-in LLVM
 JIT. Run a program with: bee script.bee
 .
 Example programs are installed under /usr/share/bee/examples.
 .
 The package also installs "hive", the Bee package manager:
 hive install <package> fetches a package into ./hive_modules so that
 "import <package>" finds it.
 .
 And "beegen", which generates Bee bindings from C++ headers. It needs a
 libclang shared library at run time (install libclang-dev to use it).
EOF

echo "[deb] building package ..."
fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB" >/dev/null
rm -rf "$STAGE"

echo "[deb] done: $DEB"
dpkg-deb --info "$DEB" | sed 's/^/       /'
