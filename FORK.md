# Cade fork of Visual Pinball

This repository is a **fork of [vpinball/vpinball](https://github.com/vpinball/vpinball)**.
It is not an official Visual Pinball release. It tracks upstream `master` and is
rebased on it regularly.

## What this fork adds

1. **Game element event plugin API** — lets VPX plugins observe game element
   events (bumper hits, target drops, spinner spins, …) and actuate elements
   (eject/destroy/kick ball, light/drop-target/flipper state).
2. **Cade Bridge plugin** (`plugins/cade-bridge/`) — VPX hosts a gRPC
   `PlatformService` on `0.0.0.0:50052`; the **Cade runtime** ([cade.run](https://cade.run))
   connects to it as a client. See `plugins/cade-bridge/PROTOCOL.md`.

## License

Same as upstream: code marked `// license:GPLv3+` is **GPLv3-or-later**; other
files retain their upstream license (see `LICENSE` and `docs/license.txt`).
**All additions in this fork are GPLv3-or-later** and carry the `// license:GPLv3+`
marker. This fork is non-commercial.

The **Cade runtime is a separate program** that communicates with the plugin over
gRPC/TCP (it is not linked into the VPX binary). The cade-bridge **plugin** is
linked into VPX and is therefore GPLv3+, like the rest of VPX.

## Changes vs. upstream (GPLv3 §5 notice)

New files:
- `plugins/cade-bridge/**` (the plugin, proto, build files)
- `src/core/VPXGameElementBridge.h`
- `make/CMakeLists_plugin_CadeBridge.txt`, `make/plugin-cade-bridge.vcxproj`

Modified upstream files:
- `src/core/ieditable.h` — one-line `FireGroupEvent` hook into the bridge
- `src/core/VPXPluginAPIImpl.{h,cpp}` — game element event broadcast + plugin API verbs
- `src/parts/kicker.{h,cpp}` — kick-only kicker primitive
- `src/core/player.cpp` — log the OS error when a plugin DLL fails to load
- `plugins/plugins/VPXPlugin.h` — game element event + verb declarations
- `make/CMakeLists_plugins.txt`, `.gitignore` — register the plugin / ignore generated proto
