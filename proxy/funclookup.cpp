#include "funclookup.h"

#include <MinHook.h>

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

// call rel32 to `stub`, then a nop, over the 6 bytes at `site`.
bool Redirect(uint8_t* site, const uint8_t* stub) {
    int64_t rel = stub - (site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;
    uint8_t code[6] = {0xe8, 0, 0, 0, 0, 0x90};
    int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    DWORD old;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, code, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return true;
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
    bool ok = Redirect(fn + kSiteEntity, entityStub) && Redirect(fn + kSiteComponent, componentStub);
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
