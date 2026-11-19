#pragma once
#include <windows.h>

// Script calls addressed to child entities ("child:Function" / "*:Function") search the
// entity tree by name on every call. This module measures those lookups and (in later steps)
// caches them.
namespace tscfix::lookup {

bool InstallProbe(HMODULE engine);
void OnFrame();  // game thread, once per frame

}  // namespace tscfix::lookup
