#pragma once
#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// Engine-side experiments, each located by fingerprint and switchable at runtime so
// they can be A/B tested within one session.
namespace tscfix::experiments {

// Shadow map refresh: when active, the engine's per-frame shadow map update runs only
// every `every`-th frame; the frames in between reuse the previous shadow map.
bool InstallShadowSkip(HMODULE engine, int every);
void SetShadowSkip(bool active);

// Staggered shadow cascades: when active, the nearest cascade is drawn every frame and
// the outer ones in turn. Matrix location/stride/count come from local configuration.
bool InstallCascadeStagger(HMODULE engine, int matrixOffset, int matrixStride, int cascades);
void SetCascadeStagger(bool active);
// Runs the shadow update on a worker thread (game thread waits). Needs the cascade hook.
bool InstallShadowWorker();
void SetShadowWorker(bool active);
// Split variant: the near cascade on even frames, all outer cascades on odd frames, so
// each cascade refreshes every other frame with an evened-out cost per frame.
void SetShadowSplit(bool active);

// Coarser level of detail for shadow casters: raises the engine's global LOD bias by `bias`
// while the shadow update runs. Needs the cascade hook (InstallCascadeStagger) first;
// the bias field offset comes from local configuration.
bool InstallShadowLod(HMODULE engine, int biasOffset);
void SetShadowLod(bool active, int bias);

// Speed limits for the player's train, published in the telemetry block. playerCaller
// (local configuration) selects the call site that serves the player; 0 = probe only.
bool InstallSpeedLimits(HMODULE engine, uintptr_t playerCaller);

// Probe: per-frame count and timing of scenery render calls, by caller and pass
// argument (logged every 300 frames). Local research only; output names engine offsets.
bool InstallSceneryProbe(HMODULE engine);
void OnFrame();  // game thread, once per frame
bool Tracing();  // true while the one-frame shadow update trace is running
// Research builds: single-step trace one shadow cascade's scenery call at a given shadow frame.
void SetResearchTrace(int cascade, int frame);

// Device wrapper hooks for staggered cascades: a full-viewport Clear inside the shadow
// update may be held back (returns true = do not execute now); flushed before any
// render-target/viewport/depth change and at Present.
bool HoldShadowClear(IDirect3DDevice9* device, DWORD rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
void FlushShadowClear();

// Shadow maps on two threads ([threading] shadow_threads=2, with the shadow pass on its worker):
// the widest of the sun's shadow maps is drawn by a second thread beside the others. Each thread
// runs the engine's own shadow-map loop with the other's maps switched off; what that loop still
// does for a map that is not the thread's (clear, the cab's draw) is dropped at the device.
bool InstallMapSplit(HMODULE engine, int map);
void SetMapSplit(bool on);
extern thread_local const int* t_shadowMapIndex;  // the loop's current map minus one, in this thread's manager
extern thread_local uint32_t t_shadowMaps;         // bit per map this thread draws
inline bool NotMyShadowMap() {
    return t_shadowMapIndex && !((t_shadowMaps >> (*t_shadowMapIndex + 1)) & 1);
}

}  // namespace tscfix::experiments
