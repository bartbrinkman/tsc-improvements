#pragma once
#include <windows.h>

// Drop-in replacements for small engine helpers that are called very often, with the same
// results. The engine's string compare (used by every string-keyed map: script functions,
// controls, entity names) measures both strings byte by byte and then compares byte by byte;
// the replacement uses the CRT strlen and compares eight bytes at a time.
namespace tscfix::fastpaths {

bool Install(HMODULE engine);
void SetActive(bool active);

}  // namespace tscfix::fastpaths
