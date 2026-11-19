#pragma once

// Frame-time capture without PresentMon (which needs an elevated session). Bench opens a
// window with the named events "Local\tscfix.frames.{start,stop,done}.<pid>"; every Present
// in between is logged to tscfix-frames.csv in PresentMon's column format.
namespace tscfix::framelog {

void Init();     // creates the events (once, at start-up)
void OnFrame();  // game thread, at Present

}  // namespace tscfix::framelog
