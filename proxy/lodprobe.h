#pragma once
#include <windows.h>

// [experiments] lod_probe=1: logs what the level-of-detail selector does on the threads that draw
// the main picture (see lodprobe.cpp). Measurement only.
namespace tscfix::lodprobe {

bool Install(HMODULE engine, int mode);  // 1: inputs of the selector; 2: level changes per instance
void LogUse();  // one line per thread, every 600 shadow passes

}  // namespace tscfix::lodprobe
