#include "lookup.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

namespace tscfix::lookup {
namespace {

// Child-call dispatch (game version v79.0a): (entity, child name or null for "*", function
// name, arguments, bool* handled) -> int. Searches the entity's subtree for matching names.
constexpr sites::Fingerprint kChildCall = {0xa625e0483501d932ull, 24};
using ChildCallFn = int (*)(void* entity, const char* child, const char* func, void* args, bool* handled);
ChildCallFn g_childCall = nullptr;

uint64_t Fnv(const char* s, uint64_t h = 0xCBF29CE484222325ull) {
    for (; s && *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 0x100000001B3ull;
    return h;
}

// Per-frame key census: how often the same (entity, child, function) repeats within a frame.
constexpr int kSlots = 1 << 15;
struct Key { uint64_t hash; unsigned frame; };
Key g_keys[kSlots];
unsigned g_frame = 1;
uint64_t g_calls = 0, g_nested = 0, g_broadcast = 0, g_unique = 0, g_uniqueTarget = 0;
int g_depth = 0;  // the search re-enters ChildCall for every entity it visits
int64_t g_ticks = 0;
Key g_targets[kSlots];  // (entity, child) only: what a cache keyed on the search target would see

bool Seen(Key* table, uint64_t h) {
    for (int i = 0; i < kSlots; ++i) {
        Key& k = table[(h + i) & (kSlots - 1)];
        if (k.frame != g_frame) {
            k = {h, g_frame};
            return false;
        }
        if (k.hash == h) return true;
    }
    return true;
}

int ChildCall(void* entity, const char* child, const char* func, void* args, bool* handled) {
    if (g_depth > 0) {
        ++g_nested;
        ++g_depth;
        int r = g_childCall(entity, child, func, args, handled);
        --g_depth;
        return r;
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ++g_depth;
    int r = g_childCall(entity, child, func, args, handled);
    --g_depth;
    QueryPerformanceCounter(&b);
    g_ticks += b.QuadPart - a.QuadPart;
    ++g_calls;
    if (!child) ++g_broadcast;
    uint64_t target = Fnv(child, reinterpret_cast<uintptr_t>(entity) * 0x9E3779B97F4A7C15ull);
    if (!Seen(g_keys, Fnv(func, target))) ++g_unique;
    if (!Seen(g_targets, target)) ++g_uniqueTarget;
    return r;
}

// --- research: identify the culling service the scene graph consults ---------------------------
// Service accessor (game version v79.0a): returns the culling interface used by scene graph nodes.
constexpr sites::Fingerprint kCullerGet = {0x106014a2d0c521d9ull, 16};
using GetFn = void* (*)();
GetFn g_cullerGet = nullptr;
HMODULE g_engine = nullptr;

void LogClass(const char* what, void* object, const int* slots, int n) {
    if (!object) return;
    void** vt = *static_cast<void***>(object);
    auto base = reinterpret_cast<uintptr_t>(g_engine);
    auto col = static_cast<const DWORD*>(vt[-1]);  // complete object locator (x64: RVAs)
    const char* name = reinterpret_cast<const char*>(base + col[3] + 16);
    Log("lookup: %s is %s (vtable +0x%llx)", what, name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt) - base));
    for (int i = 0; i < n; ++i)
        Log("lookup:   slot +0x%x -> +0x%llx", slots[i] * 8,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[slots[i]]) - base));
}

}  // namespace

bool InstallProbe(HMODULE engine) {
    g_engine = engine;
    g_cullerGet = static_cast<GetFn>(sites::Find(engine, kCullerGet));
    void* target = sites::Find(engine, kChildCall);
    if (!target) {
        Log("lookup: child call not found; probe disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&ChildCall), reinterpret_cast<void**>(&g_childCall)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    Log("lookup: probe %s", ok ? "installed" : "failed");
    return ok;
}

void OnFrame() {
    if (!g_childCall) return;
    ++g_frame;
    if (g_frame == 600 && g_cullerGet) {
        const int slots[] = {6, 15};
        LogClass("culling service", g_cullerGet(), slots, 2);
    }
    if (g_frame % 300 != 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    Log("lookup: per frame: script child calls %.1f (broadcast %.1f, nested visits %.1f), unique call keys %.1f, "
        "unique targets %.1f, %.3f ms in child calls",
        g_calls / 300.0, g_broadcast / 300.0, g_nested / 300.0, g_unique / 300.0, g_uniqueTarget / 300.0,
        g_ticks * 1000.0 / f.QuadPart / 300.0);
    g_calls = g_nested = g_broadcast = g_unique = g_uniqueTarget = 0;
    g_ticks = 0;
}

}  // namespace tscfix::lookup
