#pragma once
#include <windows.h>

// [fixes] fast_node_lookup: a model's node is found by name from a remembered index instead of a
// search over all nodes. Same answers as the engine's search (see nodelookup.cpp).
namespace tscfix::nodelookup {

// verify: the engine's search still gives every answer; remembered answers are only compared.
bool Install(HMODULE engine, bool verify);
void SetActive(bool active);

}  // namespace tscfix::nodelookup
