#pragma once
// (research) Single-step memory-access tracer for one engine call.
#include <windows.h>

#include <cstdint>

namespace tscfix::tracer {

// Arms a trace of the next call bracketed by TraceCall(); writes tscfix-trace.bin and
// tscfix-trace-modules.txt next to the game executable when the call returns.
void TraceArm();
// The calling thread is being single-stepped right now.
bool Tracing();
bool TraceArmed();
// Runs fn(arg...) under the tracer if armed (game thread only).
void TraceCall(void (*fn)(void*, int), void* self, int pass);
// Traces fn(a, b) now (no arming needed); output files carry `tag` (tscfix-trace-<tag>.bin).
uintptr_t TraceCall2(uintptr_t (*fn)(void*, void*), void* a, void* b, const char* tag);

}  // namespace tscfix::tracer
