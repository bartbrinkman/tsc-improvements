# Technical description

This document is for developers. For installation, see the [README](../README.md).

## Overview

The mod is a proxy `version.dll`. The game loads `version.dll` from its own folder before the Windows copy. The proxy forwards every export to `C:\Windows\System32\version.dll` (`proxy/exports.cpp`), so the game sees no difference. When the proxy loads, it reads `tscfix.ini` and installs its changes in the game's process memory. No file of the game is changed.

Supported engine modules:

- `GameManagerVC64.dll` (DirectX 9 build, `RailWorks64.exe`). This is the tested build.
- `GameManagerDX12_VC64.dll` (DirectX 12 build). The proxy finds it, but it is not tested in the same way.

## Finding engine code

The source contains no addresses and no bytes of the game. A location in the engine is a one-way fingerprint:

- FNV-1a 64 over the first N bytes of a function from the module's `.pdata` table. N grows (16 to 128) until the hash is unique among all functions of the module.
- Optional: an offset into that function, or an instruction there that addresses a target RIP-relative (a data item or a call target).
- Or: a virtual table slot of a class, found by its run-time type name (`proxy/rtti.cpp`).

`proxy/sites.cpp` scans the module once at start-up and resolves all sites (`sites::ResolveAll`). `proxy/engine_sites.h` is the table. It is made by `tools/sitegen.py` from a local list of names and addresses that is not in this repository. `tools/fingerprint.py` makes a single fingerprint.

A site that does not resolve (another game version) switches off only the change that needs it. The log says which. The render threading needs all its sites; if one is missing, it is not installed at all.

Field offsets inside engine objects are in the source. They tell where the mod reads or writes, not what the game's code is. See [IP-POLICY.md](IP-POLICY.md).

Hooks use MinHook (`third_party/minhook`).

## Render threading (`[threading] render=1`)

The stock engine runs the update and the render of a frame on one thread. Direct3D calls go to its own render thread.

With `render=1` (preset in `proxy/config.h`):

1. The game update runs first, alone, as in the stock engine.
2. Then the render of the frame is split. The shadow pass runs on a worker thread beside the main view (`proxy/overlap.cpp`). With `shadow_threads=2`, the widest shadow map has a second shadow thread.
3. Device calls from all threads are recorded and replayed in order on one render thread (`proxy/rtdevice.cpp`, `proxy/rteffect.cpp`). The wrapper is generated at build time from the SDK's `d3d9.h` (`tools/gen_d3d9_wrapper.py`).

While the render runs, nothing changes the world. So the render threads only need to stay clear of each other. Shared engine state that each pass writes has a copy per thread:

- `proxy/binder.cpp`: the engine's global draw binder. Every instruction that addresses it RIP-relative is found at start-up (`proxy/ripscan.cpp`) and redirected to the thread's copy.
- `proxy/overlapstate.cpp`, `proxy/pipeline.cpp`: render-state managers, sorted queues, instancing lists, material overrides, shadow flags.
- `proxy/safecounts.cpp`: atomic user counts for objects that both threads touch.

Measured standing at Haarlem: 26.7 ms per frame, stock 39.0 ms. Results between game launches differ by up to 10%.

## Fixes (`[fixes]`)

| Key | Default | What it does | Source |
|---|---|---|---|
| `multicore` | 1 | Starts the engine's job threads, as the `-Multicore` option does. | `main.cpp` |
| `brake_hold` | 1 | A bogie that the game's own brake law holds (no traction, brake on, brake stronger than the gradient) keeps its pose with zero velocity after 0.5 s. When all bogies of a consist are held, its car bodies are kept too. Without it, PhysX lets a braked standing train creep by millimetres, and the camera moves with it. | `physicsfix.cpp` |
| `engine_heap_budget` | 1 | The engine heap's budget is the machine's commit limit, not 2 GB. Small fixed heaps get 16 times their budget (at most 1 GB). The sound system heap gets 512 MB instead of 100 MB. | `main.cpp` |
| `physics_index` | 1 | Removing a collideable no longer walks a list of all collideables (stutter when tiles unload). | `physicsfix.cpp` |
| `entity_search_cache` | 1 | Script calls to `child:function` and `*:function` find their targets from a cache. The cache is cleared when any entity gains or loses a child. | `entitysearch.cpp` |
| `node_set_skip` | 1 | An `ActivateNode` script call that changes nothing is skipped. Needs `entity_search_cache`. | `nodeskip.cpp` |
| `script_call_cache` | 1 | The script `Call()` binding's lookup of its own entity is cached per Lua state. | `scriptcall.cpp` |
| `qi_cache` | 1 | One interface query of entity containers is cached per thread. | `qicache.cpp` |
| `script_timing` | 1 | Script call timing is skipped while its overlay is off. | `scripttiming.cpp` |
| `rich_presence_async` | 1 | Steam rich presence updates leave the game thread. Unchanged values are dropped. | `richpresence.cpp` |
| `safe_counts` | 1 | Thread-safe user counts for engine objects. | `safecounts.cpp` |
| `override_lock_all` | 1 | Draws with material overrides are kept apart between render threads. | `pipeline.cpp` |
| `effect_nosavestate` | 1 | D3DX effects do not save and restore the full device state. | `d3dlayer.cpp` |
| `state_filter` | 0 | Drops redundant Direct3D state calls. | `d3dlayer.cpp` |
| `fast_function_lookup` | 0 | A script call finds its engine function from an index. | `funclookup.cpp` |
| `fast_node_lookup` | 0 | A model's node is found by name from an index. | `nodelookup.cpp` |
| `fast_strings` | 0 | Faster engine string compare with the same results. | `fastpaths.cpp` |

