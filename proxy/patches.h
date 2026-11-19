#pragma once
#include <windows.h>

// Mid-function code caves that add software prefetches to hot traversal loops.
//
// A site is a short run of instructions inside an engine function. It is located by the
// function's fingerprint plus an offset, and its bytes are checked against a one-way hash
// before anything is written. Those instructions move into a stub that first runs a small
// prefetch recipe, then the moved instructions, then jumps back. Behaviour is unchanged;
// only cache misses move earlier.
//
// Site details are game-specific and stay in the local ini ([patches] section):
//   <name>=<fp hash hex>,<fp length>,<offset hex>,<length>,<bytes hash hex>,<recipe>
// Recipe steps, separated by ';' (64-bit registers, signed hex or decimal displacements):
//   rax=[rbx+16]   load          rax=&[rbx+16]   address only
//   pf[rcx+64]     prefetcht0    ?rax==rdi / ?rax>=rbp   skip the rest of the recipe
namespace tscfix::patches {

// Reads the [patches] section of the ini at `iniPath` and prepares every valid site (inactive).
int Install(HMODULE engine, const char* iniPath);

// Switches all prepared sites on or off. Call from the game thread between frames.
void SetActive(bool active);

}  // namespace tscfix::patches
