#pragma once
// [fixes] script_call_cache: cache for the script Call() binding's lookup of its own entity and script.
#include <windows.h>

namespace tscfix::scriptcall {

bool Install(HMODULE engine);
void SetActive(bool on);

}  // namespace tscfix::scriptcall
