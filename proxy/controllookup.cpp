#include "controllookup.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

// Control by name for a script call (game version v79.0a). The engine function gets (component
// holder, script holder). It asks the component for its control container, takes two arguments
// from the top of the script's Lua stack (an index number, then the name string, which it copies
// into a new engine string), and looks the name up.
//
// What the code reading gives (2026-11-20):
//   - For index 0 the engine itself remembers the answer: the container has a second map
//     (name -> control) that only this function reads and writes. A name that is in that map gets
//     the stored answer, also when the stored answer is "no such control". A name that is not in
//     it is searched in the container's real control map and the answer is then stored.
//   - Nothing takes an entry out of that map. It is made in the container's constructor and freed
//     in the container's destructor. No other engine function uses it.
//   - So the engine's answer for (container, name, index 0) cannot change from the first call to
//     the end of the container's life.
//
// Here the answer is remembered by (container, Lua string) after the engine gave it once. The next
// equal call gets it without the engine string and without the map search. A remembered answer is
// used only when
//   - the container was not constructed or destructed since (hooks on both),
//   - the container's map is the same map object and has not become smaller (a second guard for
//     the same event: a new container at the same address starts with an empty map),
//   - the Lua string has the same address and the same characters,
//   - the index argument is the number 0 and the name argument is a string,
//   - the script holder is the script of the Call() that runs now (so the Lua stack is known).
// Every other case runs the engine's function, and its answer is remembered.
namespace tscfix::controllookup {
namespace {

constexpr sites::Fingerprint kControl = {0xa25c05bd1c79140dull, 24};
constexpr sites::Fingerprint kContainerBegin = {0x26c7ffed438af420ull, 24};
constexpr sites::Fingerprint kContainerEnd = {0x086a98bf606679d2ull, 32};
// The container's interface id: the operand of an instruction in the lookup function.
constexpr sites::Site kContainerIid[] = {{0xa25c05bd1c79140dull, 24, 1, 3, 7, 0x1f, nullptr}};
// Container: +0x2f8 the remembered-answer map (head node), +0x300 its size.
constexpr size_t kMapHead = 0x2f8, kMapSize = 0x300;
// Lua state: +0x10 top, +0x18 base; a stack value is 16 bytes: type at +0, value at +8.
constexpr size_t kLuaTop = 0x10, kLuaBase = 0x18, kLuaValue = 16;
constexpr int kLuaNumber = 3, kLuaString = 4;

using ControlFn = void* (*)(void** component, void** script);
using BeginFn = void* (*)(uint8_t* container);
using EndFn = void (*)(uint8_t* container);
ControlFn g_control = nullptr;
BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
ToStringFn g_toString = nullptr;
const void* g_iid = nullptr;

int g_mode = 0;
volatile bool g_active = true;
volatile DWORD g_thread = 0;  // the table is used on one thread: the first that makes a script call
thread_local Current t_current{};
volatile LONG64 g_pairGeneration = 1;  // (see Current in controllookup.h)

// When a container last began or ended, by a hash of its address, on a clock that counts those
// events. Two containers on one slot only make an entry look older than it is.
constexpr size_t kContainerSlots = 4096;
volatile LONG64 g_clock = 1;
volatile LONG64 g_changed[kContainerSlots];
size_t ContainerSlot(const uint8_t* c) { return (reinterpret_cast<uintptr_t>(c) >> 4) * 0x9E3779B97F4A7C15ull >> 52; }
void ContainerChanged(const uint8_t* c) { g_changed[ContainerSlot(c)] = InterlockedIncrement64(&g_clock); }

struct Entry {
    uint8_t* container;
    const void* luaString;
    void* control;
    LONG64 clock;
    void* mapHead;
    uint64_t mapSize;
    char name[64];
};
constexpr size_t kSlots = 16384;  // power of two
Entry g_entries[kSlots];

volatile LONG64 g_remembered = 0, g_searched = 0, g_learned = 0, g_otherIndex = 0, g_noScript = 0, g_noContainer = 0, g_longName = 0;
volatile LONG64 g_staleClock = 0, g_staleMap = 0, g_staleName = 0, g_verified = 0, g_verifyWrong = 0;
volatile LONG64 g_stalePair = 0;  // lookups that found a pair left behind by a call that is gone

void* ContainerBegin(uint8_t* container) {
    ContainerChanged(container);
    return g_begin(container);
}
void ContainerEnd(uint8_t* container) {
    ContainerChanged(container);
    g_end(container);
}

void Release(void* object) {  // (an engine object's second virtual function)
    if (object) reinterpret_cast<void (*)(void*)>((*static_cast<void***>(object))[1])(object);
}

void* Control(void** component, void** script) {
    if (!g_mode || !component || !script || !*component || !*script) return g_control(component, script);
    DWORD me = GetCurrentThreadId();
    if (!g_thread)  // (the first thread claims it, once)
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_thread), static_cast<LONG>(me), 0);
    Current now = t_current;
    // A pair left behind by a call that a Lua error ended (see Current in controllookup.h): this
    // lookup is not inside that call, or a script state changed since. The pair is dropped.
    volatile char mark = 0;
    if (now.L && (reinterpret_cast<uintptr_t>(&mark) >= now.frame || now.generation != g_pairGeneration)) {
        InterlockedIncrement64(&g_stalePair);
        t_current = now = Current{};
    }
    if (me != g_thread || !now.L || now.script != *script || !g_toString) {
        InterlockedIncrement64(&g_noScript);
        return g_control(component, script);
    }
    // The two arguments at the top of the Lua stack: the name (a string), then the index (the number 0).
    auto* L = static_cast<uint8_t*>(now.L);
    uint8_t* top;
    uint8_t* base;
    memcpy(&top, L + kLuaTop, 8);
    memcpy(&base, L + kLuaBase, 8);
    int nameType = 0, indexType = 0;
    double index = -1;
    if (base && top >= base + 2 * kLuaValue) {
        memcpy(&nameType, top - 2 * kLuaValue, 4);
        memcpy(&indexType, top - kLuaValue, 4);
        memcpy(&index, top - kLuaValue + 8, 8);
    }
    if (nameType != kLuaString || indexType != kLuaNumber || index != 0.0) {
        InterlockedIncrement64(&g_otherIndex);
        return g_control(component, script);
    }
    // The container, as the engine's function asks for it (no reference is taken).
    void* object = *component;
    auto* container = reinterpret_cast<uint8_t* (*)(void*, const void*)>((*static_cast<void***>(object))[0x28 / 8])(object, g_iid);
    if (!container) {
        InterlockedIncrement64(&g_noContainer);
        return g_control(component, script);
    }
    const void* luaString;
    memcpy(&luaString, top - 2 * kLuaValue + 8, 8);
    const char* name = static_cast<const char*>(g_toString(L, -2));
    size_t length = name ? strnlen(name, sizeof Entry::name) : sizeof Entry::name;
    if (length >= sizeof Entry::name) {
        InterlockedIncrement64(&g_longName);
        return g_control(component, script);
    }
    void* mapHead;
    uint64_t mapSize;
    memcpy(&mapHead, container + kMapHead, 8);
    memcpy(&mapSize, container + kMapSize, 8);
    uint64_t hash =
        (reinterpret_cast<uintptr_t>(container) * 0x9E3779B97F4A7C15ull) ^ (reinterpret_cast<uintptr_t>(luaString) * 0xC2B2AE3D27D4EB4Full);
    Entry& e = g_entries[(hash >> 40) & (kSlots - 1)];
    bool known = e.container == container && e.luaString == luaString;
    if (known) {
        if (g_changed[ContainerSlot(container)] > e.clock) InterlockedIncrement64(&g_staleClock), known = false;
        else if (e.mapHead != mapHead || mapSize < e.mapSize) InterlockedIncrement64(&g_staleMap), known = false;
        else if (memcmp(e.name, name, length + 1) != 0) InterlockedIncrement64(&g_staleName), known = false;
    }
    if (known && g_mode == 2 && g_active) {
        // What the engine's function does on this path: both arguments leave the Lua stack, both holders are released.
        void* control = e.control;
        uint8_t* newTop = top - 2 * kLuaValue;
        memcpy(L + kLuaTop, &newTop, 8);
        void* heldScript = *script;
        Release(object);
        Release(heldScript);
        InterlockedIncrement64(&g_remembered);
        return control;
    }
    Entry learn{};
    learn.container = container, learn.luaString = luaString;
    memcpy(learn.name, name, length + 1);
    void* remembered = e.control;
    LONG64 clockBefore = g_changed[ContainerSlot(container)];
    void* control = g_control(component, script);  // (releases the holders: nothing of them is read after this)
    uint8_t* topAfter;
    memcpy(&topAfter, L + kLuaTop, 8);
    bool popped = topAfter == top - 2 * kLuaValue;
    if (known) {  // verify, or the off block of an A/B: the remembered answer is compared with the engine's
        bool right = remembered == control && popped;
        InterlockedIncrement64(right ? &g_verified : &g_verifyWrong);
        if (!right && g_verifyWrong <= 20)
            Log("controllookup: verify: remembered answer differs: container %p, name %s: remembered %p, engine %p, Lua stack moved by %lld bytes",
                static_cast<void*>(container), learn.name, remembered, control, static_cast<long long>(top - topAfter));
        if (right) return control;
    }
    InterlockedIncrement64(&g_searched);
    // Learn: the engine took both arguments, and the container did not begin or end during the call.
    if (!popped || g_changed[ContainerSlot(container)] != clockBefore) return control;
    learn.control = control;
    learn.clock = g_clock;
    memcpy(&learn.mapHead, container + kMapHead, 8);
    memcpy(&learn.mapSize, container + kMapSize, 8);
    if (learn.mapSize == 0) return control;  // the engine did not store the answer: not the path that was read
    e = learn;
    InterlockedIncrement64(&g_learned);
    return control;
}

}  // namespace

