#pragma once
#include <windows.h>

// [experiments] refcount_census=1 (measurement only): which engine classes get their user count
// changed by more than one thread while a worker thread draws.
//
// The engine has two kinds of AddRef: with a lock prefix (safe) and plain "inc [rcx+N]; ret"
// (not safe for two threads). This census puts a counting detour into every vtable slot that
// points at a plain AddRef (or at a thunk that jumps to one). Nothing else changes: the detour
// calls the engine's own function.
namespace tscfix::refcensus {

bool Install(HMODULE engine);
// Logs the classes that two or more threads changed while a worker drew (totals since start).
void LogUse();

}  // namespace tscfix::refcensus
