#pragma once
#include <windows.h>

// Removing a collideable from the physics manager searches a linked list of every collideable
// in the world, so unloading a tile full of objects costs O(n^2) and shows up as a stutter.
// This keeps an index from collideable to its list node; on removal the node is first moved to
// the front of the list, so the engine's own removal finds it immediately and does the rest
// (unlink, free, count) unchanged.
namespace tscfix::physicsfix {

bool Install(HMODULE engine, int listField);  // listField: local offset of the list in the manager
void SetActive(bool active);
void OnFrame();  // game thread: logs removal cost every 300 frames
// Braked standing trains held still (see physicsfix.cpp, [fixes] brake_hold).
bool InstallBrakeHold(HMODULE engine);
// Cab camera spring (see physicsfix.cpp): scales applied when the camera registers its spring.
bool InstallCabCamera(HMODULE engine, int springPercent, int dampingPercent, bool spring, bool watch = false);  // watch: [experiments] camera_watch

}  // namespace tscfix::physicsfix
