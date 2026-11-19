#include "refcensus.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

#include "log.h"
#include "overlap.h"

namespace tscfix::refcensus {
namespace {

constexpr int kMaxSlots = 512;
using AddRefFn = void (*)(void*);
AddRefFn g_original[kMaxSlots] = {};
int g_slotCount = 0;
uint8_t* g_base = nullptr;
bool g_installed = false;

// (vtable the call came through, thread) -> calls while a worker draws.
struct Cell {
    std::atomic<uintptr_t> vtable{0};
    std::atomic<DWORD> thread{0};
    std::atomic<uint32_t> calls{0};
};
constexpr uint32_t kCells = 8192;
Cell g_cells[kCells];

void Note(void* self) {
    if (!overlap::Overlapping()) return;
    uintptr_t vtable = *static_cast<uintptr_t*>(self);
    DWORD me = GetCurrentThreadId();
    uint32_t i = static_cast<uint32_t>((vtable >> 3) * 2654435761u + me * 40503u) % kCells;
    for (uint32_t n = 0; n < kCells; ++n, i = (i + 1) % kCells) {
        Cell& c = g_cells[i];
        uintptr_t v = c.vtable.load(std::memory_order_relaxed);
        if (v == 0) {
            if (c.vtable.compare_exchange_strong(v, vtable)) {
                c.thread.store(me, std::memory_order_relaxed);
                v = vtable;
            }
        }
        if (v != vtable) continue;
        DWORD t = c.thread.load(std::memory_order_relaxed);
        if (t == 0) continue;  // another thread claims this cell right now: use the next one
        if (t != me) continue;
        c.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

template <int K>
void Detour(void* self) {
    Note(self);
    g_original[K](self);
}

template <size_t... K>
constexpr void MakeTable(AddRefFn* table, std::index_sequence<K...>) {
    ((table[K] = &Detour<static_cast<int>(K)>), ...);
}
AddRefFn g_detours[kMaxSlots];

// "inc dword [rcx+disp8]; ret" or "inc dword [rcx+disp32]; ret".
bool PlainAddRef(const uint8_t* f) {
    return (f[0] == 0xff && f[1] == 0x41 && f[3] == 0xc3) || (f[0] == 0xff && f[1] == 0x81 && f[6] == 0xc3);
}

// "sub rcx, imm; jmp rel32" to a plain AddRef: the AddRef of a second base class.
bool ThunkToPlainAddRef(const uint8_t* f, const uint8_t* lo, const uint8_t* hi) {
    int at;
    if (f[0] == 0x48 && f[1] == 0x83 && f[2] == 0xe9 && f[4] == 0xe9) at = 4;
    else if (f[0] == 0x48 && f[1] == 0x81 && f[2] == 0xe9 && f[7] == 0xe9) at = 7;
    else return false;
    int32_t rel;
    memcpy(&rel, f + at + 1, 4);
    const uint8_t* target = f + at + 5 + rel;
    return target >= lo && target < hi - 8 && PlainAddRef(target);
}

// The class name of a vtable (run-time type information), and the sub-object offset.
const char* ClassOf(uintptr_t vtable, uint32_t* offset) {
    __try {
        const DWORD* locator = *reinterpret_cast<const DWORD**>(vtable - 8);
        if (locator[0] != 1) return "?";
        *offset = locator[1];
        return reinterpret_cast<const char*>(g_base + locator[3] + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

}  // namespace

bool Install(HMODULE engine) {
    g_base = reinterpret_cast<uint8_t*>(engine);
    MakeTable(g_detours, std::make_index_sequence<kMaxSlots>{});
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* textLo = nullptr;
    const uint8_t* textHi = nullptr;
    uint8_t* dataLo = nullptr;
    uint8_t* dataHi = nullptr;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!memcmp(sec[s].Name, ".text", 5)) textLo = g_base + sec[s].VirtualAddress, textHi = textLo + sec[s].Misc.VirtualSize;
        if (!memcmp(sec[s].Name, ".rdata", 6)) dataLo = g_base + sec[s].VirtualAddress, dataHi = dataLo + sec[s].Misc.VirtualSize;
    }
    if (!textLo || !dataLo) return false;
    int plain = 0, thunks = 0, skipped = 0;
    for (uint8_t* p = dataLo; p + 8 <= dataHi; p += 8) {
        const uint8_t* f = *reinterpret_cast<const uint8_t**>(p);
        if (f < textLo || f >= textHi - 16) continue;
        bool isPlain = PlainAddRef(f);
        if (!isPlain && !ThunkToPlainAddRef(f, textLo, textHi)) continue;
        if (g_slotCount == kMaxSlots) {
            ++skipped;
            continue;
        }
        DWORD old;
        if (!VirtualProtect(p, 8, PAGE_READWRITE, &old)) {
            ++skipped;
            continue;
        }
        g_original[g_slotCount] = reinterpret_cast<AddRefFn>(const_cast<uint8_t*>(f));
        *reinterpret_cast<AddRefFn*>(p) = g_detours[g_slotCount++];
        VirtualProtect(p, 8, old, &old);
        isPlain ? ++plain : ++thunks;
    }
    g_installed = true;
    Log("refcount census: counting detours in %d vtable slots with a plain AddRef and %d with a thunk to one (%d not patched)", plain,
        thunks, skipped);
    return true;
}

void LogUse() {
    if (!g_installed) return;
    // Gather per vtable: threads and calls.
    struct Row {
        uintptr_t vtable;
        int threads;
        uint32_t most, second;
        DWORD mostThread, secondThread;
    };
    static Row rows[1024];
    int n = 0;
    for (const Cell& c : g_cells) {
        uintptr_t v = c.vtable.load(std::memory_order_relaxed);
        uint32_t calls = c.calls.load(std::memory_order_relaxed);
        if (!v || !calls) continue;
        DWORD t = c.thread.load(std::memory_order_relaxed);
        Row* r = nullptr;
        for (int i = 0; i < n; ++i)
            if (rows[i].vtable == v) r = &rows[i];
        if (!r) {
            if (n == 1024) continue;
            r = &rows[n++];
            *r = {v, 0, 0, 0, 0, 0};
        }
        ++r->threads;
        if (calls > r->most) r->second = r->most, r->secondThread = r->mostThread, r->most = calls, r->mostThread = t;
        else if (calls > r->second) r->second = calls, r->secondThread = t;
    }
    int shared = 0;
    for (int i = 0; i < n; ++i) shared += rows[i].threads >= 2;
    Log("refcount census: %d classes had count changes while a worker drew; %d of them from two or more threads", n, shared);
    for (int i = 0; i < n; ++i) {
        const Row& r = rows[i];
        if (r.threads < 2) continue;
        uint32_t offset = 0;
        const char* name = ClassOf(r.vtable, &offset);
        Log("refcount census:   %s +0x%x (vtable rva %llx): %d threads; thread %lu: %u, thread %lu: %u", name, offset,
            static_cast<unsigned long long>(r.vtable - reinterpret_cast<uintptr_t>(g_base)), r.threads, r.mostThread, r.most,
            r.secondThread, r.second);
    }
}

}  // namespace tscfix::refcensus
