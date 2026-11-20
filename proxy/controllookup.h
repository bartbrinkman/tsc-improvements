#pragma once
#include <windows.h>

#include <cstdint>

// [fixes] fast_control_lookup: a script call that names a control (GetControlValue,
// SetControlValue, ControlExists and others) gets the control from a remembered answer instead of
// a new engine string and a search by string compares (see controllookup.cpp).
namespace tscfix::controllookup {

// mode 1: verify (the engine's lookup always runs; a remembered answer is compared with it).
// mode 2: remembered answers are used.
bool Install(HMODULE engine, int mode);
void SetActive(bool on);  // in-session A/B (mode 2)
void LogUse();

// From the script Call() hook (scriptcall.cpp): the Lua state and the script object of the call
// that runs now on this thread (null when not known). Returns the previous pair for the restore.
//
// A Lua error can leave Call() without the restore, and the pair then stays set after its call
// is gone. Two more values make such a pair harmless:
//   - `frame`: where Call()'s frame was on the machine stack. The stack grows down, so a lookup
//     inside that call runs at a lower address. A lookup at the same address or above it is not
//     inside that call, and does not use the pair.
//   - `generation`: the count of PairsChanged() when the pair was made. A script state that was
//     set up or bound again since then makes every older pair unusable (the script's address can
//     belong to another script now, with another Lua state).
struct Current {
    void* L;
    void* script;
    uintptr_t frame = 0;
    LONG64 generation = 0;
};
using ToStringFn = void* (*)(void* L, int idx);
Current Pair(void* L, void* script, uintptr_t frame);  // a pair with the generation of now
Current SetCurrent(Current now);
void PairsChanged();  // a script state was set up or bound again (scriptcall.cpp)
void SetToString(ToStringFn toString);

}  // namespace tscfix::controllookup
