#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

// Finds the instructions in a module's code that address a data range RIP-relative, and re-encodes
// each to address through a base register instead (so a thunk can point that register at a
// per-thread copy of the data). Works on the module as loaded in this process: nothing about the
// module's code is stored anywhere else.
namespace tscfix::ripscan {

struct Ref {
    uintptr_t at;          // instruction address
    uint8_t length;        // its length
    uintptr_t target;      // the address its operand points at
    uint8_t scratch;       // a register the instruction does not use (r8..r15)
    uint8_t base;          // the base register the re-encoded instruction addresses through
    uint8_t newLength;
    uint8_t reencoded[15];
};

// Linear sweep over every function in the module's exception directory. Instructions that
// cannot be redirected this way (branches and stack operations through memory, segment
// overrides) are counted in *skipped and not returned.
std::vector<Ref> Find(HMODULE module, uintptr_t lo, uintptr_t hi, size_t* skipped);

}  // namespace tscfix::ripscan
