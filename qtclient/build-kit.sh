#!/usr/bin/env bash
# Build one qtclient Qt kit from this tree and install it.
#
#   qtclient/build-kit.sh <kit> <prefix> [<host-prefix>]
#
# <kit>: macos-arm64 | macos-x64 | linux-x64 | linux-arm64 | windows-x64 |
#        wasm_multithread (needs <host-prefix>: a desktop kit of this tree, and
#        emsdk from qtclient/emsdk-version on PATH).
# The wasm kit gets qtclient/emsdk-version copied to <prefix>/emsdk-version, so
# consumers build with the exact Emscripten it was built with.
#
# Portable to macOS /bin/bash 3.2 (no arrays in locals).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
KIT="${1:?usage: $0 <kit> <prefix> [<host-prefix>]}"
PREFIX="${2:?usage: $0 <kit> <prefix> [<host-prefix>]}"
HOST_PREFIX="${3:-}"
BUILD="${QT_BUILD_DIR:-$PREFIX.build}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# Modules the qtclient engines do not use. Keep in sync with find_package(Qt6 …)
# in qtclient (client/tibia, launcher). qtmultimedia: unused, and a host FFmpeg
# (often x86_64 under /usr/local on Apple Silicon) breaks the arm64 link.
SKIP_MODULES="qtwebengine qtwebview qtmultimedia qt3d qtcharts qtdatavis3d qtgraphs qtquick3d
qtquick3dphysics qtquickeffectmaker qtquicktimeline qtlottie qtlocation qtpositioning qtsensors
qtconnectivity qtserialbus qtserialport qtcoap qtmqtt qtopcua qtgrpc qthttpserver qtremoteobjects
qtscxml qtspeech qtvirtualkeyboard qtwayland qtactiveqt qttools qtdoc qttranslations qtqa
qtrepotools"
SKIP_ARGS=""
for m in $SKIP_MODULES; do SKIP_ARGS="$SKIP_ARGS -skip $m"; done

native_path() {
  # configure.bat / cmake need D:/… — MSYS /d/… would become a literal D:\d\…
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi
}

# GitHub windows runners + Git Bash put MinGW ahead of MSVC; CMake then picks
# GNU and qsb.exe dies at runtime (0xC0000005 / 0xC0000139). Use cl.exe.
PLATFORM_ARGS=""
CMAKE_EXTRA=""
case "$KIT" in
  windows-*)
    cleaned=""
    oifs="$IFS"
    IFS=':'
    # shellcheck disable=SC2086
    for part in $PATH; do
      case "$part" in *[Mm]ingw*) continue ;; esac
      cleaned="${cleaned:+$cleaned:}$part"
    done
    IFS="$oifs"
    export PATH="$cleaned"
    command -v cl >/dev/null 2>&1 || command -v cl.exe >/dev/null 2>&1 || {
      echo "[kit] Windows: cl.exe not on PATH (VS Build Tools / ilammy/msvc-dev-cmd)" >&2
      exit 1
    }
    PLATFORM_ARGS="-platform win32-msvc"
    CMAKE_EXTRA="-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl"
    ;;
  wasm_multithread)
    [[ -n "$HOST_PREFIX" ]] || { echo "[kit] wasm needs <host-prefix>" >&2; exit 1; }
    command -v emcc >/dev/null || { echo "[kit] wasm needs emcc on PATH" >&2; exit 1; }
    want="$(cat "$HERE/emsdk-version")"
    have="$(emcc --version | head -1 | sed -E 's/.* ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
    [[ "$have" == "$want" ]] || { echo "[kit] emcc $have, qtclient/emsdk-version wants $want" >&2; exit 1; }
    # JSPI + native wasm EH: qtclient's /play file system suspends main (JSPI).
    PLATFORM_ARGS="-platform wasm-emscripten -qt-host-path $(native_path "$HOST_PREFIX")
      -feature-thread -feature-wasm-exceptions -feature-wasm-jspi"
    ;;
esac

CONFIGURE="$SRC/configure"
[[ -x "$CONFIGURE" ]] || CONFIGURE="$SRC/configure.bat"

mkdir -p "$PREFIX" "$BUILD"
echo "[kit] $KIT: configuring $(native_path "$PREFIX")"
(
  cd "$BUILD"
  if [[ ! -f CMakeCache.txt ]]; then
    # Intentional word-split of the argument strings (bash 3.2-safe, no arrays).
    # shellcheck disable=SC2086
    "$CONFIGURE" \
      -prefix "$(native_path "$PREFIX")" \
      -release -nomake examples -nomake tests -opensource -confirm-license \
      $PLATFORM_ARGS $SKIP_ARGS \
      -- -DCMAKE_BUILD_TYPE=Release $CMAKE_EXTRA
  fi
  cmake --build . --parallel "$JOBS"
  cmake --install .
)
[[ -f "$PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]] || {
  echo "[kit] $KIT: install failed (no Qt6Config.cmake under $PREFIX)" >&2
  exit 1
}
[[ "$KIT" == wasm_multithread ]] && cp "$HERE/emsdk-version" "$PREFIX/emsdk-version"
echo "[kit] $KIT: installed $PREFIX"
