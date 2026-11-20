#include "schedwatch.h"

#include <MinHook.h>
#include <dbghelp.h>

#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <vector>

#include "flickercap.h"
#include "log.h"
#include "rtti.h"
#include "sites.h"

// Scheduler layout (cSchedulerModule): +0xe8 one map per (mode x 16 + pass), 16 bytes each, its
// head node first; +0xc5c the current mode (below 12). A map node: +0 left, +8 parent, +0x10
// right, +0x19 "is the head", +0x28 the RenderableList. A RenderableList: +8 / +0x10 its
// renderables (16 bytes each), +0x20 / +0x28 the renderables removed while a walk ran (16 bytes
// each, skipped by the walk), +0x50 the number of walks that run now.
namespace tscfix::schedwatch {
namespace {

constexpr sites::Fingerprint kWalk = {0x677951c87a13a94dull, 16};        // (scheduler, pass): the render walk
constexpr sites::Fingerprint kRegister = {0x39393e0df51c0cf7ull, 16};    // (scheduler, renderable, passes, slot, order)
constexpr sites::Fingerprint kUnregister = {0x39e0b4bf7bb3f0a7ull, 32};  // (scheduler, renderable)
constexpr size_t kMaps = 0xe8, kMode = 0xc5c, kNodeList = 0x28;
constexpr size_t kItems = 8, kRemoved = 0x20, kWalks = 0x50;

using WalkFn = void (*)(void* scheduler, int pass);
using RegisterFn = uint64_t (*)(void* scheduler, void* renderable, unsigned passes, int slot, int order);
using UnregisterFn = void (*)(void* scheduler, void* renderable);
WalkFn g_walk = nullptr;
RegisterFn g_register = nullptr;
UnregisterFn g_unregister = nullptr;

volatile DWORD g_walker = 0;      // the thread of the first walk (the game thread)
volatile DWORD g_walkingNow = 0;  // the thread inside a walk now (0: none)
volatile LONG g_registers = 0, g_unregisters = 0, g_foreign = 0, g_duringOther = 0, g_duringOwn = 0, g_bad = 0, g_skippedWalks = 0;
volatile LONG g_told = 0;
volatile LONG64 g_skippedItems = 0, g_walkedItems = 0, g_removedLive = 0;
ULONGLONG g_lastSummary = 0;

struct Event {
    ULONGLONG tick;
    DWORD thread;
    DWORD walking;  // the thread in a walk at that time
    char kind;      // 'R' register, 'U' unregister
    void* renderable;
    void* caller;
};
constexpr int kRing = 64;
Event g_ring[kRing];
volatile LONG g_ringAt = 0;

bool Read(const void* from, void* to, size_t n) {
    __try {
        memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Where(void* at, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* slash = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%llx", slash ? slash + 1 : name,
                 static_cast<unsigned long long>(static_cast<uint8_t*>(at) - reinterpret_cast<uint8_t*>(m)));
    } else {
        snprintf(out, n, "%p", at);
    }
}

void LogStack(const char* what) {
    void* frames[12] = {};
    USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
    char line[512];
    int at = snprintf(line, sizeof line, "schedwatch: %s stack:", what);
    for (USHORT i = 0; i < n && at < static_cast<int>(sizeof line) - 64; ++i) {
        char w[96];
        Where(frames[i], w, sizeof w);
        at += snprintf(line + at, sizeof line - at, " %s", w);
    }
    Log("%s", line);
}

void Note(char kind, void* renderable, void* caller) {
    DWORD me = GetCurrentThreadId(), walking = g_walkingNow;
    LONG i = InterlockedIncrement(&g_ringAt) % kRing;
    g_ring[i] = {GetTickCount64(), me, walking, kind, renderable, caller};
    const char* why = nullptr;
    if (g_walker && me != g_walker) {
        InterlockedIncrement(&g_foreign);
        why = "from a thread that does not walk the lists";
    }
    if (walking && walking != me) {
        InterlockedIncrement(&g_duringOther);
        why = "while another thread walks the lists";
    } else if (walking == me) {
        InterlockedIncrement(&g_duringOwn);  // (the engine's own case: the walk keeps it in the removed list)
    }
    if (why && InterlockedIncrement(&g_told) <= 24) {
        char w[96];
        Where(caller, w, sizeof w);
        Log("schedwatch: %s of renderable %p on thread %lu %s (walker %lu, walking now %lu), called from %s", kind == 'R' ? "register" : "unregister",
            renderable, me, why, g_walker, walking, w);
        LogStack(kind == 'R' ? "register" : "unregister");
    }
}

uint64_t Register(void* scheduler, void* renderable, unsigned passes, int slot, int order) {
    InterlockedIncrement(&g_registers);
    Note('R', renderable, _ReturnAddress());
    return g_register(scheduler, renderable, passes, slot, order);
}

void Unregister(void* scheduler, void* renderable) {
    InterlockedIncrement(&g_unregisters);
    Note('U', renderable, _ReturnAddress());
    g_unregister(scheduler, renderable);
}

void DumpOnce(const char* why) {
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    auto write = dbg ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-sched.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (write && f != INVALID_HANDLE_VALUE)
        ok = write(GetCurrentProcess(), GetCurrentProcessId(), f,
                   static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpScanMemory), nullptr, nullptr,
                   nullptr) != FALSE;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Log("schedwatch: minidump (%s) %s: %s", why, ok ? "written" : "NOT written", path);
}

void LogRing() {
    LONG end = g_ringAt;
    for (LONG k = end - kRing + 1; k <= end; ++k) {
        if (k < 1) continue;
        const Event& e = g_ring[k % kRing];
        if (!e.tick) continue;
        char w[96];
        Where(e.caller, w, sizeof w);
        Log("schedwatch:   event %ld: %c %p thread %lu walking %lu at %llu ms from %s", k, e.kind, e.renderable, e.thread, e.walking,
            static_cast<unsigned long long>(e.tick), w);
    }
}

bool Vec(const uint8_t* list, size_t at, uint64_t* begin, uint64_t* end) {
    uint64_t v[2];
    if (!Read(list + at, v, sizeof v)) return false;
    *begin = v[0], *end = v[1];
    return (v[0] & 7) == 0 && (v[1] & 7) == 0 && v[0] <= v[1] && (v[1] - v[0]) % 16 == 0 && (v[1] - v[0]) / 16 < 1000000 && (v[0] != 0 || v[1] == 0);
}

// The lists of the pass, in the walk's order. Returns false (after logging) at the first list that
// is not sound.
bool Check(uint8_t* scheduler, int pass) {
    uint32_t mode = 0;
    if (!Read(scheduler + kMode, &mode, 4) || mode >= 12 || pass < 0 || pass >= 16) return true;  // (the engine handles these itself)
    uint8_t* head = nullptr;
    if (!Read(scheduler + kMaps + (static_cast<size_t>(mode) * 16 + pass) * 16, &head, 8) || !head) return true;
    uint8_t* node = nullptr;
    Read(head, &node, 8);
    for (int guard = 0; node && node != head && guard < 4096; ++guard) {
        uint8_t* list = nullptr;
        const char* bad = nullptr;
        uint64_t ib = 0, ie = 0, rb = 0, re = 0;
        if (!Read(node + kNodeList, &list, 8) || !list || (reinterpret_cast<uintptr_t>(list) & 7)) bad = "the node's list pointer";
        else if (!Vec(list, kItems, &ib, &ie)) bad = "the renderables array";
        else if (!Vec(list, kRemoved, &rb, &re)) bad = "the removed-while-walking array";
        if (bad) {
            InterlockedIncrement(&g_bad);
            if (InterlockedIncrement(&g_told) <= 40) {
                Log("schedwatch: list %p (node %p, mode %u, pass %d) not sound: %s; renderables %llx..%llx, removed %llx..%llx, walking now %lu, thread %lu",
                    list, node, mode, pass, bad, ib, ie, rb, re, g_walkingNow, GetCurrentThreadId());
                LogRing();
                DumpOnce(bad);
            }
            return false;
        }
        if (re > rb) {
            // Removed entries that are still renderables of the list: the walk skips them. Outside a
            // walk the engine has emptied this array, so any entry here is left over.
            uint32_t walks = 0;
            Read(list + kWalks, &walks, 4);
            size_t removed = (re - rb) / 16;
            if (!walks) InterlockedExchangeAdd64(&g_removedLive, static_cast<LONG64>(removed));
            if (!walks && InterlockedIncrement(&g_told) <= 40)
                Log("schedwatch: list %p (mode %u, pass %d) has %zu removed entries outside a walk (renderables %zu)", list, mode, pass, removed, (ie - ib) / 16);
        }
        InterlockedExchangeAdd64(&g_walkedItems, static_cast<LONG64>((ie - ib) / 16));
        // In-order successor (the walk's own way): right subtree's leftmost, else up while a right child.
        uint8_t* right = nullptr;
        char nil = 1;
        if (!Read(node + 0x10, &right, 8) || !right || !Read(right + 0x19, &nil, 1)) break;
        if (!nil) {
            node = right;
            for (int g = 0; g < 64; ++g) {
                uint8_t* left = nullptr;
                char lnil = 1;
                if (!Read(node, &left, 8) || !left || !Read(left + 0x19, &lnil, 1) || lnil) break;
                node = left;
            }
        } else {
            uint8_t* parent = nullptr;
            for (int g = 0; g < 64; ++g) {
                char pnil = 1;
                uint8_t* pr = nullptr;
                if (!Read(node + 8, &parent, 8) || !parent || !Read(parent + 0x19, &pnil, 1)) { node = nullptr; break; }
                if (pnil || !Read(parent + 0x10, &pr, 8) || pr != node) { node = parent; break; }
                node = parent;
            }
        }
    }
    return true;
}

// --- Vehicle watch ---------------------------------------------------------------------------
// Counting thunks in the vtables of the rail vehicle classes: one counter per slot index (all
// classes together). At each new frame of the walker thread the counts of the frame are taken.
constexpr int kSlots = 96;
volatile LONG64 g_slotCalls[kSlots];
LONG64 g_slotBefore[kSlots];
int g_slotCount = 0;           // slots with thunks (the shortest table)
constexpr int kHistory = 12;   // frames kept per slot
LONG g_hist[kSlots][kHistory];
uint64_t g_frameNo = 0;
LONG64 g_minuteSum[kSlots];
uint64_t g_minuteFrames = 0;
ULONGLONG g_minuteStart = 0;
int g_lowFor[kSlots];          // frames in a row below the level before the drop
LONG g_dropLevel[kSlots];
int g_events = 0;
ULONGLONG g_lastEvent = 0;
bool g_vehicles = false;
int g_lastPass = 1 << 30;

LONG Median(const LONG* v, int n) {
    LONG s[kHistory];
    memcpy(s, v, n * sizeof(LONG));
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && s[j - 1] > s[j]; --j) { LONG t = s[j]; s[j] = s[j - 1], s[j - 1] = t; }
    return s[n / 2];
}

