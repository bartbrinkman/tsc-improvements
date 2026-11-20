#include "funclookup.h"

#include <MinHook.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

// Function by name on an entity (game version v79.0a). A script call "Function" reaches an engine
// function that asks the entity, and then each of its components, for a table of (name, handler)
// pairs. It compares the name with every entry in turn. For an entry with that name it calls the
// handler, and it stops when a handler accepts the call.
//
// The tables are constant data in the engine module. Here the first entry with the name is
// remembered per (table, count, name). The two places where the engine asks for a table get the
// table from the remembered entry on (pointer moved forward, count made smaller), or a count of 0
// when no entry has the name. The engine's own loop then runs as before: the entries that are
// passed over are the ones its compare would reject. Nothing after the loop reads the pointer or
// the count (read from the code).
//
// A table outside the engine module is not remembered: the engine's loop gets it unchanged.
namespace tscfix::funclookup {
namespace {

constexpr sites::Fingerprint kFunctionByName = {0xdd470f94a1c71db2ull, 32};
// The two table requests inside it: offset, the 6 bytes there, the vtable slot they call.
constexpr size_t kSiteEntity = 0x4c, kSiteComponent = 0x152;
// (one-way hashes of the 6 bytes at each place: a virtual call, and a register move with a virtual call)
constexpr uint64_t kHashEntity = 0xf918782598d0526dull, kHashComponent = 0xe6f073ac1d2f4f23ull;
uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct Pair {
    const char* name;
    void* handler;
};
using TableFn = int (*)(void* self, Pair** table);

volatile bool g_active = false;
bool g_verify = false;
uintptr_t g_imageLo = 0, g_imageHi = 0;

struct Entry {
    const Pair* table;
    int count;
    int index;  // first entry with the name, or -1: no entry has it
    char name[48];
};
constexpr size_t kSlots = 2048;  // power of two
thread_local Entry t_entries[kSlots];
volatile LONG64 g_remembered = 0, g_searched = 0, g_skippedEntries = 0, g_notConstant = 0, g_differences = 0, g_longNames = 0;

int Search(const Pair* table, int count, const char* name) {
    for (int i = 0; i < count; ++i)
        if (strcmp(table[i].name, name) == 0) return i;
    return -1;
}

// What the engine's loop gets: the count to use, with *out moved to the first entry to compare.
int Adjust(Pair** out, int count, const char* name) {
    const Pair* table = out ? *out : nullptr;
    if (!g_active || !table || count <= 0 || !name) return count;
    auto at = reinterpret_cast<uintptr_t>(table);
    if (at < g_imageLo || at + static_cast<size_t>(count) * sizeof(Pair) > g_imageHi) {
        InterlockedIncrement64(&g_notConstant);
        return count;
    }
    size_t length = strlen(name);
    if (length >= sizeof Entry::name) {
        InterlockedIncrement64(&g_longNames);
        return count;
    }
    uint64_t hash = 0xCBF29CE484222325ull ^ at;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ static_cast<unsigned char>(name[i])) * 0x100000001B3ull;
    Entry& e = t_entries[(hash ^ (hash >> 29)) & (kSlots - 1)];
    bool known = e.table == table && e.count == count && memcmp(e.name, name, length + 1) == 0;
    int index;
    if (g_verify) {  // the engine's loop decides; the remembered entry is only compared
        index = Search(table, count, name);
        if (known && e.index != index) InterlockedIncrement64(&g_differences);
        InterlockedIncrement64(known ? &g_remembered : &g_searched);
        if (!known) e.table = table, e.count = count, e.index = index, memcpy(e.name, name, length + 1);
        return count;
    }
    if (known) {
        index = e.index;
        InterlockedIncrement64(&g_remembered);
    } else {
        index = Search(table, count, name);
        e.table = table, e.count = count, e.index = index, memcpy(e.name, name, length + 1);
        InterlockedIncrement64(&g_searched);
    }
    if (index < 0) {
        InterlockedAdd64(&g_skippedEntries, count);
        return 0;  // the engine skips its loop: no entry has the name
    }
    InterlockedAdd64(&g_skippedEntries, index);
    *out = const_cast<Pair*>(table) + index;
    return count - index;
}

int EntityTable(void* self, Pair** out, const char* name) {
    int count = reinterpret_cast<TableFn>((*static_cast<void***>(self))[0x138 / 8])(self, out);
    return Adjust(out, count, name);
}
int ComponentTable(void* self, Pair** out, const char* name) {
    int count = reinterpret_cast<TableFn>((*static_cast<void***>(self))[0x58 / 8])(self, out);
    return Adjust(out, count, name);
}

uint8_t* AllocNear(uint8_t* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity, base = reinterpret_cast<uintptr_t>(target) & ~(step - 1);
    for (uintptr_t d = step; d < 0x70000000; d += step)
        for (int side = 0; side < 2; ++side) {
            uintptr_t c = side ? base + d : base - d;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(c), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
    return nullptr;
}

// call rel32 to `stub`, then a nop, for the 6 bytes at `site`.
struct Patch {
    uint8_t* site;
    uint8_t code[6];
};
bool Prepare(Patch& patch, uint8_t* site, const uint8_t* stub) {
    int64_t rel = stub - (site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;
    const uint8_t code[6] = {0xe8, 0, 0, 0, 0, 0x90};
    int32_t rel32 = static_cast<int32_t>(rel);
    patch.site = site;
    memcpy(patch.code, code, 6);
    memcpy(patch.code + 1, &rel32, 4);
    return true;
}

// Writes the patches while no other thread of the process runs (the way MinHook installs a hook).
// A thread that runs the function during the write could read half of the old 6 bytes and half of
// the new. So the other threads are suspended first. A suspended thread can also stand inside
// the 6 bytes (the component place is two instructions): its next instruction would start in the
// middle of the new call. Then all threads run on for a moment and the write is tried again. A
// thread that stands at the first byte, or that is inside the called function and returns to the
// byte after the 6, runs right with the old and with the new bytes.
//
// Nothing between the first suspend and the last resume uses the heap or the log: a suspended
// thread can hold their locks. A thread that starts after the list was made is not suspended (the
// same limit as MinHook has).
bool WritePatches(const Patch* patches, int count) {
    constexpr int kMaxThreads = 1024, kTries = 50;
    static DWORD ids[kMaxThreads];
    static HANDLE threads[kMaxThreads];
    for (int attempt = 0; attempt < kTries; ++attempt) {
        int idCount = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        DWORD process = GetCurrentProcessId(), me = GetCurrentThreadId();
        if (Thread32First(snapshot, &te)) {
            do {
                if (te.th32OwnerProcessID == process && te.th32ThreadID != me && idCount < kMaxThreads) ids[idCount++] = te.th32ThreadID;
                te.dwSize = sizeof te;
            } while (Thread32Next(snapshot, &te));
        }
        CloseHandle(snapshot);

        int threadCount = 0;
        bool inside = false;
        for (int i = 0; i < idCount; ++i) {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
            if (!thread) continue;  // (ended since the list was made)
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread);
                continue;
            }
            threads[threadCount++] = thread;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread, &context)) {
                inside = true;  // not known where it stands: not written this time
                continue;
            }
            for (int k = 0; k < count; ++k) {
                auto site = reinterpret_cast<uintptr_t>(patches[k].site);
                if (context.Rip > site && context.Rip < site + 6) inside = true;
            }
        }
        bool written = false;
        if (!inside) {
            written = true;
            for (int k = 0; k < count; ++k) {
                DWORD old;
                if (!VirtualProtect(patches[k].site, 6, PAGE_EXECUTE_READWRITE, &old)) {
                    written = false;  // (an earlier patch stays: each one is complete on its own)
                    break;
                }
                memcpy(patches[k].site, patches[k].code, 6);
                VirtualProtect(patches[k].site, 6, old, &old);
                FlushInstructionCache(GetCurrentProcess(), patches[k].site, 6);
            }
        }
        for (int i = 0; i < threadCount; ++i) {
            ResumeThread(threads[i]);
            CloseHandle(threads[i]);
        }
        if (!inside) return written;
        Sleep(1);
    }
    return false;
}

}  // namespace

