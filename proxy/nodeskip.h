#pragma once
#include <windows.h>

// [fixes] node_set_skip: script calls that show or hide a model part ("ActivateNode") are not
// carried out when the part already has that state (see nodeskip.cpp).
namespace tscfix::nodeskip {

// mode 1: verify (every call still runs; a "skip" verdict is checked against what the call did).
// mode 2: skip.
bool Install(HMODULE engine, int mode);
void SetActive(bool on);  // in-session A/B (mode 2)
void SetCensus(bool on);  // (diagnosis) log the nodes whose value flips most
void LogUse();

// From the script Call() hook (scriptcall.cpp), on the game thread.
enum Verdict { kPass = 0, kLearn = 1, kVerify = 2, kSkip = 3 };
using ToStringFn = void* (*)(void* L, int idx);
int Enter(void* L, ToStringFn toString);
void Leave(int verdict, int results);
void ScriptChanged();  // a Lua state was bound to another entity or script

}  // namespace tscfix::nodeskip
