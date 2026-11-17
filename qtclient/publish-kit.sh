#!/usr/bin/env bash
# Publish an installed kit of this tree:
#   ghcr.io/qtclient/qt5:<sha12>-<kit>          (OCI artifact, qt-prefix.tar.zst)
#   GitHub Release qt-<sha12>, asset qt-<sha12>-<kit>.tar.zst on this repository
#
#   qtclient/publish-kit.sh <kit> <prefix>
#
# <sha12> is this tree's commit: qtclient pins it as third_party/qt5-cip and
# pulls exactly that kit (qtclient tools/qt). Needs oras, zstd, gh; logged in to
# ghcr.io; GH_TOKEN with contents:write.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KIT="${1:?usage: $0 <kit> <prefix>}"
PREFIX="${2:?usage: $0 <kit> <prefix>}"
IMAGE="${QT_GHCR_IMAGE:-ghcr.io/qtclient/qt5}"
SHORT="$(git -C "$HERE" rev-parse --short=12 HEAD)"
TAG="${SHORT}-${KIT}"

[[ -f "$PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]] || { echo "[kit] nothing installed at $PREFIX" >&2; exit 1; }
WORK=".kit-publish-$$"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "[kit] packing $PREFIX"
tar -C "$PREFIX" -cf - . | zstd -T0 -3 -o "$WORK/qt-prefix.tar.zst"

echo "[kit] pushing $IMAGE:$TAG"
(cd "$WORK" && oras push "$IMAGE:$TAG" \
  --artifact-type application/vnd.qtclient.qt.prefix.v1+tar \
  "qt-prefix.tar.zst:application/vnd.oci.image.layer.v1.tar+zstd")

# Public mirror for consumers that cannot read GHCR packages. Must be a
# published (non-draft) release: drafts serve untagged-… download URLs.
REL="qt-${SHORT}"
ASSET="$WORK/qt-${TAG}.tar.zst"
mv "$WORK/qt-prefix.tar.zst" "$ASSET"
gh release view "$REL" >/dev/null 2>&1 \
  || gh release create "$REL" --target "$(git -C "$HERE" rev-parse HEAD)" --title "$REL" \
       --notes "qtclient Qt kits for $(git -C "$HERE" rev-parse HEAD)."
gh release edit "$REL" --draft=false >/dev/null
gh release upload "$REL" "$ASSET" --clobber
echo "[kit] published $IMAGE:$TAG and release $REL"