bool Install(HMODULE engine, bool verify) {
    auto* fn = static_cast<uint8_t*>(sites::Find(engine, kFunctionByName));
    if (!fn) {
        Log("funclookup: function search not found (game version changed?); disabled");
        return false;
    }
    if (Fnv(fn + kSiteEntity, 6) != kHashEntity || Fnv(fn + kSiteComponent, 6) != kHashComponent) {
        Log("funclookup: the table requests are not where they were (game version changed?); disabled");
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(engine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(engine) + dos->e_lfanew);
    g_imageLo = reinterpret_cast<uintptr_t>(engine);
    g_imageHi = g_imageLo + nt->OptionalHeader.SizeOfImage;
    uint8_t* stubs = AllocNear(fn, 64);
    if (!stubs) {
        Log("funclookup: no memory near the engine; disabled");
        return false;
    }
    // Entity table: the name is in r15. Component table: the object is in rsi, the name at [rbp+0x6f].
    uint8_t* p = stubs;
    uint8_t* entityStub = p;
    const uint8_t a[] = {0x4d, 0x89, 0xf8, 0x48, 0xb8};  // mov r8, r15; mov rax, imm64
    memcpy(p, a, sizeof a), p += sizeof a;
    void* target = reinterpret_cast<void*>(&EntityTable);
    memcpy(p, &target, 8), p += 8;
    *p++ = 0xff, *p++ = 0xe0;  // jmp rax
    uint8_t* componentStub = p;
    const uint8_t b[] = {0x48, 0x89, 0xf1, 0x4c, 0x8b, 0x45, 0x6f, 0x48, 0xb8};  // mov rcx, rsi; mov r8, [rbp+0x6f]; mov rax, imm64
    memcpy(p, b, sizeof b), p += sizeof b;
    target = reinterpret_cast<void*>(&ComponentTable);
    memcpy(p, &target, 8), p += 8;
    *p++ = 0xff, *p++ = 0xe0;
    FlushInstructionCache(GetCurrentProcess(), stubs, 64);
    g_verify = verify;
    Patch patches[2];
    bool ok = Prepare(patches[0], fn + kSiteEntity, entityStub) && Prepare(patches[1], fn + kSiteComponent, componentStub) && WritePatches(patches, 2);
    Log("funclookup: function tables %s%s", ok ? "start at the remembered entry" : "NOT redirected",
        verify ? " (verify: the engine's search runs in full)" : "");
    return ok;
}

void SetActive(bool active) { g_active = active; }

void LogUse() {
    static int calls = 0;  // (called once per second: one line per 30 s)
    if (++calls % 30) return;
    LONG64 n = g_remembered + g_searched;
    if (!n) return;
    Log("funclookup: %lld table requests from a remembered entry, %lld searched, %lld compares saved, %lld tables not constant, %lld long names",
        g_remembered, g_searched, g_skippedEntries, g_notConstant, g_longNames);
    if (g_verify) Log("funclookup: verify: %lld remembered entries differ from a fresh search", g_differences);
}

}  // namespace tscfix::funclookup
