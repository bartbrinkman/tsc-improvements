#pragma once
#include <windows.h>

// [fixes] fast_function_lookup: a script call finds its engine function in an entity's function
// table from a remembered entry instead of a compare with every entry (see funclookup.cpp).
namespace tscfix::funclookup {

// verify: the engine's own search still runs in full; remembered entries are only compared.
bool Install(HMODULE engine, bool verify);
void SetActive(bool active);
void LogUse();

}  // namespace tscfix::funclookup
