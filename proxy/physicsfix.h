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
// Camera rest (see physicsfix.cpp, [fixes] camera_rest): while the cab camera's transform stays
// within restUm micrometres and turnPpm millionths of its rest transform, the engine gets the rest
// transform, bit for bit. Installed inactive; SetCameraRestActive switches it.
bool InstallCameraRest(HMODULE engine, int restUm, int turnPpm, bool log);
void SetCameraRestActive(bool active);
// What the camera rest knows about the cab camera, for other threads (cheap: four atomic reads).
// An update is "at rest" when the engine got the rest transform in it, and "passed through" when
// the camera moved (or the rest is switched off). The cab camera's update runs on the game thread,
// one or more frames before that frame is drawn.
struct CameraRestInfo {
    bool installed;         // the hooks are in place
    bool active;            // switched on (SetCameraRestActive)
    unsigned restUpdates;   // updates in a row at rest; 0 when the last update passed through
    ULONGLONG updateTick;   // GetTickCount64 of the last cab camera update (0: none yet)
    ULONGLONG passedTick;   // GetTickCount64 of the last update that passed through (0: none yet); never goes back
};
CameraRestInfo CameraRestState();

}  // namespace tscfix::physicsfix
