#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$ROOT_DIR/deps"
SCENEFX_SRC="$DEPS_DIR/scenefx"
SCENEFX_BUILD="$SCENEFX_SRC/build"
LOCAL_PREFIX="$DEPS_DIR/local"

export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig:$LOCAL_PREFIX/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:$HOME/.local/lib/pkgconfig:/home/user/AI/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "==> Checking build tools..."
missing_tools=()
for cmd in git pkg-config meson ninja gcc; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        missing_tools+=("$cmd")
    fi
done

if [ ${#missing_tools[@]} -gt 0 ]; then
    echo "Error: missing required build tools: ${missing_tools[*]}"
    echo "Install them first, e.g.:"
    echo "  Arch:   sudo pacman -S git pkgconf meson ninja gcc"
    echo "  Fedora: sudo dnf install git pkgconf-pkg-config meson ninja-build gcc"
    echo "  Debian: sudo apt install git pkg-config meson ninja-build build-essential"
    exit 1
fi

echo "==> Checking wlroots-0.20..."
if ! pkg-config --exists wlroots-0.20; then
    echo "Error: wlroots-0.20 development files were not found by pkg-config."
    echo "Install wlroots 0.20+ first:"
    echo "  Arch:   sudo pacman -S wlroots0.20"
    echo "  Fedora: sudo dnf install wlroots-devel"
    echo "  Debian/Ubuntu: install libwlroots-0.20-dev (or build wlroots 0.20 from source)"
    exit 1
fi

if pkg-config --exists scenefx-0.5; then
    echo "==> scenefx-0.5 is already available ($(pkg-config --modversion scenefx-0.5))."
    exit 0
fi

echo "==> Building scenefx-0.5 into $LOCAL_PREFIX ..."
mkdir -p "$DEPS_DIR"

if [ ! -d "$SCENEFX_SRC/.git" ]; then
    rm -rf "$SCENEFX_SRC"
    if ! git clone --branch 0.5.0 --depth 1 https://github.com/wlrfx/scenefx.git "$SCENEFX_SRC" 2>/dev/null; then
        git clone --depth 1 https://github.com/wlrfx/scenefx.git "$SCENEFX_SRC"
    fi
fi

rm -rf "$SCENEFX_BUILD"
meson setup "$SCENEFX_BUILD" "$SCENEFX_SRC" \
    --prefix="$LOCAL_PREFIX" \
    --libdir=lib \
    --buildtype=release \
    -Dexamples=false

ninja -C "$SCENEFX_BUILD"
ninja -C "$SCENEFX_BUILD" install

if pkg-config --exists scenefx-0.5; then
    echo "==> scenefx-0.5 installed successfully to $LOCAL_PREFIX!"
    echo "You can now run: make -j\$(nproc)"
else
    echo "Error: scenefx-0.5 installation succeeded but pkg-config still cannot find it."
    exit 1
fi
