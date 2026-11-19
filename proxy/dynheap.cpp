// [experiments] dynheap_probe=1 (measurement only): use of the engine's "Dynamic" heap.
//
// The Dynamic heap is a fixed 4 MB block with a Doug Lea allocator inside (cFxMemoryHeapDougLea).
// It does not keep the "in use" count the other heaps keep, so the probe counts at its own
// functions: bytes in live blocks (from the allocator's block header), the highest value, the
// calls per kind, and calls that ran while another thread was inside one. The heap sets a global
// "current arena" around each call and takes no lock, so two threads inside it at once is a fault
// in waiting.
#include "dynheap.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "engine_sites.h"
#include "log.h"

namespace tscfix::dynheap {
namespace {

using AllocFn = void* (*)(void* heap, size_t size);
using FreeFn = void (*)(void* heap, void* p);
using ReallocFn = void* (*)(void* heap, void* p, size_t a3, size_t size);
using MemalignFn = void* (*)(void* heap, size_t size, size_t align);
AllocFn g_alloc = nullptr;
FreeFn g_free = nullptr;
using FreeAlignedFn = void (*)(void* heap, void* p, void* a3, void* a4);  // (further arguments, if any, go through)
FreeAlignedFn g_freeAligned = nullptr;
ReallocFn g_realloc = nullptr;
MemalignFn g_memalign = nullptr;

// One row per heap object of this class (the functions serve every Doug Lea heap of the engine).
struct Row {
    std::atomic<void*> heap{nullptr};
    std::atomic<int64_t> live{0}, peak{0};
    std::atomic<uint64_t> allocs{0}, frees{0}, failed{0};
};
Row g_rows[8];
std::atomic<uint64_t> g_overlaps{0};
Row& RowOf(void* heap) {
    for (Row& r : g_rows) {
        void* h = r.heap.load(std::memory_order_relaxed);
        if (h == heap) return r;
        if (!h && r.heap.compare_exchange_strong(h, heap)) return r;
        if (h == heap) return r;
    }
    return g_rows[7];
}
std::atomic<int> g_inside{0};
std::atomic<DWORD> g_threads[8];
bool g_installed = false;

// The allocator's header before a block: its size with flags in the low three bits.
size_t BlockSize(void* p) {
    if (!p) return 0;
    size_t head;
    memcpy(&head, static_cast<uint8_t*>(p) - 8, 8);
    return head & ~size_t{7};
}
void Add(Row& r, int64_t bytes) {
    int64_t now = r.live.fetch_add(bytes) + bytes;
    int64_t peak = r.peak.load();
    while (now > peak && !r.peak.compare_exchange_weak(peak, now)) {
    }
}
struct Inside {
    Inside() {
        if (g_inside.fetch_add(1) > 0) g_overlaps.fetch_add(1, std::memory_order_relaxed);
        DWORD me = GetCurrentThreadId();
        for (auto& t : g_threads) {
            DWORD v = t.load(std::memory_order_relaxed);
            if (v == me) break;
            if (v == 0 && t.compare_exchange_strong(v, me)) break;
        }
    }
    ~Inside() { g_inside.fetch_sub(1); }
};

void* Alloc(void* heap, size_t size) {
    Inside in;
    void* p = g_alloc(heap, size);
    Row& r = RowOf(heap);
    r.allocs.fetch_add(1, std::memory_order_relaxed);
    if (p) Add(r, static_cast<int64_t>(BlockSize(p)));
    else r.failed.fetch_add(1);
    return p;
}
void Free(void* heap, void* p) {
    Inside in;
    Row& r = RowOf(heap);
    if (p) Add(r, -static_cast<int64_t>(BlockSize(p)));
    r.frees.fetch_add(1, std::memory_order_relaxed);
    g_free(heap, p);
}
void* Realloc(void* heap, void* p, size_t a3, size_t size) {
    Inside in;
    int64_t before = static_cast<int64_t>(BlockSize(p));
    void* q = g_realloc(heap, p, a3, size);
    Row& r = RowOf(heap);
    if (q) Add(r, static_cast<int64_t>(BlockSize(q)) - before);
    else r.failed.fetch_add(1);
    return q;
}
void* Memalign(void* heap, size_t size, size_t align) {
    Inside in;
    void* p = g_memalign(heap, size, align);
    Row& r = RowOf(heap);
    r.allocs.fetch_add(1, std::memory_order_relaxed);
    // (an aligned block can sit inside a larger one: the header before it is still the allocator's)
    if (p) Add(r, static_cast<int64_t>(BlockSize(p)));
    else r.failed.fetch_add(1);
    return p;
}
void FreeAligned(void* heap, void* p, void* a3, void* a4) {
    Inside in;
    Row& r = RowOf(heap);
    if (p) Add(r, -static_cast<int64_t>(BlockSize(p)));
    r.frees.fetch_add(1, std::memory_order_relaxed);
    g_freeAligned(heap, p, a3, a4);
}

bool Hook(HMODULE engine, engine::Site site, void* detour, void** original) {
    uintptr_t rva = engine::Resolved(engine)[site];
    if (!rva) return false;
    void* target = reinterpret_cast<uint8_t*>(engine) + rva;
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool Install(HMODULE engine) {
    MH_Initialize();
    bool ok = Hook(engine, engine::DlAlloc, reinterpret_cast<void*>(&Alloc), reinterpret_cast<void**>(&g_alloc));
    ok &= Hook(engine, engine::DlFree, reinterpret_cast<void*>(&Free), reinterpret_cast<void**>(&g_free));
    ok &= Hook(engine, engine::DlRealloc, reinterpret_cast<void*>(&Realloc), reinterpret_cast<void**>(&g_realloc));
    ok &= Hook(engine, engine::DlMemalign, reinterpret_cast<void*>(&Memalign), reinterpret_cast<void**>(&g_memalign));
    ok &= Hook(engine, engine::DlFreeAligned, reinterpret_cast<void*>(&FreeAligned), reinterpret_cast<void**>(&g_freeAligned));
    g_installed = ok;
    Log("dynamic heap probe: %s", ok ? "on" : "NOT fully installed");
    return ok;
}

void LogUse(const char* tag) {
    if (!g_installed) return;
    int threads = 0;
    for (auto& th : g_threads) threads += th.load() != 0;
    for (Row& r : g_rows) {
        auto* heap = static_cast<uint8_t*>(r.heap.load());
        if (!heap) continue;
        char name[0x41] = "?";
        uint64_t size = 0;
        __try {
            memcpy(name, heap + 0x10, 0x40);
            memcpy(&size, heap + 0x98, 8);  // the block the heap took from its parent
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        name[0x40] = 0;
        Log("%s: Doug Lea heap \"%s\" (probe): %.1f KB in live blocks (highest %.1f KB) of %.1f KB, %llu allocations, %llu frees, %llu failed",
            tag, name, r.live.load() / 1024.0, r.peak.load() / 1024.0, size / 1024.0, static_cast<unsigned long long>(r.allocs.load()),
            static_cast<unsigned long long>(r.frees.load()), static_cast<unsigned long long>(r.failed.load()));
    }
    Log("%s: Doug Lea heaps (probe): used by %d thread(s), %llu calls while another thread was inside one", tag, threads,
        static_cast<unsigned long long>(g_overlaps.load()));
}

}  // namespace tscfix::dynheap
