#pragma once
// Per-thread clones of engine objects whose per-pass state the
// shadow pass and the main view both write. Engine sites are located by fingerprint
// (engine_sites.h).
#include <windows.h>

#include <cstdint>

namespace tscfix::pipeline {

bool Install(HMODULE engine);
// Game thread, before the worker starts the shadow pass: private per-thread objects for the
// worker, and the worker's binder copy pointed at them.
void PrepareWorker(uint8_t* binderCopy);
// The calling thread starts a shadow pass (per-pass clone storage is reused from here).
void BeginWorkerPass();
// Second shadow thread (some shadow maps drawn beside the others; experiments.cpp).
void SetSplit(bool on);
void SetOverrideLockAll(bool on);
void PrepareSplit();   // game thread, before the shadow pass launches
bool SplitSnapshot();  // first shadow thread, before it starts its maps
void SplitEnter();     // second shadow thread, around its maps
void SplitLeave();
// Shadow thread, after its cascades: nothing it bound stays modified.
void EndWorkerPass();
// Render thread, before the shadow pass goes to the worker: shared state it reads made final.
void BeforeShadowLaunch();
// The engine-specific settings of the threading preset (see config.h).
void AddPresets();

}  // namespace tscfix::pipeline
