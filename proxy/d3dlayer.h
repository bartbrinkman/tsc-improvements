#pragma once
#include <windows.h>
#include <d3d9.h>

// Direct3D 9 / D3DX Effect layer. Captures the game's device and effect interface
// through Microsoft's public APIs only, and offers:
//   census            - per-frame call counts (feasibility data for a render thread)
//   effectNoSaveState - begin effects with D3DXFX_DONOTSAVESTATE (skip state save/restore)
//   stateFilter       - drop device calls that set state to the value it already has
#include "sites.h"

namespace tscfix::d3d {

struct Options {
    bool census = false;
    bool effectNoSaveState = false;
    bool stateFilter = false;
    int renderThread = 0;  // rt::Mode: 0 off, 1 pass-through wrapper, 2 threaded
    // Threaded mode: create the device with D3DCREATE_MULTITHREADED. Without it the
    // wrapper must serialize every direct runtime access itself (it drains first).
    bool runtimeLock = true;
    bool force9On12 = false;  // diagnostic: create the DX9 build's device through D3D9On12
    bool default9On12Args = false;  // diagnostic: drop the caller's D3D9On12 overrides
    int swapEffect = 0;             // diagnostic: override the swap effect at CreateDevice (0 = keep)
    bool crashLog = false;          // diagnostic: also log the capability checks before CreateDevice
    int lodFade = 0;                // [experiments] lod_fade: add the fade discard to shader-model-3 pixel shaders (psfade.h)
    bool queryTrack = false;        // [experiments] query_track: log occlusion reads (not ready, one-read drops to zero)
    bool samplerProbe = false;      // [experiments] sampler_probe: count the filter settings the game puts on samplers
    int lodFadeTest = 0;            // [experiments] lod_fade_test: percent of the pixels every patched shader discards (test)
};

bool Install(const Options& options);  // call once d3d9.dll is loaded, before device creation
// Render thread: make LockRect on this surface's class drain recorded work first.
void HookSurface(IDirect3DSurface9* surface);
// The device the engine draws with (calls on it are recorded in order when the render thread is on).
IDirect3DDevice9* GameDevice();
void ResetCensus();
// A/B switches for the installed fixes (game thread, between frames).
void SetFilterActive(bool on);
void SetNoSaveStateActive(bool on);
// Render thread: locks of write-only vertex / index buffers are recorded instead of draining.
void SetDeferredLocks(bool on);
// NOOVERWRITE buffer locks without draining the render thread (the app promises not to touch
// data that queued draws use). Only while deferred locks are off.
void SetDirectNoOverwrite(bool on);
// Game thread, frame boundary: the overlap is on / off (its lock paths only run while on).
void SetLockPathsActive(bool on);
// A lock call site whose code was read: `returnOffset` is the return address of the Lock call
// relative to `module`'s base.
// WriteOnlyRange: the caller writes the whole range it locks and reads nothing back.
// LockWrapper: a generic lock helper (its callers decide what is written); WriteOnlyViaWrapper:
// a call site of such a helper that only fills (e.g. a buffer it has just created).
enum class LockSiteKind { WriteOnlyRange, CpuOnlyTexture, LockWrapper, WriteOnlyViaWrapper };
void AddVerifiedLockSite(const char* module, uintptr_t returnOffset, LockSiteKind kind);
// The same, located by fingerprint: `returnOffset` is relative to the start of the function
// `function` identifies in `module` (resolved when the module is first seen).
void AddVerifiedLockSite(const char* module, const sites::Fingerprint& function, uintptr_t returnOffset,
                         LockSiteKind kind);
void WriteCensus(const char* path);

// Frame counts since the last call (for the health line in the log).
void ReadFrameHealth(long* frames, long* over50, long* over100, long* worstMs, long* activeMs);

}  // namespace tscfix::d3d