void NewFrame() {
    ++g_frameNo;
    ULONGLONG now = GetTickCount64();
    if (!g_minuteStart) g_minuteStart = now;
    for (int i = 0; i < g_slotCount; ++i) {
        LONG64 total = g_slotCalls[i];
        LONG c = static_cast<LONG>(total - g_slotBefore[i]);
        g_slotBefore[i] = total;
        g_minuteSum[i] += c;
        LONG* h = g_hist[i];
        if (g_frameNo > kHistory) {
            // A drop: the median of the frames before has at least 6 calls, this frame has at least
            // 3 fewer and under 75% of it. It counts only when it lasts 4 frames.
            LONG before = Median(h, kHistory);
            if (!g_lowFor[i] && before >= 6 && c <= before - 3 && c * 4 < before * 3) g_lowFor[i] = 1, g_dropLevel[i] = before;
            else if (g_lowFor[i] && c <= g_dropLevel[i] - 3 && c * 4 < g_dropLevel[i] * 3) ++g_lowFor[i];
            else g_lowFor[i] = 0;
            if (g_lowFor[i] == 4) {
                Log("schedwatch: vehicles: slot %d drops from %ld to %ld calls per frame for 4 frames (frame %llu)", i, g_dropLevel[i], c,
                    static_cast<unsigned long long>(g_frameNo));
                if (g_events < 6 && now - g_lastEvent > 20000) {
                    ++g_events, g_lastEvent = now;
                    flickercap::Trigger();
                    Log("schedwatch: vehicles: frame capture started (event %d)", g_events);
                }
            }
        }
        memmove(h, h + 1, (kHistory - 1) * sizeof(LONG));
        h[kHistory - 1] = c;
    }
    ++g_minuteFrames;
    if (now - g_minuteStart >= 60000 && g_minuteFrames) {
        char line[1200];
        int n = 0;
        for (int i = 0; i < g_slotCount && n < 1100; ++i)
            if (g_minuteSum[i]) n += snprintf(line + n, sizeof line - n, " %d:%.1f", i, static_cast<double>(g_minuteSum[i]) / g_minuteFrames);
        Log("schedwatch: vehicles: calls per frame by slot (%llu frames):%s", static_cast<unsigned long long>(g_minuteFrames), line);
        memset(g_minuteSum, 0, sizeof g_minuteSum);
        g_minuteFrames = 0, g_minuteStart = now;
    }
}

