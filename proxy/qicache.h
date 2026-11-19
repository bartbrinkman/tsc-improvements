#pragma once
// [fixes] qi_cache: per-thread cache for one interface query of the engine's entities (the
// container interface asked of every node an entity search visits).
#include <windows.h>

namespace tscfix::qicache {

// verify: every 64th hit is also asked of the engine and compared.
bool Install(HMODULE engine, bool verify);
void SetActive(bool on);

}  // namespace tscfix::qicache
