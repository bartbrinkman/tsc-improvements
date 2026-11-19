#pragma once
#include <windows.h>

// In-process sampling profiler for one thread (the game's main thread).
//
// A worker thread suspends the target ~1000x per second, copies its registers,
// walks the stack with the x64 unwind tables every module already carries
// (.pdata), and resumes it. While the target is suspended the sampler takes no
// locks and allocates nothing, so it cannot deadlock against it.
//
// Control: named events "Local\tscfix.profile.{start,stop,done}.<pid>".
// Output:  tscfix-profile.txt next to the game executable, as collapsed stacks
//          ("root;...;leaf count"), frames named module!0x<function rva>.
namespace tscfix::profiler {

void Start(DWORD targetThreadId, int intervalUs);
// Frame boundary (game thread, at Present). Samples taken inside frames slower than 100 ms are
// also written to tscfix-profile-spikes.txt, so a single stutter is not diluted by the run.
void OnPresent();
// Sample another thread instead (before the start event), e.g. the overlap worker.
void Retarget(DWORD threadId);

}  // namespace tscfix::profiler
