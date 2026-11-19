#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

// Locate functions in a module by one-way fingerprint (see tools/fingerprint.py):
// FNV-1a 64 over the first `length` bytes of a function listed in .pdata.
namespace tscfix::sites {

struct Fingerprint {
    uint64_t hash;
    size_t length;
};

// Returns the function address, or nullptr if no (or more than one) function matches.
void* Find(HMODULE module, const Fingerprint& fp);

// An engine location. Kind 0: a function found by fingerprint, plus `offset` into it. Kind 1: the
// instruction at function + offset addresses its operand relative to itself (a RIP-relative
// operand or a call/jmp target); the location is that operand (displacement at instruction +
// dispAt, instruction insnLength bytes long). Kind 2: slot `offset` of the vtable of class `rtti`
// (run-time type name) for its sub-object at offset `hash`.
struct Site {
    uint64_t hash;
    uint8_t length;    // fingerprint window
    uint8_t kind;
    uint8_t dispAt;
    uint8_t insnLength;
    uint32_t offset;
    const char* rtti;
};

// Resolves every site in one pass over the module's functions. out[i] receives the location as an
// offset from the module base, or 0 when its function is missing or not unique. Returns how many
// resolved.
size_t ResolveAll(HMODULE module, const Site* sites, size_t count, uintptr_t* out);

}  // namespace tscfix::sites