void Walk(void* scheduler, int pass) {
    DWORD me = GetCurrentThreadId();
    if (!g_walker) InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_walker), static_cast<LONG>(me), 0);
    if (me == g_walker && g_vehicles) {
        if (pass <= g_lastPass) NewFrame();
        g_lastPass = pass;
    }
    if (!Check(static_cast<uint8_t*>(scheduler), pass)) {
        InterlockedIncrement(&g_skippedWalks);  // (the engine would read garbage: this walk is left out)
        return;
    }
    DWORD before = InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_walkingNow), static_cast<LONG>(me));
    if (before && before != me && InterlockedIncrement(&g_told) <= 24) {
        Log("schedwatch: walk of pass %d on thread %lu while thread %lu walks", pass, me, before);
        LogStack("second walker");
    }
    g_walk(scheduler, pass);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_walkingNow), static_cast<LONG>(before));
    ULONGLONG now = GetTickCount64();
    if (me == g_walker && now - g_lastSummary >= 60000) {
        g_lastSummary = now;
        Log("schedwatch: registers %ld, unregisters %ld; from another thread %ld, while another thread walks %ld, during its own walk %ld; lists not sound %ld (walks left out %ld); removed entries outside a walk %lld; renderables per minute walked %lld",
            g_registers, g_unregisters, g_foreign, g_duringOther, g_duringOwn, g_bad, g_skippedWalks, g_removedLive, g_walkedItems);
        InterlockedExchange64(&g_walkedItems, 0);
    }
}

}  // namespace

