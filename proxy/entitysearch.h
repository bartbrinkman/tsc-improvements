#pragma once
#include <windows.h>

// [fixes] entity_search_cache (default 1): a script call to "child:function" finds its entity from a
// remembered tree instead of a search over the whole entity tree (see entitysearch.cpp).
namespace tscfix::entitysearch {

// mode: 0 off, 1 verify (the engine searches, the remembered tree is checked against it), 2 replace.
bool Install(HMODULE engine, int mode);
void SetMode(int mode);
// Children added to or removed from any entity so far, or -1 when the hooks are not installed.
LONG TreeChanges();
// The engine's entity search function once this module has hooked it (its first bytes are then
// changed, so a search by fingerprint no longer finds it), else null.
void* SearchFunction();

}  // namespace tscfix::entitysearch
