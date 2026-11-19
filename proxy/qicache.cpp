// Per-thread cache for the entity container-interface query.
//
// Entity searches (script calls addressed to a child or to "*") ask every node they visit for
// its container interface. On a leaf the answer is "none", reached only after every component
// was asked. The answer depends on the entity's class and component list, which are fixed once
// loaded. The cache maps entity -> answer, valid while the entity's vtable and component vector
// (+0x30 / +0x38) are unchanged; one table per thread, nothing shared.
//
// Which interface is the container one is learned at runtime: the first query made from inside
// the fingerprinted entity search names it. Until then nothing is cached.
#include "qicache.h"

#include <MinHook.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <new>

#include "log.h"
#include "entitysearch.h"
#include "sites.h"

namespace tscfix::qicache {
namespace {

constexpr sites::Fingerprint kEntityQuery = {0xd694dc2ca4d33ce7ull, 16};
constexpr sites::Fingerprint kEntitySearch = {0xa625e0483501d932ull, 24};
constexpr size_t kComponentsBegin = 0x30, kComponentsEnd = 0x38;

using QueryFn = void* (*)(void* entity, const uint64_t* iid);
QueryFn g_query = nullptr;
uintptr_t g_search = 0;           // start of the entity search
uint64_t g_iid[2] = {};           // the container interface id, once learned
volatile bool g_learned = false;
volatile bool g_active = false;
bool g_verify = false;
volatile long g_mismatches = 0;

struct Entry {
    void* entity;
    void* vtable;
    void* begin;
    void* end;
    void* answer;
};
constexpr size_t kSlots = 1 << 14;
struct Table {
    Entry e[kSlots];
    uint64_t hits = 0, misses = 0;
};
thread_local Table* t_table = nullptr;

// A query made by the entity search names the container interface.
void Learn(const uint64_t* iid, void* ret) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(ret), &base, nullptr);
    if (!f || base + f->BeginAddress != g_search) return;
    g_iid[0] = iid[0], g_iid[1] = iid[1];
    g_learned = true;
    Log("qicache: container interface learned from the entity search");
}

void* Query(void* entity, const uint64_t* iid) {
    if (!g_active || !entity || !iid) return g_query(entity, iid);
    if (!g_learned) {
        Learn(iid, _ReturnAddress());
        return g_query(entity, iid);
    }
    if (iid[0] != g_iid[0] || iid[1] != g_iid[1]) return g_query(entity, iid);
    if (!t_table) t_table = new (std::nothrow) Table();
    if (!t_table) return g_query(entity, iid);
    auto* o = static_cast<uint8_t*>(entity);
    void *vt, *begin, *end;
    memcpy(&vt, o, 8), memcpy(&begin, o + kComponentsBegin, 8), memcpy(&end, o + kComponentsEnd, 8);
    Entry& e = t_table->e[(reinterpret_cast<uintptr_t>(entity) >> 4) * 0x9E3779B97F4A7C15ull >> 50 & (kSlots - 1)];
    if (e.entity == entity && e.vtable == vt && e.begin == begin && e.end == end) {
        ++t_table->hits;
        if (g_verify && (t_table->hits & 63) == 0) {
            void* real = g_query(entity, iid);
            if (real != e.answer) {
                long n = InterlockedIncrement(&g_mismatches);
                if (n <= 20) Log("qicache: verify mismatch on %p: cached %p, engine %p", entity, e.answer, real);
                e.answer = real;
            }
        }
        if ((t_table->hits & 0x3fffff) == 0)
            Log("qicache: thread %lu: %llu hits, %llu misses, %ld mismatches", GetCurrentThreadId(),
                static_cast<unsigned long long>(t_table->hits), static_cast<unsigned long long>(t_table->misses),
                g_mismatches);
        return e.answer;
    }
    ++t_table->misses;
    void* answer = g_query(entity, iid);
    e = {entity, vt, begin, end, answer};
    return answer;
}

}  // namespace

bool Install(HMODULE engine, bool verify) {
    void* q = sites::Find(engine, kEntityQuery);
    void* search = sites::Find(engine, kEntitySearch);
    if (!search) search = entitysearch::SearchFunction();  // (hooked first by the entity search cache)
    if (!q || !search) {
        Log("qicache: entity query or search not found (game version changed?); disabled");
        return false;
    }
    g_search = reinterpret_cast<uintptr_t>(search);
    g_verify = verify;
    MH_Initialize();
    bool ok = MH_CreateHook(q, reinterpret_cast<void*>(&Query), reinterpret_cast<void**>(&g_query)) == MH_OK &&
              MH_EnableHook(q) == MH_OK;
    Log("qicache: container query cache %s%s", ok ? "installed" : "NOT installed", verify ? " (verify)" : "");
    return ok;
}

void SetActive(bool on) { g_active = on; }

}  // namespace tscfix::qicache
