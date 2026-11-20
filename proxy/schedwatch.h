#pragma once
#include <windows.h>

// (diagnosis) [experiments] sched_watch=1: watches the render scheduler's lists of renderables
// (crash of 20 Nov: the render walk read a list whose pending-removal array was garbage).
// - register / unregister of a renderable: counted by thread; a call from a thread other than the
//   one that walks the lists, or while another thread walks them, is logged with its stack.
// - the render walk: every list of the pass is checked before the engine reads it. A list that is
//   not sound is logged with the last register / unregister events, a small minidump is written
//   (tscfix-sched.dmp in the game folder, once), and that walk is skipped instead of crashing.
namespace tscfix::schedwatch {

bool Install(HMODULE engine);

// (diagnosis) [experiments] vehicle_watch=1 (needs sched_watch=1 for the frame count): every virtual
// function of the rail vehicle classes (derived from iRailVehicle) is counted per frame. The function
// that draws a vehicle runs once per drawn car, so a drop of several cars at once that lasts is logged
// and starts the frame capture ([capture] flicker_key must be set). Every minute the calls per frame
// of each slot are logged.
bool InstallVehicles(HMODULE engine);

}  // namespace tscfix::schedwatch
