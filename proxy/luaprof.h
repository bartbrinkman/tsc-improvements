#pragma once
#include <windows.h>

// Per-script timing of the embedded Lua 5.0.2 interpreter.
//
// Hooks lua_pcall (located by fingerprint) and attributes the time of every call
// made on the main thread to the called Lua function, identified by its chunk
// source and line from the public Lua 5.0.2 object layout. Nested calls are
// subtracted, so "self" is the time spent in that script function itself.
namespace tscfix::luaprof {

bool Install(HMODULE engine, DWORD mainThreadId);
void SetActive(bool active);
void WriteReport(const char* path, double seconds);

}  // namespace tscfix::luaprof
