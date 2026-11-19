#pragma once
#include <windows.h>

// [fixes] safe_counts: thread-safe user counts for engine objects.
//
// About half of the engine's classes count their users with a plain add and subtract
// ("inc [rcx+N]; ret"). Two threads that change one count at the same time can lose a change.
// The object is then destroyed while something still holds it. Seen: a track section destroyed
// early (the game stopped at the unload of its tile), and the player profile, whose count four
// threads change while the shadow threads draw.
//
// The fix adds the processor's lock prefix to these adds and subtracts, in the game's memory
// only. It changes a function only when its bytes are exactly one of the known shapes, and a
// class only when both its AddRef and its Release are known shapes.
namespace tscfix::safecounts {

bool Install(HMODULE engine);

}  // namespace tscfix::safecounts