Current Pair(void* L, void* script, uintptr_t frame) { return {L, script, frame, g_pairGeneration}; }
void PairsChanged() { InterlockedIncrement64(&g_pairGeneration); }
Current SetCurrent(Current now) {
    Current before = t_current;
    t_current = now;
    return before;
}
void SetToString(ToStringFn toString) { g_toString = toString; }
void SetActive(bool on) { g_active = on; }

bool Install(HMODULE engine, int mode) {
    uintptr_t iid = 0;
    sites::ResolveAll(engine, kContainerIid, 1, &iid);  // (before the hook: the fingerprint reads the function's first bytes)
    void* control = sites::Find(engine, kControl);
    void* begin = sites::Find(engine, kContainerBegin);
    void* end = sites::Find(engine, kContainerEnd);
    if (!iid || !control || !begin || !end) {
        Log("controllookup: engine functions not found (game version changed?); off");
        return false;
    }
    g_iid = reinterpret_cast<uint8_t*>(engine) + iid;
    MH_Initialize();
    bool ok = MH_CreateHook(begin, reinterpret_cast<void*>(&ContainerBegin), reinterpret_cast<void**>(&g_begin)) == MH_OK &&
              MH_CreateHook(end, reinterpret_cast<void*>(&ContainerEnd), reinterpret_cast<void**>(&g_end)) == MH_OK &&
              MH_CreateHook(control, reinterpret_cast<void*>(&Control), reinterpret_cast<void**>(&g_control)) == MH_OK &&
              MH_EnableHook(begin) == MH_OK && MH_EnableHook(end) == MH_OK && MH_EnableHook(control) == MH_OK;
    if (ok) g_mode = mode;
    Log("controllookup: %s", !ok ? "hooks failed; off" : mode == 2 ? "remembered answers are used" : "verify (the engine's lookup always runs)");
    return ok;
}

void LogUse() {
    static int calls = 0;  // (called once per second: one line per 30 s)
    if (!g_mode || ++calls % 30) return;
    Log("controllookup: %lld from a remembered answer, %lld by the engine (%lld learned); not tried: index not 0 or no name %lld, "
        "script not known %lld, no container %lld, long name %lld; answers dropped: container began or ended %lld, map changed %lld, "
        "other Lua string %lld",
        g_remembered, g_searched, g_learned, g_otherIndex, g_noScript, g_noContainer, g_longName, g_staleClock, g_staleMap, g_staleName);
    Log("controllookup: verify: %lld remembered answers equal to the engine's, %lld differ; %lld pairs of ended calls dropped", g_verified, g_verifyWrong,
        g_stalePair);
}

}  // namespace tscfix::controllookup
