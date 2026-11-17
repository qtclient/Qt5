# qtclient Qt

This repository is [CipSoft-Tibia/Qt5](https://github.com/CipSoft-Tibia/Qt5)
(CipSoft's patched Qt 6.10.3) plus the changes qtclient needs. `master` is what
qtclient builds against: qtclient pins a commit of it as `third_party/qt5-cip`
and pulls the matching prebuilt kits.

## Changes on top of CipSoft

- **JSPI-safe `QNetworkReply` and `QWebSocket` on wasm** (`qtbase/src/network/access/qnetworkreplywasmimpl*`,
  `qtwebsockets/src/websockets/qwebsocket_wasm_p.cpp`, `qtbase/cmake/QtWasmHelpers.cmake`).
  Both use the browser APIs (`fetch()`, `WebSocket`) with every event delivered
  through `QWasmSuspendResumeControl`, like Qt's own DOM events. Emscripten's
  fetch / websocket libraries call into wasm straight from the browser event,
  which under JSPI runs on the stack of a suspended main thread. Also: no leaked
  response bodies (the old reply never closed its `emscripten_fetch_t`), real
  streamed download progress, and Qt no longer links `-sFETCH` / `websocket.js`.
- **`qtclient/`**: kit build and publish scripts and the Emscripten pin.

Keep each change a separate commit, so rebasing onto a new CipSoft Qt is a plain
`git rebase`.

## Kits

Every push to `master` runs **qtclient kits** (`.github/workflows/qtclient-kits.yml`):
`macos-arm64`, `linux-x64`, `windows-x64` and `wasm_multithread` (JSPI + native
wasm exceptions, Emscripten from `qtclient/emsdk-version`), published as

- `ghcr.io/qtclient/qt:<sha12>-<kit>` (OCI artifact, `qt-prefix.tar.zst`), and
- GitHub Release `qt-<sha12>` here, asset `qt-<sha12>-<kit>.tar.zst`.

A kit already published for the commit is not rebuilt.

Local build of one kit:

```bash
qtclient/build-kit.sh macos-arm64 ~/Qt/cip-$(git rev-parse --short=12 HEAD)/macos
# wasm: emsdk at qtclient/emsdk-version on PATH, plus a desktop kit of this commit as host
qtclient/build-kit.sh wasm_multithread ~/Qt/cip-<sha12>/wasm_multithread ~/Qt/cip-<sha12>/macos
```

The install layout `~/Qt/cip-<sha12>/<macos|gcc_64|msvc2022_64|wasm_multithread>` is
what qtclient's `make qt` expects.

## Updating to a new CipSoft Qt

```bash
git fetch https://github.com/CipSoft-Tibia/Qt5.git master
git rebase FETCH_HEAD          # replay the qtclient commits
git push --force-with-lease origin master
```

Then bump `third_party/qt5-cip` in qtclient once the kits are published.
