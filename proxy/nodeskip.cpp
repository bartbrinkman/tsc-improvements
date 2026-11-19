#include "nodeskip.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine_sites.h"
#include "entitysearch.h"
#include "log.h"
#include "sites.h"

// Train scripts set model parts shown or hidden every frame, mostly to the state the part already
// has (measured: 98.7% of 8 million sets in a 4-minute drive). Each call goes through the whole
// script call path: entity search, function table, argument holders, node search by name.
//
// The engine's handler (read from the code) does this for one named node: get the entity's model,
// find the node index by name, set bit 4 in the model's flags, write one byte in the model's state
// array. So a call changes nothing when the byte already has the value and the bit is set.
//
// Here a call Call("...ActivateNode", name, value) is remembered by (Lua state, function string,
// node string, value) together with what the engine did for it: one set of (model, index, byte).
// The next equal call is skipped when
//   - the model has not ended its life or loaded its shape again since (hooks on both),
//   - no entity gained or lost a child since, and no Lua state was bound again since,
//   - the remembered call returned no results to the script,
//   - the state byte has the value now and the flag bit is set now.
// Every other case runs the engine's call and is learned again.
namespace tscfix::nodeskip {
namespace {

constexpr sites::Fingerprint kActivateNode = {0x1f0cd731ea680b02ull, 32};  // the handler; the setter is a leaf it calls
constexpr sites::Fingerprint kModelEnd = {0x9b33aac87d0e989full, 24};
constexpr sites::Fingerprint kModelLoad = {0x81e29ddefe73644cull, 16};
// Model object: +8 shape, +0x10 flags, +0x18 state bytes; shape +0x74 node count.
constexpr size_t kModelShape = 8, kModelFlags = 0x10, kModelStates = 0x18, kShapeCount = 0x74;
// Lua state: +0x10 top, +0x18 base; a stack value is 16 bytes: type at +0, value at +8.
constexpr size_t kLuaTop = 0x10, kLuaBase = 0x18;
constexpr int kLuaNumber = 3, kLuaString = 4;

using SetNodeFn = void (*)(uint8_t* model, uint32_t index, uint8_t shown);
using ModelEndFn = void* (*)(uint8_t* model, uint32_t flags);
using ModelLoadFn = void (*)(uint8_t* model);
SetNodeFn g_setNode = nullptr;
ModelEndFn g_modelEnd = nullptr;
ModelLoadFn g_modelLoad = nullptr;

int g_mode = 0;
volatile bool g_active = true;
DWORD g_thread = 0;

// When a model last changed (ended or loaded), by a hash of its address, on a clock that counts
// those events. A collision only makes an entry look older than it is.
constexpr size_t kModelSlots = 4096;
volatile LONG64 g_modelClock = 1;
volatile LONG64 g_modelChanged[kModelSlots];
size_t ModelSlot(const uint8_t* model) { return (reinterpret_cast<uintptr_t>(model) >> 4) * 0x9E3779B97F4A7C15ull >> 52; }
void ModelChanged(const uint8_t* model) { g_modelChanged[ModelSlot(model)] = InterlockedIncrement64(&g_modelClock); }
volatile LONG g_scriptClock = 0;

struct Entry {
    void* L;
    const char* fnAt;
    const char* nodeAt;
    double value;
    uint8_t* model;
    uint32_t index;
    uint8_t shown;
    LONG64 modelClock;
    LONG treeClock, scriptClock;
    char fn[40];
    char node[56];
};
constexpr size_t kSlots = 65536;  // power of two (about 2,500 distinct calls per train set; fewer shared slots)
Entry g_entries[kSlots];

struct Now {  // the call that runs now (game thread)
    int verdict = kPass;
    Entry key{};
    Entry* slot = nullptr;
    int sets = 0;
    bool spoiled = false;  // another candidate call ran inside it
    uint8_t* model = nullptr;
    uint32_t index = 0;
    uint8_t shown = 0;
    bool wouldChange = false;
};
Now g_now;

volatile LONG64 g_sets = 0, g_setsSame = 0, g_skipped = 0, g_learned = 0, g_notLearned = 0, g_ran = 0, g_verified = 0, g_verifyWrong = 0;
volatile LONG64 g_staleModel = 0, g_staleTree = 0, g_staleScript = 0, g_stateDiffers = 0;

void SetNode(uint8_t* model, uint32_t index, uint8_t shown) {
    uint8_t* shape;
    uint8_t* states;
    memcpy(&shape, model + kModelShape, 8);
    memcpy(&states, model + kModelStates, 8);
    uint32_t count = 0;
    if (shape) memcpy(&count, shape + kShapeCount, 4);
    bool real = states && index < count;
    if (real) {
        InterlockedIncrement64(&g_sets);
        if (states[index] == shown) InterlockedIncrement64(&g_setsSame);
    }
    if (g_now.verdict != kPass && GetCurrentThreadId() == g_thread) {
        if (g_now.sets++ == 0) g_now.model = model, g_now.index = index, g_now.shown = shown;
        if (real && states[index] != shown) g_now.wouldChange = true;
    }
    g_setNode(model, index, shown);
}
void* ModelEnd(uint8_t* model, uint32_t flags) {
    ModelChanged(model);
    return g_modelEnd(model, flags);
}
void ModelLoad(uint8_t* model) {
    ModelChanged(model);
    g_modelLoad(model);
}

// True when the engine's handler would change nothing for this entry now.
bool NothingToDo(const Entry& e) {
    uint8_t* shape;
    uint8_t* states;
    uint32_t flags, count;
    memcpy(&shape, e.model + kModelShape, 8);
    memcpy(&states, e.model + kModelStates, 8);
    memcpy(&flags, e.model + kModelFlags, 4);
    if (!shape || !states || !(flags & 4)) return false;
    memcpy(&count, shape + kShapeCount, 4);
    return e.index < count && states[e.index] == e.shown;
}

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

}  // namespace

// (diagnosis) [experiments] node_census=1: per script and node, how often ActivateNode flips the
// value; the nodes that flip most are logged every 5 s (a lamp shown and hidden in turn flickers).
bool g_census = false;
struct Flips { double last = -1; long flips = 0, calls = 0; };
std::unordered_map<std::string, Flips> g_flips;
void Census(void* L, const char* node, double value) {
    char key[160];
    snprintf(key, sizeof key, "%p %s", L, node);
    Flips& f = g_flips[key];
    ++f.calls;
    if (f.last >= 0 && f.last != value) ++f.flips;
    f.last = value;
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (!next) next = now + 5000;
    if (now < next) return;
    next = now + 5000;
    std::vector<std::pair<long, std::string>> top;
    for (auto& [k, v] : g_flips)
        if (v.flips && k.find("SendConsistMessage") == std::string::npos) top.push_back({v.flips, k + " calls " + std::to_string(v.calls)});
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    Log("nodeskip: census: %zu nodes, %zu flip; top flips in 5 s:", g_flips.size(), top.size());
    for (size_t i = 0; i < top.size() && i < 30; ++i) Log("nodeskip:   %ld flips: %s", top[i].first, top[i].second.c_str());
    g_flips.clear();
}

int Enter(void* L, ToStringFn toString) {
    if (!g_mode || !L) return kPass;
    DWORD me = GetCurrentThreadId();
    if (!g_thread) g_thread = me;
    if (me != g_thread) return kPass;
    if (g_now.verdict != kPass) {  // a script call inside a script call: the outer one is not learned
        g_now.spoiled = true;
        return kPass;
    }
    uint8_t* top;
    uint8_t* base;
    memcpy(&top, static_cast<uint8_t*>(L) + kLuaTop, 8);
    memcpy(&base, static_cast<uint8_t*>(L) + kLuaBase, 8);
    if (g_census && base && top >= base + 32) {  // any Call(fn, x) or Call(fn, name, x) with a number x
        int c1, c2, c3 = 0;
        memcpy(&c1, base, 4), memcpy(&c2, base + 16, 4);
        if (top >= base + 48) memcpy(&c3, base + 32, 4);
        double v = -2;
        if (c2 == kLuaNumber) memcpy(&v, base + 24, 8);
        else if (c2 == kLuaString && c3 == kLuaNumber) memcpy(&v, base + 40, 8);
        const char* name = c1 == kLuaString ? static_cast<const char*>(toString(L, 1)) : nullptr;
        const char* arg = c2 == kLuaString ? static_cast<const char*>(toString(L, 2)) : "";
        if (name && v != -2) {
            char both[128];
            snprintf(both, sizeof both, "%s(%s)", name, arg ? arg : "");
            Census(L, both, v);
        }
    }
    if (!base || top < base + 48) return kPass;  // fewer than three arguments
    int t1, t2, t3;
    memcpy(&t1, base, 4), memcpy(&t2, base + 16, 4), memcpy(&t3, base + 32, 4);
    if (t1 != kLuaString || t2 != kLuaString || t3 != kLuaNumber) return kPass;
    const char* fn = static_cast<const char*>(toString(L, 1));
    if (!fn) return kPass;
    size_t fnLength = strnlen(fn, sizeof Entry::fn);
    if (fnLength < 12 || fnLength >= sizeof Entry::fn || memcmp(fn + fnLength - 12, "ActivateNode", 12) != 0) return kPass;
    if (fnLength > 12 && fn[fnLength - 13] != ':') return kPass;
    const char* node = static_cast<const char*>(toString(L, 2));
    if (!node) return kPass;
    size_t nodeLength = strnlen(node, sizeof Entry::node);
    if (nodeLength >= sizeof Entry::node) return kPass;
    double value;
    memcpy(&value, base + 40, 8);

    uint64_t bits;
    memcpy(&bits, &value, 8);
    uint64_t hash = (reinterpret_cast<uintptr_t>(L) * 0x9E3779B97F4A7C15ull) ^ (reinterpret_cast<uintptr_t>(fn) * 0xC2B2AE3D27D4EB4Full) ^
                    (reinterpret_cast<uintptr_t>(node) * 0x165667B19E3779F9ull) ^ (bits * 0x27D4EB2F165667C5ull);
    Entry& e = g_entries[(hash >> 36) & (kSlots - 1)];
    g_now = Now{};
    g_now.slot = &e;
    bool known = e.L == L && e.fnAt == fn && e.nodeAt == node && e.value == value && e.model && memcmp(e.fn, fn, fnLength + 1) == 0 &&
                 memcmp(e.node, node, nodeLength + 1) == 0;
    if (known) {
        if (g_modelChanged[ModelSlot(e.model)] > e.modelClock) InterlockedIncrement64(&g_staleModel), known = false;
        else if (entitysearch::TreeChanges() != e.treeClock) InterlockedIncrement64(&g_staleTree), known = false;
        else if (g_scriptClock != e.scriptClock) InterlockedIncrement64(&g_staleScript), known = false;
        else if (!NothingToDo(e)) InterlockedIncrement64(&g_stateDiffers), known = false;
    }
    if (known) {
        if (g_mode == 2 && g_active) {
            InterlockedIncrement64(&g_skipped);
            return kSkip;
        }
        g_now.key = e;
        return g_now.verdict = kVerify;
    }
    Entry& k = g_now.key;
    k.L = L, k.fnAt = fn, k.nodeAt = node, k.value = value;
    memcpy(k.fn, fn, fnLength + 1), memcpy(k.node, node, nodeLength + 1);
    k.modelClock = g_modelClock, k.treeClock = entitysearch::TreeChanges(), k.scriptClock = g_scriptClock;
    return g_now.verdict = kLearn;
}

void Leave(int verdict, int results) {
    if (verdict != kLearn && verdict != kVerify) return;
    Now now = g_now;
    g_now.verdict = kPass;
    InterlockedIncrement64(&g_ran);
    if (verdict == kVerify) {
        // The verdict was "skip": the call must have made one set, of the remembered byte, that changed nothing, with no results.
        bool right = !now.spoiled && now.sets == 1 && now.model == now.key.model && now.index == now.key.index && now.shown == now.key.shown &&
                     !now.wouldChange && results == 0;
        InterlockedIncrement64(right ? &g_verified : &g_verifyWrong);
        if (!right && g_verifyWrong <= 20)
            Log("nodeskip: verify: a call with the verdict \"skip\" did something else: %s(%s, %g): %d sets, model %p/%p, index %u/%u, value %u/%u, "
                "changed %d, results %d, inner call %d",
                now.key.fn, now.key.node, now.key.value, now.sets, static_cast<void*>(now.model), static_cast<void*>(now.key.model), now.index,
                now.key.index, now.shown, now.key.shown, now.wouldChange, results, now.spoiled);
        return;
    }
    // Learn: exactly one set, no results, no script call inside, and nothing in the clocks moved during the call.
    if (now.spoiled || now.sets != 1 || results != 0 || now.key.treeClock != entitysearch::TreeChanges() || now.key.scriptClock != g_scriptClock ||
        g_modelChanged[ModelSlot(now.model)] > now.key.modelClock) {
        InterlockedIncrement64(&g_notLearned);
        return;
    }
    now.key.model = now.model, now.key.index = now.index, now.key.shown = now.shown;
    *now.slot = now.key;
    InterlockedIncrement64(&g_learned);
}

void ScriptChanged() { InterlockedIncrement(&g_scriptClock); }
void SetActive(bool on) { g_active = on; }
void SetCensus(bool on) { g_census = on; }

bool Install(HMODULE engine, int mode) {
    auto* handler = static_cast<uint8_t*>(sites::Find(engine, kActivateNode));
    void* end = sites::Find(engine, kModelEnd);
    void* load = sites::Find(engine, kModelLoad);
    // (one-way hash of the setter's 25 bytes)
    if (entitysearch::TreeChanges() < 0) {
        Log("nodeskip: needs the entity search hooks ([fixes] entity_search_cache); off");
        return false;
    }
    uintptr_t setterAt = engine::Resolved(engine)[engine::SetNodeState];
    uint8_t* setter = setterAt ? reinterpret_cast<uint8_t*>(engine) + setterAt : nullptr;
    if (!handler || !end || !load || !setter || Fnv(setter, 25) != 0x897d0ea965c07b9aull) {
        Log("nodeskip: engine functions not found (game version changed?); off");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(setter, reinterpret_cast<void*>(&SetNode), reinterpret_cast<void**>(&g_setNode)) == MH_OK &&
              MH_CreateHook(end, reinterpret_cast<void*>(&ModelEnd), reinterpret_cast<void**>(&g_modelEnd)) == MH_OK &&
              MH_CreateHook(load, reinterpret_cast<void*>(&ModelLoad), reinterpret_cast<void**>(&g_modelLoad)) == MH_OK &&
              MH_EnableHook(setter) == MH_OK && MH_EnableHook(end) == MH_OK && MH_EnableHook(load) == MH_OK;
    if (ok) g_mode = mode;
    Log("nodeskip: %s", !ok ? "hooks failed; off" : mode == 2 ? "calls that change nothing are skipped" : "verify (every call runs)");
    return ok;
}

void LogUse() {
    static int calls = 0;  // (called once per second: one line per 30 s)
    if (!g_mode || ++calls % 30) return;
    Log("nodeskip: %lld skipped, %lld ran (%lld learned, %lld not learnable); verdicts dropped: model changed %lld, entity tree changed %lld, "
        "script bound again %lld, state differs %lld; all sets %lld, same value %lld",
        g_skipped, g_ran, g_learned, g_notLearned, g_staleModel, g_staleTree, g_staleScript, g_stateDiffers, g_sets, g_setsSame);
    if (g_mode == 1) Log("nodeskip: verify: %lld \"skip\" verdicts checked and right, %lld wrong", g_verified, g_verifyWrong);
}

}  // namespace tscfix::nodeskip