## Picture (`[graphics]`)

| Key | Default | What it does | Source |
|---|---|---|---|
| `cab_camera_spring` | 0 | 0: the cab camera is fixed to the cab. 1: the game's spring and damping. `cab_camera_spring_percent` and `cab_camera_damping_percent` scale the game's values. | `physicsfix.cpp` |
| `sign_text_depth_bias` | 32 | Sign digits (materials with texture overrides) are drawn this many 24-bit depth steps towards the camera, so they do not z-fight with the plate. `sign_text_slope_bias` (x100, default 100) adds a slope bias. | `pipeline.cpp` |
| `better_resolve` | 1 | The supersampling shrink uses a filter that reads every rendered sample of the pixel and its neighbours (1 Lanczos, 2 Mitchell, 3 box). `better_resolve_linear=1` filters in linear light. | `resolve.cpp` |
| `fxaa` | -1 | The game's FXAA pass: -1 off when the game supersamples, 0 off, 1 on. | `postfx.cpp` |
| `film` | 1 | Bloom and tone: 0 the game's pass, 1 the mod's, 2 the mod's strong one. | `postfx.cpp` |
| `depth_of_field` | 1 | The game's depth of field pass. | `postfx.cpp` |
| `lamp_fade` | 1 | Lamp light and lamp glow get weaker as the scene gets bright, between `lamp_fade_from_percent` (25) and `lamp_fade_to_percent` (70) of scene brightness. | `postfx.cpp` |
| `anisotropy` | 0 | 0 keeps the game's level (4). Else 8 or 16. | `resolve.cpp` |

## Other settings

- `[debug] crash_record=1`: a crash writes the module, stack, registers and memory use to `tscfix.log`. The game's crash message tells what happened in plain words.
- `[debug] crash_log=1`: also logs serious exceptions that the game handles itself.
- `[capture] flicker_key=<virtual key>`: keeps the last `flicker_frames` pictures and writes them to the temp folder `tscfix-flicker` when the key is pressed. Use this to send exact frames of a picture fault.
- `[experiments]`: measurement and diagnosis switches. They are described next to their code. They are off by default and are not meant for play.

## Log

`tscfix.log` in the game folder. Each install step writes one line, for example:

```
research: engine sites resolved 202 of 202
physicsfix: brake hold installed
nodeskip: calls that change nothing are skipped
```

A line with "not found" or "NOT installed" means that a change is switched off, usually after a game update.

## Build

Needs:

- Visual Studio 2022 with the C++ x64 tools.
- Windows 10 SDK (for `d3d9.h`).
- Python 3 on the path.

```powershell
.\proxy\build.ps1
```

The result is `build\version.dll`. The build uses the static C runtime, so the DLL needs no extra files.

To make a new fingerprint table after a game update, you need a local list of `Name address` lines for the new game version:

```powershell
pip install pefile capstone
python -I tools\sitegen.py <path to GameManagerVC64.dll> <names file> proxy\engine_sites.h
```

## Contributing

Run `git config core.hooksPath githooks` after you clone. The pre-commit hook blocks binaries, byte patterns, disassembly and module addresses. Read [IP-POLICY.md](IP-POLICY.md) before you send a change.

## Source map

| File | Part |
|---|---|
| `main.cpp` | Start-up, settings, crash record, memory fixes |
| `config.h` | `tscfix.ini` access and the render preset |
| `exports.cpp` | Forwarded `version.dll` exports |
| `sites.cpp`, `engine_sites.h`, `rtti.cpp`, `ripscan.cpp` | Finding engine code |
| `overlap.cpp`, `overlapstate.cpp`, `pipeline.cpp`, `binder.cpp`, `offload.cpp` | Render threading |
| `rtdevice.cpp`, `rteffect.cpp`, `d3dlayer.cpp` | Direct3D wrapper and replay |
| `resolve.cpp`, `postfx.cpp`, `psfade.cpp` | Picture changes |
| `physicsfix.cpp` | Physics index, brake hold, cab camera |
| `entitysearch.cpp`, `nodeskip.cpp`, `scriptcall.cpp`, `qicache.cpp`, `scripttiming.cpp`, `funclookup.cpp`, `nodelookup.cpp`, `lookup.cpp` | Script call speed-ups |
| `safecounts.cpp`, `cowtable.h` | Thread safety |
| `flickercap.cpp`, `glitch.cpp`, `framelog.cpp`, `profiler.cpp`, `trace.cpp`, `telemetry.cpp`, `ab.cpp` | Measurement and diagnosis |
