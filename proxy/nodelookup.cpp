#include "nodelookup.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

// Scene node by name (game version v79.0a): (node table, name holder, max length) -> index of the
// first node whose name equals the name without regard to case, or -1. The engine compares the
// name with every node of the model on every call, and reads the name again for each node. It
// also releases the name holder before it returns.
//
// Here the answer for a (table, name) pair is remembered. A remembered index is used only when
// the table still has the same node array and count, and the node at that index still has that
// name. Otherwise the engine's own search runs. The engine function still makes the call that
// releases the name: on a hit it gets a table of one node, the remembered one.
namespace tscfix::nodelookup {
namespace {

constexpr sites::Fingerprint kNodeIndex = {0xa6eabb0c1b71a190ull, 32};
constexpr size_t kTableCount = 0xc, kTableNodes = 0x80, kStringChars = 0x14;
using NodeIndexFn = int (*)(uint8_t* table, void** name, int maxLength);
NodeIndexFn g_nodeIndex = nullptr;
volatile bool g_active = false;
bool g_verify = false;

struct Entry {
    uint8_t* table;
    void* nodes;
    int count;
    int index;
    uint64_t hash;
};
constexpr size_t kSlots = 4096;  // power of two
Entry g_entries[kSlots];
SRWLOCK g_lock = SRWLOCK_INIT;  // the lookup runs on the game thread and on render workers
volatile LONG64 g_hits = 0, g_misses = 0, g_stale = 0, g_differences = 0;

const char* Chars(void* string) { return string ? static_cast<const char*>(string) + kStringChars : ""; }

uint64_t Hash(const uint8_t* table, const char* name) {
    uint64_t h = 0xCBF29CE484222325ull ^ reinterpret_cast<uintptr_t>(table);
    for (; *name; ++name) {
        unsigned char c = static_cast<unsigned char>(*name);
        if (c >= 'A' && c <= 'Z') c += 32;  // the engine compares without regard to case
        h = (h ^ c) * 0x100000001B3ull;
    }
    return h;
}

// The remembered index for this table and name, or -1.
int Remembered(uint8_t* table, const char* name, int maxLength, uint64_t hash) {
    int count;
    void** nodes;
    memcpy(&count, table + kTableCount, 4);
    memcpy(&nodes, table + kTableNodes, 8);
    AcquireSRWLockShared(&g_lock);
    Entry e = g_entries[hash & (kSlots - 1)];
    ReleaseSRWLockShared(&g_lock);
    if (e.hash != hash || e.table != table || e.nodes != nodes || e.count != count || e.index >= count) return -1;
    if (_strnicmp(Chars(nodes[e.index]), name, static_cast<size_t>(maxLength)) != 0) {
        InterlockedIncrement64(&g_stale);
        return -1;
    }
    return e.index;
}

void Remember(uint8_t* table, uint64_t hash, int index) {
    Entry e{table, nullptr, 0, index, hash};
    memcpy(&e.count, table + kTableCount, 4);
    memcpy(&e.nodes, table + kTableNodes, 8);
    AcquireSRWLockExclusive(&g_lock);
    g_entries[hash & (kSlots - 1)] = e;
    ReleaseSRWLockExclusive(&g_lock);
}

void Report() {
    LONG64 n = g_hits + g_misses;
    if (n % 200000) return;
    Log("nodelookup: %lld remembered, %lld searched, %lld entries out of date", g_hits, g_misses, g_stale);
    if (g_verify) Log("nodelookup: verify: %lld answers differ from the engine's search", g_differences);
}

int NodeIndex(uint8_t* table, void** name, int maxLength) {
    if (!g_active || !table || !name || maxLength <= 0) return g_nodeIndex(table, name, maxLength);
    const char* chars = Chars(*name);
    uint64_t hash = Hash(table, chars);
    int index = Remembered(table, chars, maxLength, hash);
    if (g_verify) {  // the engine's search decides; the remembered answer is only compared
        int real = g_nodeIndex(table, name, maxLength);
        if (index >= 0 && index != real) InterlockedIncrement64(&g_differences);
        InterlockedIncrement64(index >= 0 ? &g_hits : &g_misses);
        if (index < 0 && real >= 0) Remember(table, hash, real);
        Report();
        return real;
    }
    if (index >= 0) {
        // One node for the engine function: it compares once, releases the name, returns 0.
        void** nodes;
        memcpy(&nodes, table + kTableNodes, 8);
        alignas(8) uint8_t one[kTableNodes + 8] = {};
        const int count = 1;
        void** at = nodes + index;
        memcpy(one + kTableCount, &count, 4);
        memcpy(one + kTableNodes, &at, 8);
        int r = g_nodeIndex(one, name, maxLength);
        InterlockedIncrement64(&g_hits);
        Report();
        return r == 0 ? index : -1;  // (-1 cannot happen: the same compare passed just above)
    }
    index = g_nodeIndex(table, name, maxLength);  // (releases the name: the hash was taken before)
    InterlockedIncrement64(&g_misses);
    if (index >= 0) Remember(table, hash, index);
    Report();
    return index;
}

}  // namespace

bool Install(HMODULE engine, bool verify) {
    void* target = sites::Find(engine, kNodeIndex);
    if (!target) {
        Log("nodelookup: node search not found (game version changed?); disabled");
        return false;
    }
    g_verify = verify;
    MH_Initialize();
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&NodeIndex), reinterpret_cast<void**>(&g_nodeIndex)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    Log("nodelookup: node search %s%s", ok ? "replaced" : "hook failed", verify ? " (verify: the engine's answer is used)" : "");
    return ok;
}

void SetActive(bool active) { g_active = active; }

}  // namespace tscfix::nodelookup
