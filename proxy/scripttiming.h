#pragma once
#include <windows.h>

// After every Lua function call the script manager records the call's duration: it looks the
// script up in a string-keyed map (twice), allocates a node and a timing vector the first time,
// and appends the duration. The only reader is the -ShowScriptTiming overlay, which also clears
// the map every frame; but the call site tests the wrong field, so the recording runs whether
// or not the overlay is on (~7% of the game thread in a busy consist). This skips the recording
// while the overlay is off, releasing the string arguments the way the engine's function does.
namespace tscfix::scripttiming {

bool Install(HMODULE engine);
void SetActive(bool active);
void OnFrame();  // game thread: logs skipped recordings every 300 frames

}  // namespace tscfix::scripttiming