bool InstallVehicles(HMODULE engine) {
    auto tables = rtti::FindVTablesDerivedFrom(engine, ".?AViRailVehicle@@");
    if (tables.empty()) {
        Log("schedwatch: vehicles: no rail vehicle classes found; off");
        return false;
    }
    auto* image = reinterpret_cast<uint8_t*>(engine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
    uint8_t* end = image + nt->OptionalHeader.SizeOfImage;
    // Slots of each table: as long as there is a pointer into the module's code (a vtable ends where
    // the next object locator or other data begins).
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, tables.size() * kSlots * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return false;
    uint8_t* w = code;
    int most = 0;
    for (void** t : tables) {
        int slots = 0;
        while (slots < kSlots) {
            auto* f = static_cast<uint8_t*>(t[slots]);
            MEMORY_BASIC_INFORMATION mi{};
            if (f < image || f >= end || !VirtualQuery(f, &mi, sizeof mi) || !(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) break;
            ++slots;
        }
        Log("schedwatch: vehicles: a class with %d slots", slots);
        if (slots > most) most = slots;
        DWORD old = 0;
        if (!VirtualProtect(t, slots * sizeof(void*), PAGE_READWRITE, &old)) continue;
        for (int i = 0; i < slots; ++i) {
            uint8_t* thunk = w;
            auto put64 = [&](uint64_t v) { memcpy(w, &v, 8), w += 8; };
            *w++ = 0x48, *w++ = 0xB8, put64(reinterpret_cast<uint64_t>(&g_slotCalls[i]));  // mov rax, &counter
            *w++ = 0xF0, *w++ = 0x48, *w++ = 0xFF, *w++ = 0x00;                                // lock inc qword [rax]
            *w++ = 0x48, *w++ = 0xB8, put64(reinterpret_cast<uint64_t>(t[i]));                // mov rax, original
            *w++ = 0xFF, *w++ = 0xE0;                                                          // jmp rax
            w = thunk + 32;
            InterlockedExchangePointer(&t[i], thunk);
        }
        VirtualProtect(t, slots * sizeof(void*), old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), code, tables.size() * kSlots * 32);
    g_slotCount = most;
    g_vehicles = true;
    Log("schedwatch: vehicles: up to %d slots of %zu rail vehicle classes counted", most, tables.size());
    return true;
}

bool Install(HMODULE engine) {
    void* walk = sites::Find(engine, kWalk);
    void* reg = sites::Find(engine, kRegister);
    void* unreg = sites::Find(engine, kUnregister);
    if (!walk || !reg || !unreg) {
        Log("schedwatch: scheduler functions not found; off");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(walk, reinterpret_cast<void*>(&Walk), reinterpret_cast<void**>(&g_walk)) == MH_OK &&
              MH_CreateHook(reg, reinterpret_cast<void*>(&Register), reinterpret_cast<void**>(&g_register)) == MH_OK &&
              MH_CreateHook(unreg, reinterpret_cast<void*>(&Unregister), reinterpret_cast<void**>(&g_unregister)) == MH_OK &&
              MH_EnableHook(walk) == MH_OK && MH_EnableHook(reg) == MH_OK && MH_EnableHook(unreg) == MH_OK;
    Log("schedwatch: render scheduler watch %s", ok ? "installed" : "NOT installed");
    return ok;
}

}  // namespace tscfix::schedwatch
