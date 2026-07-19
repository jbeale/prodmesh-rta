#!/bin/bash
# Builds ProdMesh Remote RTA on macOS. One-time: chmod +x build.sh
# Installs qt/cmake/ninja via Homebrew if they are missing.
set -e
cd "$(dirname "$0")"

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required — install it from https://brew.sh then re-run."
    exit 1
fi

for pkg in qt cmake ninja; do
    if ! brew list "$pkg" >/dev/null 2>&1; then
        echo "Installing $pkg..."
        brew install "$pkg"
    fi
done

QT_PREFIX="$(brew --prefix qt)"
echo "Using Qt at $QT_PREFIX"

cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Bundle the Qt frameworks into the .app so it runs on any Mac
"$QT_PREFIX/bin/macdeployqt" build/ProdMeshRemoteRTA.app

echo ""
echo "Build complete: build/ProdMeshRemoteRTA.app"
echo "Run it with:  open build/ProdMeshRemoteRTA.app"
echo "(First launch triggers the macOS microphone permission prompt.)"
