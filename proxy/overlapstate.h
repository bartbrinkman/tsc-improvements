#pragma once
#include <windows.h>

// Per-thread copies of the engine's shared render scratch state, so the shadow pass can run
// on a worker while the game thread renders the main view. Each piece of state the two passes
// both write (found by tracing both passes and comparing their memory accesses) gets its own
// copy on the thread that runs the shadow pass; the game thread keeps using the engine's.
//
// Model node palette: the model draw writes the node matrices into a palette owned by the model
// (shared by every draw of that model). On the shadow thread our hook computes the palette into a
// thread-local buffer and points the draw context there instead.
namespace tscfix::overlapstate {

bool Install(HMODULE engine);
void SetActive(bool active);
// The calling thread starts or stops running the shadow pass.
void SetShadowRole(bool shadow);
// The calling thread draws models alongside another drawing thread: skeleton palettes are
// computed into a per-thread buffer instead of the model's shared one.
void SetPrivateDraw(bool on);
// The calling thread runs the shadow pass and per-thread state is active. Inline: every hook of
// every drawing thread asks this (it was ~1% of the shadow thread as a call).
extern volatile bool g_active;
extern thread_local bool t_shadow;
inline bool ShadowRole() { return t_shadow && g_active; }

}  // namespace tscfix::overlapstate
