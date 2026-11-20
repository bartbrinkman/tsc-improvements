#pragma once
#include <windows.h>

// The engine's loading fibers (see loadclock.cpp).
//   [fixes] precise_load_clock: the fibers' slice clock counts true milliseconds.
//   [experiments] fiber_time: log the time the game thread spends in the loading fibers per frame.
namespace tscfix::loadclock {

// clock: replace the slice clock (it stays the engine's own until SetActive(true)).
// fiberTime: install the instrument. False when nothing was installed.
bool Install(HMODULE engine, bool clock, bool fiberTime);

// The precise clock on or off. Any thread may call this: the change is made on the fibers' own
// thread, at the next reading of the clock.
void SetActive(bool active);

}  // namespace tscfix::loadclock
