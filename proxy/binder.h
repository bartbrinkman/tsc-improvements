#pragma once
// Per-thread copy of the engine's global draw binder
// (cHcDrawInterface) for the thread that runs the shadow pass. Engine sites are located by
// fingerprint (engine_sites.h); disabled when any is missing.
#include <windows.h>

#include <cstdint>

namespace tscfix::binder {

bool Install(HMODULE engine);
// Before Install: binder method `rva` continues (after the binder swap) at `detour`, which
// calls *original to run the method.
bool RegisterMethodDetour(uintptr_t rva, void* detour, void** original);
// Substitution on: binder methods called by the shadow-role thread on the global binder run
// on that thread's copy instead.
void SetSubstitute(bool on);
bool Substituting();
// The calling thread starts or stops running the shadow pass (snapshots the binder on entry).
void EnterShadow();
// Snapshot taken on the calling thread for thread `tid`, which runs the shadow pass next.
void EnterShadowFor(DWORD tid);
// The calling thread (which runs the shadow pass) points its instruction thunks at its copy.
void ActivateThread();
void LeaveShadow();
// The binder the calling thread draws with (the global, or its copy while substituting).
uint8_t* Binder();
uint8_t* Copy();  // the shadow thread's copy (valid once installed)
// Further copies for other threads (e.g. a main-view worker): allocate one, snapshot the global
// binder into it, and let the calling thread draw with it (nullptr: the global again).
uint8_t* NewCopy();
void SnapshotInto(uint8_t* copy);
// Like SnapshotInto, but from the binder the calling thread draws with (its copy, if it has one).
void SnapshotFromCurrent(uint8_t* copy);
void UseCopy(uint8_t* copy);
// Game thread, no worker running: restore (off) or re-apply (on) the binder's instruction sites
// and method hooks, so that with the overlap off the engine runs its own code. False when a
// thread was caught inside a site; try again later.
bool SetPatched(bool on);
uint8_t* Global();
uintptr_t Rva(uintptr_t rva);  // engine address of an rva
// A pointer into the global binder, moved into `copy` (others unchanged).
uintptr_t Translate(const uint8_t* copy, uintptr_t p);

}  // namespace tscfix::binder
