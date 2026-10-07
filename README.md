# tcoaal-switch

An unofficial port of *The Coffin of Andy and Leyley* to the Nintendo Switch, as homebrew.

This repository contains **no game content**. It contains tools that take a copy of the game
you own (the Steam/PC release, tested with version 3.0.11) and build a Switch `.nro` or `.nsp`
from it. Keep the build outputs to yourself: they contain your decrypted game data.

## How it works

The PC game is an RPG Maker MV project running on NW.js (Chromium plus Node.js). The port
replaces NW.js with a small native runtime:

| Layer | Implementation |
|---|---|
| JavaScript | [QuickJS-ng](https://github.com/quickjs-ng/quickjs) (interpreter). All scripts are precompiled to bytecode at build time. |
| WebGL (Pixi.js) | WebGL 1 bound directly to OpenGL ES 2 (Mesa/nouveau on Switch) — `runtime/src/gl.c` |
| Canvas 2D (bitmaps, text) | [plutovg](https://github.com/sammycage/plutovg) + stb_truetype — `runtime/src/canvas.c` |
| Web Audio | Custom mixer on SDL2 audio, Vorbis decoding on worker threads via stb_vorbis — `runtime/src/audio.c` |
| Browser/DOM, XHR/fetch, fonts, localStorage | JS shims — `runtime/js/` |
| Node/NW.js (`fs`, `path`, `crypto`, `nw.gui`, …) | JS shims over a virtual filesystem — `runtime/js/node.js` |
| Input | SDL2 game controllers exposed as standard gamepads; DOM key events on the host build |

The game's assets are encrypted and its custom engine logic is hidden inside several plugin
files. Both are handled at build time (`tools/build_romfs.py`):

* **Asset format:** `"TCOAAL"` + 1-byte length + payload. Only the first *n* bytes are encrypted
  (n = 0 means the whole file), using an autokey XOR seeded from the hashed file name. Full
  details are in `tools/README.md`.
* **Hidden core script:** 7 base64 chunks spread across plugins, joined, then zlib-inflated. It is
  extracted to `js/plugins/_tcoaal_core.js` and patched so that the game neither decrypts again
  nor requires Steam (`BUILD_FLAGS`).
* The asm.js Vorbis decoder plugins are replaced by the runtime's native `stbvorbis` decoder.

Saves, settings and `runtime.log` are written to `sdmc:/switch/tcoaal/` on the Switch (both the
NRO and NSP builds use it). On the host build they go to `~/.local/share/tcoaal-switch/`.

## Building

Requirements:

* devkitPro with devkitA64, libnx and the switch portlibs: `switch-sdl2`, `switch-mesa`, `switch-zlib`
* A host C toolchain, CMake, SDL2, zlib and OpenGL ES (for the host runtime and the bytecode compiler)
* Python 3, plus Pillow or ImageMagick for the icon, and `curl`
* For `.nsp` only: `hacbrewpack` and your own `prod.keys`

```sh
git submodule update --init
export DEVKITPRO=/opt/devkitpro

make GAME="/path/to/The Coffin of Andy and Leyley"   # -> build/tcoaal.nro
make nsp                                            # -> build/tcoaal.nsp (KEYS=/path/to/prod.keys)
make run                                            # run build/romfs with the host (Linux) runtime
```

`GAME` may be the game folder or its `www/` folder. If you've installed mods, point it at
unmodded files. The NRO icon is downloaded at build time (override with `ICON_URL=...`, or set
`ICON_URL=` to use the default libnx icon).

### Debugging

`runtime.log` collects the runtime and game logs. Optional debug settings, read from the
environment (host) or `<save dir>/debug.cfg` (one `KEY=value` per line):

| Key | Effect |
|---|---|
| `TCOAAL_STATS=1` | Log FPS, average/worst frame time, JS time and heap every 5 s |
| `TCOAAL_SHOTS=120:/path/a.png,600:/path/b.png` | Screenshot after the given frames. This can upset yuzu's GPU emulation. |
| `TCOAAL_KEYS=300:13,420:27` | Tap a DOM keyCode at the given frames |
| `TCOAAL_EXIT=900` | Quit after N frames |

**Remove `debug.cfg` before playing normally:** with `TCOAAL_KEYS`/`TCOAAL_EXIT` set, the game
presses keys by itself and quits.

## Status

Working (host build, NRO and NSP in yuzu, and on a real Switch):

* Asset decryption, hidden-script extraction, and bytecode compilation of all scripts
* Boot, title screen, New Game, intro cutscenes, message windows with name boxes and coloured text
* Character portraits on the correct side, with dialogue text aligned correctly (needed the legacy
  `RegExp.$1`… statics, which QuickJS lacks; shimmed in `runtime/js/core.js`)
* Saving and loading of settings and global save data
* Audio: background music, sound effects and fades, with Vorbis decoding on worker threads
* Gamepad input. A/B and X/Y are swapped on the Switch so that Nintendo A confirms and B cancels.
* NRO with a custom icon; NSP (title ID `0100C0FF1E5A0000`) with an 8 MB main-thread stack

### Known issues / TODO

* **Performance is really bad** (confirmed on real hardware). The JS interpreter takes about
  10 ms per frame in yuzu, versus about 1.5 ms on a desktop PC. Ideas:
  * profile hot paths (Pixi's sprite batching, the tilemap, window/bitmap redraws)
  * move image decoding off the main thread (it currently happens synchronously when images load)
  * avoid repeated full-texture uploads of dirty canvases
  * check `-O3`/LTO, and the CPU boost mode during play
* **Touchscreen is not supported.** Mouse/touch events are not forwarded yet.
* **Button mappings are not fully verified.** A/B were confirmed swapped and fixed. X/Y were
  swapped along with them (the game's dash/menu actions) but haven't been checked in game. The
  shoulder buttons, triggers and +/− also haven't been checked.
* **The controls help at the start of the game still describes the PC/keyboard controls.** It
  should show Switch buttons.
* **Unsupported canvas blend modes:** `difference`, `saturation` and `multiply` fall back to
  normal drawing. They appear to be used only by RPG Maker's non-WebGL fallback paths and its
  capability tests. `lighter` (used for the menu background blur) is implemented.
* The NRO build gets whatever main-thread stack hbloader provides, while the NSP gets 8 MB. If the
  NRO crashes on deep JS recursion, use the NSP.

### Notes for NSP builds

`runtime/npdm.json` is based on nx-hbloader's `hbl.json`. It must **not** contain a
`debug_flags` capability: yuzu then fails to load the title with error `0007-0020`
("Unable to completely parse the kernel metadata").
