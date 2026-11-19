#pragma once
#include <windows.h>

// In-session A/B measurement. While a measurement window is open (named events
// "Local\tscfix.ab.{start,stop,done}.<pid>"), a feature is switched on and off every
// `period` frames and every frame is logged with its state: wall-clock frame time and
// the game thread's own CPU time. Both states see the same scene, interleaved, so
// differences between game sessions (AI traffic, streaming) cancel out.
namespace tscfix::ab {

using Toggle = void (*)(bool on);

// Registers the feature under test. Call once at start-up; no-op when name is empty.
void Configure(const char* feature, int periodFrames, Toggle toggle);
// Called once per frame on the game thread (at Present).
void OnFrame();

}  // namespace tscfix::ab
