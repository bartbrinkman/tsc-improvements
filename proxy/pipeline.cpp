#include "lodprobe.h"
#include "refcensus.h"
#include "pipeline.h"

#include <MinHook.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "config.h"
#include "hde/hde64.h"
#include "cowtable.h"
#include "engine_sites.h"
#include "sites.h"
#include "glitch.h"
#include "log.h"
#include "d3dlayer.h"
#include <algorithm>

#include "offload.h"
#include "tracer.h"
#include "overlap.h"
#include "profiler.h"
#include "rtdevice.h"
#include "overlapstate.h"
#include "binder.h"
#include "trace.h"

namespace tscfix::pipeline {
namespace {

// Appends to buf (holding n chars) without ever overflowing: sprintf_s would end the process
// through the CRT's invalid-parameter handler (exit 0xC0000409) when a long line does not fit.
int Append(char* buf, size_t size, int n, const char* fmt, ...) {
    if (n < 0 || static_cast<size_t>(n) >= size) return static_cast<int>(size ? size - 1 : 0);
    va_list args;
    va_start(args, fmt);
    int w = _vsnprintf_s(buf + n, size - n, _TRUNCATE, fmt, args);
    va_end(args);
    return w < 0 ? static_cast<int>(size - 1) : n + w;
}

uintptr_t g_base = 0;
// Engine locations used inline, resolved at start-up (engine_sites.h).
uintptr_t kLockWrapperRet0 = 0, kLockWrapperRet1 = 0, kBatchFillRet0 = 0, kBatchFillRet1 = 0, kBatchFillRet2 = 0, kShadowTexLockRet0 = 0, kShadowTexLockRet1 = 0;
uintptr_t kInlineRets[16] = {};

// Growable thread-local byte buffer.
struct Buffer {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint8_t* Get(size_t need) {
        if (size < need) {
            size_t n = need * 2 < 4096 ? 4096 : need * 2;
            auto* p = static_cast<uint8_t*>(_aligned_malloc(n, 16));
            if (!p) return nullptr;
            _aligned_free(data);
            data = p;
            size = n;
        }
        return data;
    }
};

// --- concurrent-use counters --------------------------------------------------------------------
// Engine structures that only one thread may be inside at a time carry an owner word at the
// hooks that enter them. A second thread entering while another is inside is counted and
// reported (LogSharedUse, every 600 shadow passes; the first few with both thread ids), so a
// conflict shows up as a number in the log, not as a crash some frames later.
enum SharedUse { kUseInstancing, kUseQueueFlush, kUseRsm, kUseTileWalk, kUseCount };
const char* const kUseNames[kUseCount] = {"instanced-geometry queue", "sorted queue flushed while filled",
                                          "track batch lists", "main-view tile walk"};
struct alignas(64) UseWord {
    volatile LONG owner = 0;
    volatile LONG clashes = 0;
    volatile LONG entries = 0;
};
UseWord g_use[kUseCount];
void NoteClash(SharedUse what, DWORD other) {
    if (InterlockedIncrement(&g_use[what].clashes) <= 5)
        Log("research overlap: CONFLICT: thread %lu entered the %s while thread %lu was inside", GetCurrentThreadId(),
            kUseNames[what], other);
}
struct UseScope {
    UseWord& u;
    bool outer;
    explicit UseScope(SharedUse what) : u(g_use[what]) {
        LONG me = static_cast<LONG>(GetCurrentThreadId());
        LONG prev = InterlockedExchange(&u.owner, me);
        outer = prev != me;
        InterlockedIncrement(&u.entries);
        if (prev && prev != me) NoteClash(what, static_cast<DWORD>(prev));
    }
    ~UseScope() {
        if (outer) InterlockedExchange(&u.owner, 0);
    }
};
// Sorted queues: fills are serialized by the queue lock; a flush is not (it draws). The queue a
// thread is flushing is published so a fill of the same queue by another thread is counted.
volatile LONG g_firstUses = 0, g_firstUseWaits = 0;  // material first draws (see MaterialDraw)
extern CRITICAL_SECTION g_serviceWaitedLock;
extern volatile LONG g_techniquePauses, g_serviceBypasses;
void* volatile g_flushing[4] = {};
volatile LONG g_flushingBy[4] = {};
// (measurement) How long threads wait for the locks that keep draws apart, per lock: time and
// count of the acquisitions that had to wait, per 600 shadow passes. A lock that makes two
// threads take turns shows here as milliseconds per frame.
enum LockId { kLockQueue, kLockMainInst, kLockNetwork, kLockTerrain, kLockOverride, kLockShared, kLockFirstUse,
              kLockBuild, kLockShadowInst, kLockService, kLockMainFlush, kLockZSort, kLockCount };
const char* const kLockNames[kLockCount] = {"sorted queue", "instanced queue (main view)", "network", "terrain",
                                            "material overrides", "other shared render", "material first use",
                                            "geometry build", "instanced queue (shadow)", "service", "main-view flush",
                                            "z-sorted queue"};
volatile LONG64 g_lockWaitTicks[kLockCount] = {};
volatile long g_lockWaits[kLockCount] = {};
struct LockWaitTimer {
    int id;
    LARGE_INTEGER t0;
    explicit LockWaitTimer(int lock) : id(lock) { QueryPerformanceCounter(&t0); }
    ~LockWaitTimer() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        InterlockedAdd64(&g_lockWaitTicks[id], t1.QuadPart - t0.QuadPart);
        InterlockedIncrement(&g_lockWaits[id]);
    }
};
inline void EnterTimed(CRITICAL_SECTION* cs, int id) {
    if (TryEnterCriticalSection(cs)) return;
    LockWaitTimer timer(id);
    EnterCriticalSection(cs);
}
inline void AcquireTimed(SRWLOCK* lock, int id) {
    if (TryAcquireSRWLockExclusive(lock)) return;
    LockWaitTimer timer(id);
    AcquireSRWLockExclusive(lock);
}
void LogLockWaits() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    char line[700];
    int n = 0;
    for (int i = 0; i < kLockCount; ++i) {
        LONG64 ticks = InterlockedExchange64(&g_lockWaitTicks[i], 0);
        long count = InterlockedExchange(&g_lockWaits[i], 0);
        if (count) n = Append(line, sizeof line, n, " %s %.2f ms (%.1f waits);", kLockNames[i], ticks * 1000.0 / f.QuadPart / 600.0, count / 600.0);
    }
    Log("research overlap: lock waits per shadow pass, all threads:%s", n ? line : " none");
}

void LogSharedUse() {
    char line[400];
    int n = 0;
    for (int i = 0; i < kUseCount; ++i)
        n = Append(line, sizeof line, n, " %s %ld of %ld;", kUseNames[i], InterlockedExchange(&g_use[i].clashes, 0),
                   InterlockedExchange(&g_use[i].entries, 0));
    Log("research overlap: concurrent use (conflicts of entries, last 600 shadow passes):%s", line);
    Log("research overlap: material first draws %ld, of which %ld waited for another thread's first draw; technique "
        "selections on the forked worker (replay thread parked) %ld; service calls by a worker the game thread was waiting for %ld",
        InterlockedExchange(&g_firstUses, 0), InterlockedExchange(&g_firstUseWaits, 0), InterlockedExchange(&g_techniquePauses, 0),
        InterlockedExchange(&g_serviceBypasses, 0));
}

// --- scenery tile walk -------------------------------------------------------------------------
// TileWalk (walker, a, b): the per-pass scenery tile walk, which writes into the tile records and
// the walker's visible-tile buffer. On the shadow thread it runs on private copies of all three.
// Walker (0x70 bytes, inside the scenery manager): +0 grid header, +8 visible buffer, +0x10
// count. Grid header (0x30): +0 tile array, +8 width, +0xc height. Tile record: 0x60 bytes.
uintptr_t kTileWalk = 0;
constexpr size_t kWalkerSize = 0x70, kGridSize = 0x30, kTileSize = 0x60;
using TileWalkFn = void (*)(void* walker, uint32_t a, uint32_t b);
TileWalkFn g_tileWalk = nullptr;
thread_local Buffer t_walker, t_grid, t_tiles, t_visible;

// (glitch detection) Tiles the shadow pass draws, per shadow update and walk: a tile drawn in the
// updates before and after but not in between is logged.
uintptr_t kTileDraw = 0;
using TileDrawFn = void (*)(void* tile, uint32_t a, uint32_t b, uint8_t inside);
TileDrawFn g_tileDraw = nullptr;
SRWLOCK g_tileLock = SRWLOCK_INIT;
struct LodSeen {  // the LOD context a shadow tile draw saw (binder +0x20 and its fields)
    uintptr_t obj;
    int32_t forced, bias;
    float scale, add;
    uint8_t flag;
    bool operator<(const LodSeen& o) const {
        return std::tie(obj, forced, bias, scale, add, flag) < std::tie(o.obj, o.forced, o.bias, o.scale, o.add, o.flag);
    }
};
// Shadow-role work per update (all shadow threads): sorted-queue inserts, instanced entries.
volatile long g_shadowInserts = 0, g_shadowInstEntries = 0;
// Instanced clones per update: taken with the build flag (+0x414) set, with no LOD slot mesh,
// queued (InstAdd), and drawn by the flush.
volatile long g_cloneDirty = 0, g_cloneNoSlots = 0, g_cloneQueued = 0, g_cloneDrawn = 0;
struct ShadowTiles {
    std::map<LodSeen, int> lods;
    long inserts = 0, instEntries = 0, dirty = 0, noSlots = 0, queued = 0, instDrawn = 0;
    int walks = 0;
    std::set<std::pair<int, uintptr_t>> drawn;  // (walk, tile)
};
ShadowTiles g_tileRing[3];
volatile long g_shadowWalk = -1;  // walk of the current shadow update
uint64_t g_tileUpdates = 0;
int g_tileDropsLogged = 0;
void TileDraw(void* tile, uint32_t a, uint32_t b, uint8_t inside) {
    if (overlapstate::ShadowRole() && g_shadowWalk >= 0) {
        AcquireSRWLockExclusive(&g_tileLock);
        g_tileRing[2].drawn.insert({static_cast<int>(g_shadowWalk), reinterpret_cast<uintptr_t>(tile)});
        LodSeen l{};
        if (uint8_t* bd = binder::Binder()) {
            uint8_t* lod = nullptr;
            memcpy(&lod, bd + 0x20, 8);
            l.obj = reinterpret_cast<uintptr_t>(lod);
            if (lod) {
                memcpy(&l.flag, lod + 0x121, 1), memcpy(&l.forced, lod + 0x128, 4), memcpy(&l.bias, lod + 0x144, 4);
                memcpy(&l.scale, lod + 0x148, 4), memcpy(&l.add, lod + 0x13c, 4);
            }
        }
        ++g_tileRing[2].lods[l];
        ReleaseSRWLockExclusive(&g_tileLock);
    }
    g_tileDraw(tile, a, b, inside);
}
// Shadow worker, at the start of a shadow update: the last one is complete.
void ShadowTilesNextUpdate() {
    AcquireSRWLockExclusive(&g_tileLock);
    ++g_tileUpdates;
    g_tileRing[2].inserts = InterlockedExchange(&g_shadowInserts, 0);
    g_tileRing[2].instEntries = InterlockedExchange(&g_shadowInstEntries, 0);
    g_tileRing[2].dirty = InterlockedExchange(&g_cloneDirty, 0);
    g_tileRing[2].noSlots = InterlockedExchange(&g_cloneNoSlots, 0);
    g_tileRing[2].queued = InterlockedExchange(&g_cloneQueued, 0);
    g_tileRing[2].instDrawn = InterlockedExchange(&g_cloneDrawn, 0);
    const ShadowTiles &A = g_tileRing[0], &B = g_tileRing[1], &C = g_tileRing[2];
    if (g_tileUpdates > 3 && A.walks == B.walks && B.walks == C.walks && A.walks > 0) {
        int dropped = 0, added = 0;
        int byWalk[8] = {};
        for (const auto& t : A.drawn)
            if (C.drawn.count(t) && !B.drawn.count(t)) ++dropped, ++byWalk[t.first & 7];
        for (const auto& t : B.drawn)
            if (!A.drawn.count(t) && !C.drawn.count(t)) ++added;
        if ((dropped || added) && g_tileDropsLogged++ < 200)
            Log("research overlap: shadow update %llu: %d tile(s) not drawn that the updates around it drew (walks "
                "0-3: %d %d %d %d), %d drawn only in it; tiles drawn %zu / %zu / %zu",
                static_cast<unsigned long long>(g_tileUpdates - 2), dropped, byWalk[0], byWalk[1], byWalk[2], byWalk[3],
                added, A.drawn.size(), B.drawn.size(), C.drawn.size());
    }
    if (g_tileUpdates > 3 && A.walks == B.walks && B.walks == C.walks && g_tileDropsLogged < 200 &&
        (B.queued < (std::min)(A.queued, C.queued) || B.instDrawn < (std::min)(A.instDrawn, C.instDrawn) || B.dirty || B.noSlots)) {
        ++g_tileDropsLogged;
        Log("research overlap: shadow update %llu: instanced entries %ld / %ld / %ld, queued %ld / %ld / %ld, drawn %ld / "
            "%ld / %ld, cloned dirty %ld / %ld / %ld, cloned without slots %ld / %ld / %ld (before / this / after)",
            static_cast<unsigned long long>(g_tileUpdates - 2), A.instEntries, B.instEntries, C.instEntries, A.queued,
            B.queued, C.queued, A.instDrawn, B.instDrawn, C.instDrawn, A.dirty, B.dirty, C.dirty, A.noSlots, B.noSlots, C.noSlots);
    }
    if (C.lods.size() > 1 && g_tileDropsLogged++ < 200) {
        char line[700];
        int n = 0;
        for (auto& [l, k] : C.lods)
            if (n < 640)
                n = Append(line, sizeof line, n, " [lod %llx flag %d forced %d bias %d scale %.3f add %.3f] x%d;",
                               static_cast<unsigned long long>(l.obj), l.flag, l.forced, l.bias, l.scale, l.add, k);
        Log("research overlap: shadow update %llu saw %zu LOD contexts:%s", static_cast<unsigned long long>(g_tileUpdates),
            C.lods.size(), line);
    }
    g_tileRing[0] = std::move(g_tileRing[1]);
    g_tileRing[1] = std::move(g_tileRing[2]);
    g_tileRing[2] = ShadowTiles{};
    g_shadowWalk = -1;
    ReleaseSRWLockExclusive(&g_tileLock);
}

// --- tile bounds, refreshed before the passes share the records -------------------------------
// Tiles flagged dirty (+0x2c) get their height range and margin (+0 / +4 / +8) refreshed by the
// main view's walk, in place. If the shadow walk copies the records meanwhile, a copy can hold a
// partial range (missing and extra shadow casters). (switch tilerefresh) RefreshTileBounds does
// the refresh for all dirty tiles on the render thread before the shadow pass starts, from the
// tile's object list (+0x30), so the main walk then finds none dirty.
volatile bool g_swTileRefresh = false;
uint8_t* volatile g_tileGrid = nullptr;  // grid header of the main view's walker (seen in a walk)
uint64_t g_tilesRefreshed = 0, g_refreshPasses = 0;
void RefreshTileBounds() {
    uint8_t* grid = g_tileGrid;
    if (!g_swTileRefresh || !grid) return;
    uint8_t* tiles;
    int32_t width, height;
    memcpy(&tiles, grid, 8), memcpy(&width, grid + 8, 4), memcpy(&height, grid + 0xc, 4);
    if (!tiles || width <= 0 || height <= 0) return;
    using VFloat = float (*)(void*);
    using VPtr = void* (*)(void*);
    auto vfn = [](void* obj, size_t off) { return (*static_cast<void***>(obj))[off / 8]; };
    ++g_refreshPasses;
    for (int64_t i = 0; i < int64_t(width) * height; ++i) {
        uint8_t* t = tiles + i * kTileSize;
        if (!t[0x2c]) continue;
        static bool first = true;
        if (first) first = false, Log("research overlap: tile bounds refresh ran (first dirty tile)");
        float lo = 3.4028235e+38f, hi = -3.4028235e+38f, margin = 0.0f;
        uint8_t* head;
        memcpy(&head, t + 0x30, 8);
        if (head) {
            uint8_t* node;
            memcpy(&node, head, 8);
            while (node && node != head) {
                void* obj;
                memcpy(&obj, node + 0x10, 8);
                if (obj) {
                    if (void* b = reinterpret_cast<VPtr>(vfn(obj, 0xa8))(obj)) {
                        float h = reinterpret_cast<VFloat>(vfn(b, 0x78))(b);
                        if (h < lo) lo = h;
                        if (h > hi) hi = h;
                        float r = reinterpret_cast<VFloat>(vfn(obj, 0x38))(obj);
                        if (r > margin) margin = r;
                    }
                }
                memcpy(&node, node, 8);
            }
        }
        memcpy(t, &lo, 4), memcpy(t + 4, &hi, 4), memcpy(t + 8, &margin, 4);
        t[0x2c] = 0;
        ++g_tilesRefreshed;
    }
    if (g_refreshPasses % 600 == 0)
        Log("research overlap: tile bounds refreshed before the shadow pass: %.1f tiles per pass",
            double(g_tilesRefreshed) / 600), g_tilesRefreshed = 0;
}

// (switch tilelock) the shadow walk copies the tile records only while no main-view walk runs.
volatile bool g_swTileLock = false;
SRWLOCK g_tileWalkLock = SRWLOCK_INIT;
void TileWalk(void* walker, uint32_t a, uint32_t b) {
    if (!overlapstate::ShadowRole()) {
        uint8_t* grid;
        memcpy(&grid, walker, 8);
        if (grid) g_tileGrid = grid;
        bool locked = g_swTileLock;
        if (locked) AcquireSRWLockExclusive(&g_tileWalkLock);
        UseScope use(kUseTileWalk);
        g_tileWalk(walker, a, b);
        if (locked) ReleaseSRWLockExclusive(&g_tileWalkLock);
        return;
    }
    if (g_tileDraw) {
        AcquireSRWLockExclusive(&g_tileLock);
        g_shadowWalk = g_tileRing[2].walks++;
        ReleaseSRWLockExclusive(&g_tileLock);
    }
    auto* w = static_cast<uint8_t*>(walker);
    uint8_t* grid;
    memcpy(&grid, w, 8);
    if (!grid) return g_tileWalk(walker, a, b);
    int32_t width, height;
    memcpy(&width, grid + 8, 4);
    memcpy(&height, grid + 0xc, 4);
    uint8_t* tiles;
    memcpy(&tiles, grid, 8);
    size_t count = width > 0 && height > 0 ? static_cast<size_t>(width) * height : 0;
    uint8_t* wc = t_walker.Get(kWalkerSize);
    uint8_t* gc = t_grid.Get(kGridSize);
    uint8_t* tc = t_tiles.Get(count * kTileSize + 1);
    uint8_t* vc = t_visible.Get(count * 8 + 8);
    if (!wc || !gc || !tc || !vc || !tiles) return g_tileWalk(walker, a, b);
    memcpy(wc, w, kWalkerSize);
    memcpy(gc, grid, kGridSize);
    {
        bool locked = g_swTileLock;
        if (locked) AcquireSRWLockShared(&g_tileWalkLock);
        memcpy(tc, tiles, count * kTileSize);
        if (locked) ReleaseSRWLockShared(&g_tileWalkLock);
    }
    memcpy(gc, &tc, 8);
    memcpy(wc, &gc, 8);
    memcpy(wc + 8, &vc, 8);
    g_tileWalk(wc, a, b);
}

// --- render-state managers (track network batches) --------------------------------------------
// cRenderStateManager (0x28 bytes): +0 / +8 vector of chunk lists (0xf8 bytes each: +0 / +8 /
// +0x10 vector of chunk pointers, +0xf0 count), +0x18 / +0x1c counters. RsmClear clears it,
// RsmAdd (manager, list index, chunk) appends, RsmDraw draws every list. On the shadow
// thread all three run on a per-thread clone: same lists, own chunk vectors.
uintptr_t kRsmClear = 0, kRsmAdd = 0, kRsmDraw = 0;
constexpr size_t kRsmSize = 0x28, kListSize = 0xf8, kListCount = 0xf0;
using RsmFn = void (*)(void* manager);
using RsmAddFn = void (*)(void* manager, int index, void* chunk);
RsmFn g_rsmClear = nullptr, g_rsmDraw = nullptr;
RsmAddFn g_rsmAdd = nullptr;

struct RsmClone {
    const void* global = nullptr;
    uint8_t header[kRsmSize] = {};
    Buffer lists;
    size_t count = 0;
    Buffer chunks[256];  // per-list chunk vectors (list count is fixed at load; capped here)
};
thread_local RsmClone t_rsm[2];

RsmClone* FindClone(const void* manager, bool create) {
    for (RsmClone& c : t_rsm)
        if (c.global == manager) return &c;
    if (!create) return nullptr;
    for (RsmClone& c : t_rsm)
        if (!c.global) return c.global = manager, &c;
    return nullptr;
}

void SetVector(uint8_t* list, uint8_t* begin, size_t used, size_t capacity) {
    uint8_t* end = begin + used * 8;
    uint8_t* cap = begin + capacity * 8;
    memcpy(list, &begin, 8);
    memcpy(list + 8, &end, 8);
    memcpy(list + 0x10, &cap, 8);
}

// Rebuilds the clone from the engine's manager with empty chunk vectors.
bool Rebuild(RsmClone& c, const uint8_t* g) {
    uint8_t *begin, *end;
    memcpy(&begin, g, 8);
    memcpy(&end, g + 8, 8);
    size_t n = static_cast<size_t>(end - begin) / kListSize;
    if (n > std::size(c.chunks)) return false;
    uint8_t* lists = c.lists.Get(n * kListSize + 1);
    if (!lists) return false;
    memcpy(lists, begin, n * kListSize);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* store = c.chunks[i].Get(256 * 8);
        if (!store) return false;
        SetVector(lists + i * kListSize, store, 0, c.chunks[i].size / 8);
        memset(lists + i * kListSize + kListCount, 0, 4);
    }
    memcpy(c.header, g, kRsmSize);
    uint8_t* lend = lists + n * kListSize;
    memcpy(c.header, &lists, 8);
    memcpy(c.header + 8, &lend, 8);
    memcpy(c.header + 0x10, &lend, 8);
    memset(c.header + 0x18, 0, 0x10);
    c.count = n;
    return true;
}

void RsmClear(void* manager) {
    if (!overlapstate::ShadowRole()) {
        UseScope use(kUseRsm);
        return g_rsmClear(manager);
    }
    RsmClone* c = FindClone(manager, true);
    if (!c || !Rebuild(*c, static_cast<uint8_t*>(manager))) return g_rsmClear(manager);
}

void RsmAdd(void* manager, int index, void* chunk) {
    if (!overlapstate::ShadowRole() || index == -1) {
        UseScope use(kUseRsm);
        return g_rsmAdd(manager, index, chunk);
    }
    RsmClone* c = FindClone(manager, false);
    if (!c) {  // first use this pass without a clear: start from an empty clone
        c = FindClone(manager, true);
        if (!c || !Rebuild(*c, static_cast<uint8_t*>(manager))) return g_rsmAdd(manager, index, chunk);
    }
    if (index == 0) return;
    uint32_t state;
    memcpy(&state, static_cast<uint8_t*>(chunk) + 0xc0, 4);
    if (state <= 2 || static_cast<size_t>(index - 1) >= c->count) return;
    size_t i = static_cast<size_t>(index - 1);
    uint8_t* list = c->lists.data + i * kListSize;
    uint8_t *begin, *end;
    memcpy(&begin, list, 8);
    memcpy(&end, list + 8, 8);
    size_t used = static_cast<size_t>(end - begin) / 8;
    if ((used + 1) * 8 > c->chunks[i].size) {  // grow our own storage, keep the entries
        Buffer& b = c->chunks[i];
        auto* p = static_cast<uint8_t*>(_aligned_malloc(b.size * 2, 16));
        if (!p) return;
        memcpy(p, b.data, used * 8);
        _aligned_free(b.data);
        b.data = p;
        b.size *= 2;
        begin = p;
    }
    memcpy(begin + used * 8, &chunk, 8);
    SetVector(list, begin, used + 1, c->chunks[i].size / 8);
    int32_t n;
    memcpy(&n, list + kListCount, 4);
    ++n;
    memcpy(list + kListCount, &n, 4);
    memcpy(&n, c->header + 0x1c, 4);
    ++n;
    memcpy(c->header + 0x1c, &n, 4);
}

void RsmDraw(void* manager) {
    RsmClone* c = overlapstate::ShadowRole() ? FindClone(manager, false) : nullptr;
    g_rsmDraw(c ? static_cast<void*>(c->header) : manager);
}

// --- network renderer: one thread at a time ----------------------------------------------------
// NetworkRender (cNetworkRenderer vf10) writes per-pass draw state into shared track chunks
// (NetChunk), so the shadow and main network renders must not interleave: both take one lock. The game thread never waits for the worker while it holds
// it (see InstBuild).
uintptr_t kNetworkRender = 0;
using NetworkRenderFn = void (*)(void* self, uint32_t pass);
NetworkRenderFn g_networkRender = nullptr;
CRITICAL_SECTION g_networkLock;
thread_local int t_sharedLocks = 0;  // shared render locks this thread holds
volatile long g_lockBypasses = 0;  // worker went ahead unlocked (would have deadlocked)
struct SharedRenderLock {
    CRITICAL_SECTION* cs;
    bool held = true;
    explicit SharedRenderLock(CRITICAL_SECTION* c, int id = kLockShared) : cs(c) {
        if (overlapstate::ShadowRole()) {
            if (TryEnterCriticalSection(cs)) {
                ++t_sharedLocks;
                return;
            }
            LockWaitTimer timer(id);
            // The worker never waits for a lock the game thread holds while the game thread
            // waits for the worker: it goes ahead without it (logged) instead of deadlocking.
            while (!TryEnterCriticalSection(cs)) {
                // (Only then: another shadow thread holding it is drawing, and must be waited for.)
                if (tscfix::overlap::GameThreadJoining() && tscfix::overlap::GameThreadHolds(cs)) {
                    held = false;
                    if (InterlockedIncrement(&g_lockBypasses) <= 5)
                        Log("research overlap: shared render lock bypassed (game thread joining)");
                    return;
                }
                YieldProcessor();
            }
        } else if (tscfix::overlap::Overlapping()) {
            if (!TryEnterCriticalSection(cs)) {
                LockWaitTimer timer(id);
                // The scenery thread draws the main view too, so it comes here. Same rule as for
                // the shadow threads, with the test the first-use and service locks use: when the
                // game thread holds the lock while it waits for this thread, this thread goes
                // ahead without it. (For the game thread the test is never true: it waits.)
                while (!TryEnterCriticalSection(cs)) {
                    if (tscfix::overlap::GameThreadHoldsWhileWaitingForMe(cs)) {
                        held = false;
                        if (InterlockedIncrement(&g_lockBypasses) <= 5)
                            Log("research overlap: shared render lock bypassed (game thread waits for this thread)");
                        return;
                    }
                    SwitchToThread();
                }
            }
        } else {
            held = false;  // no worker: nothing to guard against
            return;
        }
        ++t_sharedLocks;
    }
    ~SharedRenderLock() {
        if (!held) return;
        --t_sharedLocks;
        LeaveCriticalSection(cs);
    }
};

void NetworkRender(void* self, uint32_t pass) {
    SharedRenderLock lock(&g_networkLock, kLockNetwork);
    g_networkRender(self, pass);
}

// Terrain: cTerrainManager vf39 (TerrainDraw, draw) and vf40 (TerrainPatches) render the terrain for the
// current pass; the shadow and main terrain renders do not interleave either.
uintptr_t kTerrainDraw = 0, kTerrainPatches = 0;
using TerrainDrawFn = void (*)(void* self, int a, uint32_t b);
using TerrainPatchesFn = void (*)(void* self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint32_t e);
TerrainDrawFn g_terrainDraw = nullptr;
TerrainPatchesFn g_terrainPatches = nullptr;
CRITICAL_SECTION g_terrainLock;
void TerrainDraw(void* self, int a, uint32_t b) {
    SharedRenderLock lock(&g_terrainLock, kLockTerrain);
    g_terrainDraw(self, a, b);
}
void TerrainPatches(void* self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint32_t e) {
    SharedRenderLock lock(&g_terrainLock, kLockTerrain);
    g_terrainPatches(self, a, b, c, d, e);
}

// --- instanced geometry ------------------------------------------------------------------------
// cInstancedGeometryObject (0x418 bytes) is written per draw (world matrix, and the draw block
// at +0x2d0, 0x160 bytes). On the shadow thread InstEntry gets a per-call clone of the object and
// its draw block, InstAdd queues the clone in a thread-local map (same key as the engine's,
// **(obj+8)+0xa0), and InstFlush draws the queued clones per key and LOD slot (+0xd0 + 0x40 *
// slot) with InstDraw, then resets the binder (BinderReset). The lazy build InstBuild (when +8
// and +0x414 are set) is serialized between threads.
uintptr_t kInstEntry = 0, kInstAdd = 0, kInstFlush = 0,
                    kInstDraw = 0, kInstBuild = 0, kBinderReset = 0;
uintptr_t kBinderGlobal = 0;
constexpr size_t kInstSize = 0x418, kDrawBlockSize = 0x160, kDrawBlock = 0x2d0;
using InstEntryFn = void (*)(void* obj, const void* world, uint8_t deferred, uint8_t tileRelative);
using InstAddFn = void (*)(void* obj);
using InstFlushFn = void (*)();
using InstDrawFn = void (*)(void* obj, uint8_t a, uint8_t b, int slot);
using InstBuildFn = void (*)(void* obj);
using BinderResetFn = void (*)(void* binder, void* a, uint8_t b);
InstEntryFn g_instEntry = nullptr;
InstAddFn g_instAdd = nullptr;
InstFlushFn g_instFlush = nullptr;
InstDrawFn g_instDraw = nullptr;
InstBuildFn g_instBuild = nullptr;
volatile bool g_splitShadow = false;  // the shadow maps are drawn by two threads (SetSplit)
BinderResetFn g_binderReset = nullptr;
SRWLOCK g_buildLock = SRWLOCK_INIT;

// Bump allocator for the clones; emptied when nothing queued refers to it.
struct Arena {
    uint8_t* data = nullptr;
    size_t size = 0, used = 0;
    std::vector<uint8_t*> retired;  // older blocks still referenced until the next reset
    uint8_t* Alloc(size_t n) {
        n = (n + 15) & ~size_t{15};
        if (used + n > size) {
            if (data) retired.push_back(data);
            size_t s = size ? size * 2 : 1 << 20;
            while (s < n) s *= 2;
            data = static_cast<uint8_t*>(_aligned_malloc(s, 16));
            if (!data) return size = used = 0, nullptr;
            size = s;
            used = 0;
        }
        uint8_t* p = data + used;
        used += n;
        return p;
    }
    void Reset() {
        for (uint8_t* p : retired) _aligned_free(p);
        retired.clear();
        used = 0;
    }
};
thread_local Arena t_arena;
thread_local std::map<uint64_t, std::vector<uint8_t*>>* t_queue = nullptr;

std::map<uint64_t, std::vector<uint8_t*>>& Queue() {
    if (!t_queue) t_queue = new std::map<uint64_t, std::vector<uint8_t*>>();
    return *t_queue;
}

// The shadow pass's ring workers add to the shadow thread's map (t_queue points at it): every
// shadow-role access to a map goes through this lock.
SRWLOCK g_shadowInstLock = SRWLOCK_INIT;

// (switch instlock) the shadow clone of an instanced object is taken while no main-view entry
// writes it.
volatile bool g_swInstLock = false;
SRWLOCK g_instEntryLock = SRWLOCK_INIT;
void InstEntry(void* obj, const void* world, uint8_t deferred, uint8_t tileRelative) {
    if (!overlapstate::ShadowRole()) {
        bool locked = g_swInstLock;
        if (locked) AcquireSRWLockExclusive(&g_instEntryLock);
        g_instEntry(obj, world, deferred, tileRelative);
        if (locked) ReleaseSRWLockExclusive(&g_instEntryLock);
        return;
    }
    if (g_tileDraw) InterlockedIncrement(&g_shadowInstEntries);
    AcquireSRWLockShared(&g_shadowInstLock);
    bool empty = Queue().empty();
    ReleaseSRWLockShared(&g_shadowInstLock);
    if (empty) t_arena.Reset();
    auto* o = static_cast<uint8_t*>(obj);
    uint8_t* clone = t_arena.Alloc(kInstSize);
    uint8_t* block;
    memcpy(&block, o + kDrawBlock, 8);
    uint8_t* blockClone = block ? t_arena.Alloc(kDrawBlockSize) : nullptr;
    if (!clone || (block && !blockClone)) return g_instEntry(obj, world, deferred, tileRelative);
    bool instLocked = g_swInstLock;
    if (instLocked) AcquireSRWLockShared(&g_instEntryLock);
    memcpy(clone, o, kInstSize);
    if (g_tileDraw) {
        if (clone[0x414]) InterlockedIncrement(&g_cloneDirty);
        bool any = false;
        for (int slot = 0; slot < 8; ++slot) {
            uint64_t mesh;
            memcpy(&mesh, clone + 0xd0 + 0x40 * slot, 8);
            any |= mesh != 0;
        }
        if (!any) InterlockedIncrement(&g_cloneNoSlots);
    }
    if (block) {
        memcpy(blockClone, block, kDrawBlockSize);
        memcpy(clone + kDrawBlock, &blockClone, 8);
    }
    if (instLocked) ReleaseSRWLockShared(&g_instEntryLock);
    g_instEntry(clone, world, deferred, tileRelative);
}

// The engine's own instanced-geometry queue (one map for the main view) is filled by the track,
// train and scenery tasks and drawn by the flush at the end of the scenery walk. With scenery on
// its own thread, fills and the flush are serialized between the main-view threads.
SRWLOCK g_mainInstLock = SRWLOCK_INIT;
struct MainInstLock {
    bool held = tscfix::overlap::Overlapping();
    MainInstLock() {
        if (held) AcquireTimed(&g_mainInstLock, kLockMainInst);
    }
    ~MainInstLock() {
        if (held) ReleaseSRWLockExclusive(&g_mainInstLock);
    }
};
// The scenery thread (forked module) keeps its instanced objects in a queue of its own and draws
// them in its own flush, into its own sorted queue. With the engine's one queue the game thread
// had to wait for the scenery thread before each of its flushes. One of those is inside the track
// draw, where it holds the network lock: the game thread waited 5 ms there every frame and both
// shadow threads waited behind it for the lock (runs diag-four, locks-four). The objects are the
// engine's own, not copies: the track and the scenery do not queue the same object.
// ([experiments] fork_own_instances=0: the engine's queue for both, as before.)
volatile bool g_forkOwnInstances = true;
inline bool OwnInstQueueThread() { return g_forkOwnInstances && tscfix::overlap::OnForkWorker(); }
void InstAdd(void* obj) {
    if (!overlapstate::ShadowRole() && OwnInstQueueThread()) {
        auto* o = static_cast<uint8_t*>(obj);
        uint8_t* geometry;
        memcpy(&geometry, o + 8, 8);
        uint64_t* keyp = nullptr;
        if (geometry) memcpy(&keyp, geometry + 0xa0, 8);
        if (keyp) {
            Queue()[*keyp].push_back(o);  // (this thread's queue: no lock)
            return;
        }
    }
    if (!overlapstate::ShadowRole()) {
        MainInstLock lock;
        UseScope use(kUseInstancing);
        return g_instAdd(obj);
    }
    auto* o = static_cast<uint8_t*>(obj);
    uint8_t* geometry;
    memcpy(&geometry, o + 8, 8);
    if (!geometry) return g_instAdd(obj);  // the engine's assertion path
    uint64_t* keyp;
    memcpy(&keyp, geometry + 0xa0, 8);
    if (g_tileDraw) InterlockedIncrement(&g_cloneQueued);
    AcquireTimed(&g_shadowInstLock, kLockShadowInst);
    Queue()[*keyp].push_back(o);
    ReleaseSRWLockExclusive(&g_shadowInstLock);
}

void InstFlush() {
    if (!overlapstate::ShadowRole() && !OwnInstQueueThread()) {
        // The flush draws, and a draw can build geometry, which on the game thread ends the
        // overlap (waits for the workers). The forked module's worker may be waiting for this
        // lock to add an object: the game thread waits for that worker first, then locks
        // (t10 hung with the two waiting for each other).
        // (Not needed when the scenery thread has its own queue: it never takes this lock.)
        if (!g_forkOwnInstances) tscfix::overlap::WaitForFork();
        MainInstLock lock;
        UseScope use(kUseInstancing);
        return g_instFlush();
    }
    // (the shadow pass's ring workers are done: their adds happen inside the ring pass)
    auto& q = Queue();
    for (auto& [key, objs] : q)
        for (int slot = 0; slot < 8; ++slot)
            for (uint8_t* o : objs) {
                uint64_t mesh;
                memcpy(&mesh, o + 0xd0 + 0x40 * slot, 8);
                if (mesh) g_instDraw(o, 1, o[0x416] == 0, slot);
                if (mesh && g_tileDraw) InterlockedIncrement(&g_cloneDrawn);
            }
    q.clear();
    if (overlapstate::ShadowRole()) t_arena.Reset();  // (the shadow role queues copies; the scenery thread the objects)
    g_binderReset(reinterpret_cast<void*>(g_base + kBinderGlobal), nullptr, 0);
}

// Technique selection (SelectTechnique, called by lazy geometry builds) asks D3DX to validate a
// technique, which applies effect state through the device right there. The game thread ends the
// overlap before a build and the shadow thread holds the queue (its call waits for the replay
// thread to be idle), but a forked module's worker can do neither: its call ran beside the
// replay thread inside D3DX, which is not thread safe (t4 / t6-three-trim crashed there after
// 3.5 and 5 minutes of driving). For that thread the replay thread parks at a command boundary
// for the call; builds are rare.
uintptr_t kSelectTechnique = 0;
using SelectTechniqueFn = void* (*)(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
SelectTechniqueFn g_selectTechnique = nullptr;
volatile LONG g_techniquePauses = 0;  // (declared above for the periodic report)
void* SelectTechnique(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    // (The second shadow thread records a forked segment too: split3 crashed the replay thread in
    // D3DX on the first frames, where lazy builds are frequent.)
    if (!tscfix::overlap::OnForkWorker() && !tscfix::overlap::OnSplitWorker()) return g_selectTechnique(a, b, c, d);
    InterlockedIncrement(&g_techniquePauses);
    rt::PauseRenderThread();
    void* r = g_selectTechnique(a, b, c, d);
    rt::ResumeRenderThread();
    return r;
}
void InstBuild(void* obj) {
    auto* o = static_cast<uint8_t*>(obj);
    uint64_t geometry;
    memcpy(&geometry, o + 8, 8);
    if (!geometry || !o[0x414]) return;  // nothing to build (the engine returns here too)
    // A build creates and fills buffers, which can make the game thread wait for the worker;
    // it must not hold the lock then, so the game thread ends the overlap first.
    // (Not while it holds the network lock: the worker may be waiting for that one.)
    if (!overlapstate::ShadowRole() && !t_sharedLocks)
        tscfix::overlap::JoinIfOverlapping("lazy geometry build");
    AcquireTimed(&g_buildLock, kLockBuild);
    // (two shadow threads: neither can end the overlap for the build's device calls; see ParkReplay)
    bool park = g_splitShadow && overlapstate::ShadowRole();
    if (park) rt::PauseRenderThread();
    g_instBuild(obj);
    if (park) rt::ResumeRenderThread();
    ReleaseSRWLockExclusive(&g_buildLock);
}

// The scenery tile draw refills an instanced object's list of instance transforms (+0x20) when the
// world origin has moved away from the one the list was made for (+0xa8, written first). Whichever
// thread draws the tile first after the move does it, on the shared object: two shadow threads
// refilled one list together while loading (run split-8min: read fault growing the list). So it is
// serialized like the build that follows it (the refill sets the build flag +0x414): the game
// thread ends the overlap first; a shadow thread that waited for another's refill of the same
// object finds the origin updated and leaves the list alone.
constexpr sites::Fingerprint kInstRefill = {0xa4012b4828c333dcull, 24};
constexpr size_t kInstOrigin = 0xa8;
using InstRefillFn = void (*)(void** group);
InstRefillFn g_instRefill = nullptr;
void InstRefill(void** group) {
    auto* o = static_cast<uint8_t*>(group[0]);
    uint64_t before;
    memcpy(&before, o + kInstOrigin, 8);
    if (!overlapstate::ShadowRole() && !t_sharedLocks)
        tscfix::overlap::JoinIfOverlapping("instance list refill");
    AcquireTimed(&g_buildLock, kLockBuild);
    uint64_t now;
    memcpy(&now, o + kInstOrigin, 8);
    if (now == before) g_instRefill(group);
    ReleaseSRWLockExclusive(&g_buildLock);
}

// --- materials -----------------------------------------------------------------------------------
uintptr_t kMaterialDtor = 0, kMaterialDraw = 0;

// --- depth-only material modifier: one per binder, true two-sided flag per material -------------
// The shadow pass binds materials through a "depth-only" modifier: Apply saves the material's
// two-sided flag (+0x160) in the modifier's single slot (+0x1d) and forces it, Restore writes the
// slot back; the binder keeps the modifier's current material in the modifier itself (+0x10) and
// restores it before the next bind. The engine has ONE such modifier and assumes one thread. With
// the shadow pass on several threads (cascade thread, ring and network workers), each with its
// own binder copy:
//   - thread A's bind restored the material thread B had bound (shared +0x10), and
//   - a restore wrote another thread's saved flag into a material (shared +0x1d): the flag then
//     stayed wrong for good, and the main view culled (or stopped culling) that material's faces
//     from then on (cab instrument overlay gone: cabF/cabH, review 2026-11-10 F3).
// Fix by construction ([experiments] modifier_clones, default 1):
//   - every binder copy works with its own clone of the modifier (current material and slot
//     private); the engine's object stays the global binder's;
//   - a material's real flag is kept in a table while any thread has it forced: taken when the
//     first thread applies, written back when the last one restores. The modifier's slot is no
//     longer what a restore writes.
// Diagnostic: [research] skip_depth_modifier=1 makes the modifier a no-op on the shadow thread.
uintptr_t kDepthModifier = 0;
using ModifierFn = void (*)(void* modifier, void* material);
ModifierFn g_depthModifier = nullptr;
bool g_skipDepthModifier = false;
bool g_modifierClones = true;
constexpr size_t kBinderModifier = 0x19e8, kModCurrent = 0x10, kModKeepCull = 0x1c, kMatTwoSided = 0x160;
struct TwoSided { uint8_t value; int count; };
struct TwoSidedStripe {
    SRWLOCK lock = SRWLOCK_INIT;
    std::map<void*, TwoSided> map;
};
TwoSidedStripe g_twoSided[64];
volatile long g_modApplies = 0, g_modShared = 0, g_modUnmatched = 0, g_modCloneGets = 0, g_modClosed = 0;
TwoSidedStripe& StripeOf(void* material) { return g_twoSided[(reinterpret_cast<uintptr_t>(material) >> 6) & 63]; }
bool ModifierApplies(void* modifier, void* material) {
    using Fn = uint8_t (*)(void*, void*);
    return reinterpret_cast<Fn>((*static_cast<void***>(modifier))[3])(modifier, material) != 0;
}
void DepthModifier(void* modifier, void* material) {
    if (g_skipDepthModifier && overlapstate::ShadowRole()) return;
    if (g_modifierClones && material && ModifierApplies(modifier, material)) {
        TwoSidedStripe& st = StripeOf(material);
        AcquireSRWLockExclusive(&st.lock);
        TwoSided& t = st.map[material];
        if (t.count++ == 0) t.value = static_cast<uint8_t*>(material)[kMatTwoSided];
        else InterlockedIncrement(&g_modShared);
        ReleaseSRWLockExclusive(&st.lock);
        InterlockedIncrement(&g_modApplies);
    }
    g_depthModifier(modifier, material);
}
uintptr_t kDepthModifierRestore = 0;
ModifierFn g_depthModifierRestore = nullptr;
void DepthModifierRestore(void* modifier, void* material) {
    if (g_skipDepthModifier && overlapstate::ShadowRole()) return;
    if (g_modifierClones && material && ModifierApplies(modifier, material)) {
        TwoSidedStripe& st = StripeOf(material);
        bool done = false;
        AcquireSRWLockExclusive(&st.lock);
        auto it = st.map.find(material);
        if (it != st.map.end() && it->second.count > 0) {
            if (--it->second.count == 0) {
                static_cast<uint8_t*>(material)[kMatTwoSided] = it->second.value;
                st.map.erase(it);
            }
            done = true;
        }
        ReleaseSRWLockExclusive(&st.lock);
        if (done) return;
        InterlockedIncrement(&g_modUnmatched);
    }
    g_depthModifierRestore(modifier, material);
}
// One clone per binder copy (a copy is used by one thread at a time).
struct ModClone {
    uint8_t* binder;
    uint8_t* source;
    uint8_t sourceKeepCull;
    alignas(16) uint8_t obj[0x20];
};
ModClone g_modClones[48];
volatile long g_modCloneCount = 0;
SRWLOCK g_modCloneLock = SRWLOCK_INIT;
ModClone* FindModClone(uint8_t* b) {
    long n = g_modCloneCount;
    for (long i = 0; i < n; ++i)
        if (g_modClones[i].binder == b) return &g_modClones[i];
    return nullptr;
}
// The clone's current material gets its restore (as the binder would before the next bind).
void CloseModClone(ModClone& c) {
    void* cur;
    memcpy(&cur, c.obj + kModCurrent, 8);
    if (!cur) return;
    reinterpret_cast<ModifierFn>((*reinterpret_cast<void***>(c.obj))[1])(c.obj, cur);
    memset(c.obj + kModCurrent, 0, 8);
    InterlockedIncrement(&g_modClosed);
}
void* ModCloneFor(uint8_t* b, uint8_t* m) {
    ModClone* c = FindModClone(b);
    if (!c) {
        AcquireSRWLockExclusive(&g_modCloneLock);
        long n = g_modCloneCount;
        if (n < static_cast<long>(std::size(g_modClones))) {
            c = &g_modClones[n];
            c->binder = b, c->source = nullptr;
            InterlockedExchange(&g_modCloneCount, n + 1);
        }
        ReleaseSRWLockExclusive(&g_modCloneLock);
        if (!c) return m;  // table full: the engine's object, as before
    }
    if (c->source != m) {
        if (c->source) CloseModClone(*c);
        memcpy(c->obj, m, sizeof c->obj);
        memset(c->obj + kModCurrent, 0, 8);
        c->source = m, c->sourceKeepCull = m[kModKeepCull];
    } else if (m[kModKeepCull] != c->sourceKeepCull) {
        c->obj[kModKeepCull] = c->sourceKeepCull = m[kModKeepCull];  // changed on the engine's object
    }
    InterlockedIncrement(&g_modCloneGets);
    return c->obj;
}
bool DepthOnlyModifier(void* m) {
    return kDepthModifier && (*static_cast<void***>(m))[0] == reinterpret_cast<void*>(g_base + kDepthModifier);
}
// Binder methods: get / clear / set the binder's modifier.
uintptr_t kModifierGet = 0, kModifierClear = 0, kModifierSet = 0;
using ModifierGetFn = void* (*)(uint8_t* binder);
using ModifierClearFn = void (*)(uint8_t* binder);
using ModifierSetFn = void (*)(uint8_t* binder, void* modifier);
ModifierGetFn g_modifierGet = nullptr;
ModifierClearFn g_modifierClear = nullptr;
ModifierSetFn g_modifierSet = nullptr;
void* ModifierGet(uint8_t* b) {
    void* m;
    memcpy(&m, b + kBinderModifier, 8);
    if (!m || !g_modifierClones || b == binder::Global() || !DepthOnlyModifier(m)) return m;
    return ModCloneFor(b, static_cast<uint8_t*>(m));
}
void ModifierClear(uint8_t* b) {
    if (g_modifierClones && b != binder::Global()) {
        void* m;
        memcpy(&m, b + kBinderModifier, 8);
        ModClone* c = FindModClone(b);
        if (c && c->source == m && m) {
            CloseModClone(*c);
            memset(b + kBinderModifier, 0, 8);
            return;
        }
    }
    g_modifierClear(b);
}
void ModifierSet(uint8_t* b, void* modifier) {
    if (g_modifierClones && b != binder::Global())
        if (ModClone* c = FindModClone(b); c && c->source && c->source != modifier) CloseModClone(*c), c->source = nullptr;
    g_modifierSet(b, modifier);
}
// A thread is done drawing with binder copy b (job / pass end): nothing stays forced.
void CloseModifierOf(uint8_t* b) {
    if (!g_modifierClones || !b || b == binder::Global()) return;
    if (ModClone* c = FindModClone(b)) CloseModClone(*c);
}
void LogModifierCounts() {
    Log("research overlap: depth modifier: %ld applies (%ld on a material another thread had forced), %ld restores without an "
        "apply, %ld clone gets, %ld closed at job end, %ld clones",
        InterlockedExchange(&g_modApplies, 0), InterlockedExchange(&g_modShared, 0), InterlockedExchange(&g_modUnmatched, 0),
        InterlockedExchange(&g_modCloneGets, 0), InterlockedExchange(&g_modClosed, 0), g_modCloneCount);
}

// --- engine services shared by both threads ----------------------------------------------------
// Rare calls into engine services that mutate shared containers (option lookups append to a
// record cache; hooked: OptionsGet, ProfileGet, RecordGet). Serialized between all threads with
// one recursive lock.
uintptr_t kOptionsGet = 0, kProfileGet = 0, kRecordGet = 0;
CRITICAL_SECTION g_serviceLock;
// The game thread can hold this lock (around the light walk) at the moment it has to wait for a
// worker to finish (a buffer lock inside the walk needs the queue): a worker that then needs
// the lock would wait for the game thread, which waits for it (t11 hung this way). While the game
// thread is blocked until this very worker is done it does nothing under the lock, so the worker
// goes on under a second lock that only such workers take. Counted (LogSharedUse). Only when the
// game thread is the holder: when the other worker holds the lock, this one waits for it (ab1-fork
// crashed in a service call made beside the shadow thread's).
CRITICAL_SECTION g_serviceWaitedLock;
volatile LONG g_serviceBypasses = 0;
struct ServiceLock {
    int mode = 0;  // 0 none, 1 the service lock, 2 the game thread waits for this worker
    ServiceLock() {
        if (!tscfix::overlap::Overlapping()) return;
        if (TryEnterCriticalSection(&g_serviceLock)) { mode = 1; return; }
        LockWaitTimer timer(kLockService);
        for (;;) {
            if (TryEnterCriticalSection(&g_serviceLock)) { mode = 1; return; }
            if (tscfix::overlap::GameThreadHoldsWhileWaitingForMe(&g_serviceLock)) {
                EnterCriticalSection(&g_serviceWaitedLock);
                InterlockedIncrement(&g_serviceBypasses);
                mode = 2;
                return;
            }
            SwitchToThread();
        }
    }
    ~ServiceLock() {
        if (mode == 1) LeaveCriticalSection(&g_serviceLock);
        else if (mode == 2) LeaveCriticalSection(&g_serviceWaitedLock);
    }
};
using OptionsGetFn = uint32_t (*)(void* self, int id);
using PtrGetFn = void* (*)(void* self, void* out);
OptionsGetFn g_optionsGet = nullptr;
PtrGetFn g_profileGet = nullptr, g_recordGet = nullptr;
uint32_t OptionsGet(void* self, int id) { ServiceLock l; return g_optionsGet(self, id); }
void* ProfileGet(void* self, void* out) { ServiceLock l; return g_profileGet(self, out); }
void* RecordGet(void* self, void* out) { ServiceLock l; return g_recordGet(self, out); }
// The record set's observer vector (+0x30) also changes through RecordAdd and RecordRemove;
// unlocked, those corrupted it beside RecordGet (soakPipe10: vector growth threw, the engine
// terminated).
uintptr_t kRecordAdd = 0, kRecordRemove = 0;
PtrGetFn g_recordAdd = nullptr, g_recordRemove = nullptr;
void* RecordAdd(void* self, void* p) { ServiceLock l; return g_recordAdd(self, p); }
void* RecordRemove(void* self, void* p) { ServiceLock l; return g_recordRemove(self, p); }

// Light manager: lights are created / destroyed (and the list cleared) by the objects that own
// them, which may happen while either thread draws: cLightManager LightCreate, LightDestroy and
// LightClear are serialized under the service lock.
uintptr_t kLightCreate = 0, kLightDestroy = 0, kLightClear = 0;
using LightCreateFn = void* (*)(void* self, uint64_t a, float b, float c, float d, void* e, char f, void* g);
using LightDestroyFn = uint64_t (*)(void* self, void* light);
using LightClearFn = void (*)(void* self);
// (diagnosis, [experiments] hold_flash_ms=N) A point light that is removed within 50 ms of its
// creation was on for about one frame: too short for a screenshot. Its removal then waits N ms,
// so the frame with the light stays on the screen that long.
extern int g_holdFlashMs;
struct BornLight { void* light; ULONGLONG tick; float range; };
BornLight g_bornLights[64];
volatile long g_bornNext = 0;
void NoteBorn(void* light, float range) {
    if (g_holdFlashMs <= 0 || !light) return;
    g_bornLights[InterlockedIncrement(&g_bornNext) & 63] = {light, GetTickCount64(), range};
}
void HoldIfShortLived(void* light) {
    if (g_holdFlashMs <= 0) return;
    for (auto& b : g_bornLights)
        if (b.light == light) {
            ULONGLONG age = GetTickCount64() - b.tick;
            b.light = nullptr;
            if (age < 50) {
                static long held = 0;
                if (++held <= 8) Log("light: a point light with range %.2f lived %llu ms; its frame is held for %d ms", b.range, age, g_holdFlashMs);
                Sleep(g_holdFlashMs);
            }
            return;
        }
}
LightCreateFn g_lightCreate = nullptr;
LightDestroyFn g_lightDestroy = nullptr;
LightClearFn g_lightClear = nullptr;
void* LightCreate(void* self, uint64_t a, float b, float c, float d, void* e, char f, void* g) {
    ServiceLock l;
    void* light = g_lightCreate(self, a, b, c, d, e, f, g);
    if (light && trace::On(trace::kLight)) trace::Blob(trace::kLightCreate, reinterpret_cast<uint64_t>(light), light, 0x118);
    return light;
}
uint64_t LightDestroy(void* self, void* light) {
    HoldIfShortLived(light);
    ServiceLock l;
    TSCFIX_TRACE(trace::kLight, trace::kLightDestroy, reinterpret_cast<uint64_t>(light));
    return g_lightDestroy(self, light);
}
// Point lights are created through their own manager entry, which adds to the list the walk
// reads: serialized like LightCreate.
uintptr_t kCreatePointLight = 0;
using CreatePointLightFn = void* (*)(void* self, const void* matrix, float range, const void* colour, char b, void* e);
CreatePointLightFn g_createPointLight = nullptr;
void* CreatePointLight(void* self, const void* matrix, float range, const void* colour, char b, void* e) {
    ServiceLock l;
    void* light = g_createPointLight(self, matrix, range, colour, b, e);
    NoteBorn(light, range);
    if (light && trace::On(trace::kLight)) trace::Blob(trace::kLightCreate, reinterpret_cast<uint64_t>(light), light, 0xd0);
    return light;
}
// cLightManager LightWalk (render side) reads the light lists under the service lock too.
uintptr_t kLightWalk = 0;
using LightWalkFn = void (*)(void* self, uint64_t a, char b);
LightWalkFn g_lightWalk = nullptr;
// (diagnosis, [experiments] light_dump=N) every N light walks: every point light's record
// (manager list +0x10 / +0x18), read under the service lock: flags, far position words, colour,
// range and a hash of the cached draw block, plus whether the walk drew it (cached block changed
// or dirty bit cleared is not visible here; compare a good and a bad launch).
int g_lightDump = 0;
bool DumpLights(void* self, const char* when, long walk) {
    __try {
        auto* m = static_cast<uint8_t*>(self);
        uint8_t** begin = *reinterpret_cast<uint8_t***>(m + 0x10);
        uint8_t** end = *reinterpret_cast<uint8_t***>(m + 0x18);
        size_t n = begin && end >= begin ? static_cast<size_t>(end - begin) : 0;
        Log("light dump %s walk %ld: %zu point lights", when, walk, n);
        for (size_t i = 0; i < n && i < 400; ++i) {
            const uint8_t* l = begin[i];
            if (!l) continue;
            uint32_t flags, h = 2166136261u;
            int32_t w[5];
            float c[4], range;
            memcpy(&flags, l + 8, 4), memcpy(w, l + 0x48, 20), memcpy(c, l + 0x60, 16), memcpy(&range, l + 0x80, 4);
            for (int k = 0x90; k < 0xd0; ++k) h = (h ^ l[k]) * 16777619u;
            float f[5];
            memcpy(f, w, 20);
            Log("light dump %s: #%zu %p flags %x pos %d/%.2f %.2f %d/%.2f | raw %08x %08x %08x %08x %08x | colour %.2f %.2f %.2f %.2f range %.2f cache %08x",
                when, i, static_cast<const void*>(l), flags, w[0], f[1], f[2], w[3], f[4], w[0], w[1], w[2], w[3], w[4], c[0], c[1], c[2],
                c[3], range, h);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
int g_holdFlashMs = 0;
void LightWalk(void* self, uint64_t a, char b) {
    ServiceLock l;
    static long walks = 0;
    bool dump = g_lightDump > 0 && ++walks % g_lightDump == 0;
    if (dump) DumpLights(self, "before", walks);
    int64_t t0 = 0;
    if (trace::On(trace::kLight | trace::kState)) {
        auto* m = static_cast<uint8_t*>(self);
        uint8_t** pb = *reinterpret_cast<uint8_t***>(m + 0x10);
        uint8_t** pe = *reinterpret_cast<uint8_t***>(m + 0x18);
        uint8_t** sb = *reinterpret_cast<uint8_t***>(m + 0x28);
        uint8_t** se = *reinterpret_cast<uint8_t***>(m + 0x30);
        size_t np = pb && pe >= pb ? static_cast<size_t>(pe - pb) : 0;
        size_t ns = sb && se >= sb ? static_cast<size_t>(se - sb) : 0;
        if (trace::On(trace::kLight)) trace::Emit(trace::kLightWalkBegin, np, ns);
        static long stateWalks = 0;
        if (trace::On(trace::kState) && ++stateWalks % 100 == 0 && np && np < 2000) {
            static std::vector<uint8_t> copy;
            copy.resize(np * 0xd0);
            for (size_t k = 0; k < np; ++k) memcpy(copy.data() + k * 0xd0, pb[k], 0xd0);
            trace::Blob(trace::kStateLightPtrs, np, pb, np * 8);
            trace::Blob(trace::kStateLights, np, copy.data(), copy.size());
        }
        t0 = trace::Now();
    }
    g_lightWalk(self, a, b);
    TSCFIX_TRACE(trace::kLight, trace::kLightWalkEnd, static_cast<uint64_t>(trace::Now() - t0));
    if (dump) DumpLights(self, "after", walks);
}
void LightClear(void* self) {
    ServiceLock l;
    g_lightClear(self);
}



// --- sorted (offset) renderers -----------------------------------------------------------------
// cHcSGOffsetRenderer (0x2120 bytes, constructor QueueCtor): a sorted draw queue with its own
// memory pool. Shared instances hang off the binder (+0x28) and the graphics engine (+0x98).
// The worker gets private instances: its binder copy points at them, and the queue's entry
// points (QueueInsert, QueueBegin, QueueFlush, QueueReset) swap a shared queue for the
// worker's on the shadow thread.
uintptr_t kQueueCtor = 0, kQueueInsert = 0, kQueueBegin = 0,
                    kQueueFlush = 0, kQueueReset = 0, kEngineGet = 0,
                    kAllocator = 0;
constexpr size_t kQueueSize = 0x2120, kBinderQueue = 0x28, kEngineQueue = 0x98, kQueueModelState = 0x2118;
using QueueCtorFn = void* (*)(void* mem);
using QueueInsertFn = uint64_t (*)(void* q, void* item, void* p3);
using QueueFn = void (*)(void* q);
using QueueFlushFn = void (*)(void* q, void* a, void* b);
using EngineGetFn = void* (*)();
QueueInsertFn g_queueInsert = nullptr;
QueueFn g_queueBegin = nullptr, g_queueReset = nullptr;
QueueFlushFn g_queueFlush = nullptr;
struct QueuePair {
    void* shared;
    void* own;
};
// One table per shadow thread (slot 0: the overlap's worker; slot 1: the second shadow thread, which
// draws some of the shadow maps beside it). Written by the game thread before the shadow pass
// launches (PrepareWorker) and by that slot's thread while it runs (AdoptQueue); never by both at
// once. Read by that slot's thread.
struct QueueSet {
    QueuePair q[16];
    volatile int n = 0;
    // Queues constructed ahead on the game thread (the engine's allocator is the game thread's)
    // for the shadow thread to adopt.
    void* spare[4];
    volatile int spares = 0;
};
QueueSet g_qs[2];
thread_local int t_slot = 0;  // this thread's shadow slot (1 only on the second shadow thread)
inline QueueSet& QS() { return g_qs[t_slot]; }
struct SlotScope {  // game thread: prepare another slot's tables
    int saved;
    explicit SlotScope(int slot) : saved(t_slot) { t_slot = slot; }
    ~SlotScope() { t_slot = saved; }
};
void* MakeQueue() {
    void* allocator = *reinterpret_cast<void**>(g_base + kAllocator);
    using AllocFn = void* (*)(void* self, size_t size);
    void* mem = reinterpret_cast<AllocFn>((*static_cast<void***>(allocator))[1])(allocator, kQueueSize);
    return mem ? reinterpret_cast<QueueCtorFn>(g_base + kQueueCtor)(mem) : nullptr;
}
// Shadow thread: a queue it opens itself (begin, fill, flush inside the shadow pass) becomes its
// own from here on. The light viewport's queue is one: the shadow pass puts about 6000 elements
// per pass through it, and main-view tasks (decals) fill the same object beside it (play1: a
// flush on the shadow thread crashed while the game thread inserted).
void* AdoptQueue(void* q) {
    int n = QS().n, spare = QS().spares;
    if (n >= static_cast<int>(std::size(QS().q)) || spare <= 0) return q;  // next pass: spares are topped up
    void* own = QS().spare[spare - 1];
    QS().spares = spare - 1;
    QS().q[n] = {q, own};
    QS().n = n + 1;
    Log("research overlap: private sorted queue %p adopted by the shadow thread (shared %p)", own, q);
    return own;
}

// (telemetry) Sorted queues the shadow thread uses without a private copy: per queue, how often
// it fills, opens, resets and flushes it, and who called first. A queue it flushes itself is one
// it must own (the main view fills the same queue meanwhile: play1 froze on that).
struct SharedQueueUse {
    void* queue;
    uintptr_t firstCaller;
    int firstOp;
    volatile LONG ops[4];  // insert, begin, reset, flush
};
SharedQueueUse g_sharedQueueUse[16];
volatile LONG g_sharedQueueUseCount = 0;
thread_local int t_queueOp = -1;       // the op the current hook is (set by the hooks below)
thread_local uintptr_t t_queueCaller = 0;
void NoteSharedQueueUse(void* q) {
    if (t_queueOp < 0) return;
    LONG n = g_sharedQueueUseCount;
    for (LONG i = 0; i < n; ++i)
        if (g_sharedQueueUse[i].queue == q) {
            InterlockedIncrement(&g_sharedQueueUse[i].ops[t_queueOp]);
            return;
        }
    if (n >= 16) return;  // (shadow thread only writes: no race on the count)
    g_sharedQueueUse[n].queue = q, g_sharedQueueUse[n].firstCaller = t_queueCaller, g_sharedQueueUse[n].firstOp = t_queueOp;
    g_sharedQueueUse[n].ops[t_queueOp] = 1;
    InterlockedExchange(&g_sharedQueueUseCount, n + 1);
}
void LogSharedQueueUse() {
    static const char* const kOps[4] = {"insert", "begin", "reset", "flush"};
    for (LONG i = 0; i < g_sharedQueueUseCount; ++i) {
        SharedQueueUse& u = g_sharedQueueUse[i];
        Log("research overlap: shadow thread on shared sorted queue %p: %ld inserts, %ld begins, %ld resets, %ld flushes "
            "(last 600 passes); first was a %s from rva %llx",
            u.queue, InterlockedExchange(&u.ops[0], 0), InterlockedExchange(&u.ops[1], 0), InterlockedExchange(&u.ops[2], 0),
            InterlockedExchange(&u.ops[3], 0), kOps[u.firstOp], static_cast<unsigned long long>(u.firstCaller));
    }
}

thread_local QueuePair t_lastQueue{};  // the shadow thread's last lookup (most are for one queue)
thread_local bool t_ownHit = false;    // the last lookup was given the thread's private queue itself
void* OwnQueue(void* q) {
    t_ownHit = false;
    if (!overlapstate::ShadowRole()) return q;
    if (t_lastQueue.shared == q) return t_lastQueue.own;
    for (int i = 0; i < QS().n; ++i)
        if (QS().q[i].shared == q) return t_lastQueue = QS().q[i], QS().q[i].own;
    for (int i = 0; i < QS().n; ++i)
        if (QS().q[i].own == q) return t_ownHit = true, q;  // already its own (begin calls the hooked reset)
    NoteSharedQueueUse(q);
    return q;
}
// Queues both threads keep using (the worker has no private copy, e.g. the decal renderer's)
// are filled under one lock. Flushes draw (and may wait for the worker), so they take no lock;
// shared queues are flushed by modules that join first.
// Recursive: begin calls reset, both hooked.
// Not locked: a queue only its caller fills. That is the global binder's queue on the game thread
// (the shadow thread and a forked module each have their own in its place) and the forked
// module's own queue. Nearly every insert goes to one of these.
CRITICAL_SECTION g_queueLock;
void* volatile g_binderQueue = nullptr;  // the global binder's queue (seen in PrepareWorker)
void* g_forkQueue = nullptr;  // the forked module's own queue (see ForkPrepare)
bool CallerOnlyQueue(void* q) { return q == g_binderQueue || q == g_forkQueue; }
struct QueueLock {
    bool held;
    explicit QueueLock(bool shared) : held(shared && tscfix::overlap::Overlapping()) {
        if (held) EnterTimed(&g_queueLock, kLockQueue);
    }
    ~QueueLock() {
        if (held) LeaveCriticalSection(&g_queueLock);
    }
};
// Model draws write a flag (+0x2118) into four sorted queues: the binder's and three returned by
// the graphics engine getters QueueGet0 / QueueGet1 / QueueGet2 (fields +0xc8 / +0xd8 / +0x68).
// On the worker those getters return its private queues, so the worker's flag writes (and
// inserts) stay off the game thread's cache lines.
uintptr_t kQueueGet[3] = {};
constexpr size_t kEngineQueueField[3] = {0xc8, 0xd8, 0x68};
using QueueGetFn = void* (*)(void* engine);
QueueGetFn g_queueGet[3] = {};
void* QueueGet0(void* e) { return OwnQueue(g_queueGet[0](e)); }
void* QueueGet1(void* e) { return OwnQueue(g_queueGet[1](e)); }
void* QueueGet2(void* e) { return OwnQueue(g_queueGet[2](e)); }

struct QueueOp {  // names the hooked op for the shared-queue telemetry
    QueueOp(int op, void* ret) { t_queueOp = op, t_queueCaller = reinterpret_cast<uintptr_t>(ret) - g_base; }
    ~QueueOp() { t_queueOp = -1; }
};
uint64_t QueueInsert(void* q, void* item, void* p3) {
    if (g_tileDraw && overlapstate::ShadowRole()) InterlockedIncrement(&g_shadowInserts);
    QueueOp op(0, _ReturnAddress());
    void* own = OwnQueue(q);
    QueueLock lock(own == q && !t_ownHit && g_qs[0].n && !CallerOnlyQueue(q));
    for (int i = 0; i < 4; ++i)
        if (g_flushing[i] == own && g_flushingBy[i] != static_cast<LONG>(GetCurrentThreadId()))
            NoteClash(kUseQueueFlush, static_cast<DWORD>(g_flushingBy[i]));
    if (own == q) return g_queueInsert(own, item, p3);
    // The engine's draw code writes a queue's per-model state word (+0x2118: "draw state already
    // captured" and "one capture per model") straight into the object it knows, which is the
    // shared one. The insert reads and sets that word, so it is carried over to the private queue
    // for the call and back afterwards; without it every insert captured the draw state again
    // (q2-adopt: shadow pass 16 -> 20 ms).
    auto* shared = static_cast<uint8_t*>(q);
    auto* mine = static_cast<uint8_t*>(own);
    memcpy(mine + kQueueModelState, shared + kQueueModelState, 2);
    uint64_t r = g_queueInsert(own, item, p3);
    memcpy(shared + kQueueModelState, mine + kQueueModelState, 2);
    return r;
}
void QueueBegin(void* q) {
    QueueOp op(1, _ReturnAddress());
    void* own = OwnQueue(q);
    if (own == q && overlapstate::ShadowRole()) own = AdoptQueue(q);
    QueueLock lock(own == q && !t_ownHit && g_qs[0].n && !CallerOnlyQueue(q));
    g_queueBegin(own);
}
// A queued element keeps a snapshot of its material's texture slots 0-3 (taken at insert, element
// +0x10..+0x2f); the flush writes the snapshot into the shared material, draws, and writes the
// old values back. The shadow role's flushes did that beside the main view's own per-object texture
// swaps (sign digits, cab displays) on the same shared materials: a sign drew another sign's digit,
// or lost one. The shadow role's elements drop their snapshot (a null entry is not written); the
// depth-only shadow draw does not need those textures.
// [experiments] shadow_clear_slot_snapshots (default 1); switch shadowsnap.
volatile bool g_shadowClearSnapshots = true;
// MaterialDraw (material, pass, arg, changed) sets the material's textures only when `changed`
// (the binder had another material bound). With the passes beside each other that was not
// reliable: draws kept the previous object's textures (buildings, facades: forceTex1). Every draw
// sets them; measured free (forceTexAB2: +0.0% frame time).
// [experiments] force_material_textures (default 1); switch forcetex.
using MaterialDrawFn = void (*)(void* material, int pass, uint64_t arg, char changed);
MaterialDrawFn g_materialDraw = nullptr;
volatile bool g_swForceTex = true;
// A material's first draw resolves its effect parameters into arrays in the material and in the
// effect data its kind shares, and sets a "resolved" bit (+0x6d8 bit 0) at the end. Two threads
// drawing a new material for the first time at once both ran that setup (t5-three-trim: the
// shadow thread and the scenery thread crashed together in it, flushing the same instanced
// geometry). First draws are serialized; every later draw only tests the bit.
// The setup only runs for a material with effect data (+0x6e0); one without never gets the bit,
// and locking each of its draws made the three threads take turns (t9: frames of 260 ms when
// such materials came into view).
constexpr size_t kMaterialResolved = 0x6d8, kMaterialEffectData = 0x6e0;
CRITICAL_SECTION g_firstUseLock;
thread_local int t_firstUse = 0;  // this thread is inside a first use (holds the lock)
// The material whose setup the first-use lock's holder is inside. A worker that passes the lock
// because the blocked game thread holds it must leave exactly that material alone: the game thread
// may be halfway through its setup (the setup's effect lookups can make it wait for the workers),
// and a second setup of it would append to the half-filled arrays. The worker skips that
// material for the frame.
void* volatile g_setupInProgress = nullptr;
struct FirstUse {
    CRITICAL_SECTION* lock = nullptr;
    bool passed = false;  // went ahead beside the blocked game thread
    bool MustSkip(void* material) const { return passed && material == g_setupInProgress; }
    FirstUse() {
        if (t_firstUse++) return;  // nested: the outer one holds it
        // (Same rule as the service lock: the game thread may be inside a first use when it has to
        // wait for this worker; it does nothing under the lock meanwhile.)
        lock = &g_firstUseLock;
        if (TryEnterCriticalSection(lock)) return;
        InterlockedIncrement(&g_firstUseWaits);
        LockWaitTimer timer(kLockFirstUse);
        for (;;) {
            if (TryEnterCriticalSection(lock)) return;
            if (tscfix::overlap::GameThreadHoldsWhileWaitingForMe(&g_firstUseLock)) {
                lock = &g_serviceWaitedLock;
                EnterCriticalSection(lock);
                passed = true;
                return;
            }
            SwitchToThread();
        }
    }
    ~FirstUse() {
        --t_firstUse;
        if (lock) LeaveCriticalSection(lock);
    }
};
// A first use or a geometry build calls D3DX and the device directly (technique validation,
// buffer creation and locks). The overlap's worker ends the overlap for such a call, so the replay
// thread is idle during it. With two shadow threads neither can (each would wait for the other):
// their calls went out directly while the replay thread was running effect calls on the same
// effects, which D3DX does not allow (runs split-8min: both loading crashes came within 12 ms of
// the second shadow thread's first such calls). They park the replay thread for the duration,
// as technique selection on a forked worker already does. Taken inside the first-use / build
// lock, so one shadow thread at a time.
struct ParkReplay {
    bool on = g_splitShadow && overlapstate::ShadowRole();
    ParkReplay() {
        if (on) rt::PauseRenderThread();
    }
    ~ParkReplay() {
        if (on) rt::ResumeRenderThread();
    }
};
// Sign digits and plate text are flat quads a hair in front of the plate. At a distance both land
// on the same depth value and the winner changes with the camera's slight drift: digits break up
// from frame to frame (stock too). Their materials are the ones with texture overrides applied
// (+0x14e, set for the main view only, between ApplyOverrides and RestoreOverrides). Those draws get
// a small depth bias towards the camera; the next other material puts it back, and so does the end
// of the override draw (SignBiasEnd).
// [graphics] sign_text_depth_bias (depth steps of a 24-bit buffer, default 32; 0 = off), sign_text_slope_bias (x100, default 100).
constexpr size_t kMaterialOverridden = 0x14e;
float g_signBias = 0, g_signSlope = 0;
thread_local bool t_signBiased = false;
volatile LONG g_signBiasedDraws = 0;
// [graphics] sign_text_toggle_key: virtual key that switches the bias on and off (compare captures).
int g_signToggleKey = 0;
volatile bool g_signBiasOn = true;
void PollSignToggle() {
    static volatile LONG calls = 0;
    static bool down = false;
    if (!g_signToggleKey || (InterlockedIncrement(&calls) & 0xfff)) return;
    bool now = (GetAsyncKeyState(g_signToggleKey) & 0x8000) != 0;
    if (now && !down) {
        g_signBiasOn = !g_signBiasOn;
        Log("pipeline: sign text depth bias %s (key)", g_signBiasOn ? "on" : "off");
    }
    down = now;
}
void SignTextBias(const uint8_t* m) {
    PollSignToggle();
    bool want = g_signBiasOn && m[kMaterialOverridden] && !overlapstate::ShadowRole();
    if (want == t_signBiased) return;
    IDirect3DDevice9* d = tscfix::d3d::GameDevice();
    if (!d) return;
    float bias = want ? g_signBias : 0.0f, slope = want ? g_signSlope : 0.0f;
    d->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<DWORD*>(&bias));
    d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<DWORD*>(&slope));
    t_signBiased = want;
    if (want && (InterlockedIncrement(&g_signBiasedDraws) & 0xffff) == 1)
        Log("pipeline: sign text depth bias: %ld material changes biased", g_signBiasedDraws);
}
// The override flag is set only inside the engine's override draw, so the bias is switched off
// when that draw ends, on the thread that switched it on. Waiting for the next material draw on
// this thread let the bias run on into what came next in the command stream: another thread's part
// of it, or the next shadow pass. Both states go back to 0: the values they had are not known here,
// and to read them from the device this thread would have to wait for the render thread's queue.
struct SignBiasEnd {
    ~SignBiasEnd() {
        if (!t_signBiased) return;
        t_signBiased = false;
        IDirect3DDevice9* d = tscfix::d3d::GameDevice();
        if (!d) return;
        d->SetRenderState(D3DRS_DEPTHBIAS, 0);
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    }
};
void MaterialDraw(void* material, int pass, uint64_t arg, char changed) {
    auto* m = static_cast<uint8_t*>(material);
    if (g_signBias != 0 || g_signSlope != 0) SignTextBias(m);
    uint64_t effectData;
    memcpy(&effectData, m + kMaterialEffectData, 8);
    if ((m[kMaterialResolved] & 1) || !effectData)
        return g_materialDraw(material, pass, arg, g_swForceTex ? 1 : changed);
    FirstUse first;
    if (first.MustSkip(material)) return;
    InterlockedIncrement(&g_firstUses);
    ParkReplay park;
    g_materialDraw(material, pass, arg, g_swForceTex ? 1 : changed);
}
// The setup itself (it fills the material's parameter arrays, counting as it appends, and sets the
// resolved bit last). Queueing a material reaches it too, not only drawing one: two shadow threads
// queueing the same new material ran it together (split1: both faulted in it on the first frames).
// It runs once: a thread that waited for another's setup of the same material finds it done.
constexpr sites::Fingerprint kMaterialSetup = {0x264ff6d3d6c70edeull, 48};
using MaterialSetupFn = void (*)(void* material);
MaterialSetupFn g_materialSetup = nullptr;
void MaterialSetup(void* material) {
    auto* m = static_cast<uint8_t*>(material);
    bool wasResolved = (m[kMaterialResolved] & 1) != 0;
    FirstUse first;
    if (!wasResolved && (m[kMaterialResolved] & 1)) return;
    if (first.MustSkip(material)) return;
    void* outer = g_setupInProgress;
    if (!first.passed) g_setupInProgress = material;
    {
        ParkReplay park;
        g_materialSetup(material);
    }
    if (!first.passed) g_setupInProgress = outer;
}
void ClearSlotSnapshots(void* q);
// A flush draws its items one by one. For an item that carries replacement values (item +0x10 to
// +0x28) it writes them into four slots of the geometry every instance shares, draws, and puts
// the old values back. The shadow role's items carry none (ClearSlotSnapshots). But the game
// thread and the scenery thread both flush main-view queues: when they did so at the same time,
// one drew with the other's values. A lamp mast at Amersfoort was drawn with its lit texture in
// 1% to 24% of the frames (runs flick-four, flick-fix1 to flick-fix4). The main-view flushes now
// take turns. The scenery thread does not wait for a flush the game thread holds while the game
// thread waits for the workers (it goes ahead, as the shared render locks do).
// The Z-sorted queue (transparent objects) is one object for the whole main view. An insert takes
// an element from the queue's own stack heap and links it into a list, with no lock. With the
// scenery thread, the game thread (track, trains) and the scenery thread insert at the same time.
// Run pt-hsl-virm6 (Amsterdam Centraal, 6-car VIRM): the list became a ring, the flush that walks
// it never ended, each step added a 360-byte record to a vector, and the game grew to 40 GB in
// under a minute and then stopped. Inserts now take one lock while more than one thread draws.
constexpr sites::Fingerprint kZSortInsert = {0x32f0d82f0ca5ad0eull, 24};
using ZSortInsertFn = uint64_t (*)(void* self, float depth, void* node, uint8_t a, uint8_t b, uint32_t c);
ZSortInsertFn g_zSortInsert = nullptr;
CRITICAL_SECTION g_zSortLock;
// The queue has a flag (+0x10f1) that the engine sets for the whole shadow update and clears after
// it (two engine functions). While it is set, an insert does nothing and answers "not
// queued", and the caller draws the transparent item at once, into the shadow map. With the maps
// on worker threads the game thread clears the flag while they still draw: their transparent
// items (19 per pass at East Croydon) went into the main view's queue. They were missing from the
// shadow maps, and the main view drew them later with the wrong pass state (single objects dark
// for one frame with two shadow threads). A thread in the shadow role gets the answer the engine
// gives during the shadow update. [experiments] zsort_shadow_direct=0 turns this off.
bool g_zSortShadowDirect = true;
uint64_t ZSortInsert(void* self, float depth, void* node, uint8_t a, uint8_t b, uint32_t c) {
    if (g_zSortShadowDirect && overlapstate::ShadowRole()) return 0;
    if (!tscfix::overlap::Overlapping()) return g_zSortInsert(self, depth, node, a, b, c);
    EnterTimed(&g_zSortLock, kLockZSort);
    uint64_t r = g_zSortInsert(self, depth, node, a, b, c);
    LeaveCriticalSection(&g_zSortLock);
    return r;
}

CRITICAL_SECTION g_mainFlushLock;
struct MainFlushTurn {
    bool held = false;
    MainFlushTurn() {
        if (overlapstate::ShadowRole() || !tscfix::overlap::Overlapping()) return;
        if (!tscfix::overlap::OnForkWorker()) {
            EnterTimed(&g_mainFlushLock, kLockMainFlush);
            held = true;
            return;
        }
        if (TryEnterCriticalSection(&g_mainFlushLock)) {
            held = true;
            return;
        }
        LockWaitTimer timer(kLockMainFlush);
        while (!TryEnterCriticalSection(&g_mainFlushLock)) {
            if (tscfix::overlap::GameThreadJoining() && tscfix::overlap::GameThreadHolds(&g_mainFlushLock)) return;
            YieldProcessor();
        }
        held = true;
    }
    ~MainFlushTurn() {
        if (held) LeaveCriticalSection(&g_mainFlushLock);
    }
};
void QueueFlush(void* q, void* a, void* b) {
    QueueOp op(3, _ReturnAddress());
    MainFlushTurn turn;
    void* own = OwnQueue(q);
    if (g_shadowClearSnapshots && overlapstate::ShadowRole()) ClearSlotSnapshots(own);
    InterlockedIncrement(&g_use[kUseQueueFlush].entries);
    int slot = -1;
    LONG me = static_cast<LONG>(GetCurrentThreadId());
    for (int i = 0; i < 4 && slot < 0; ++i)
        if (InterlockedCompareExchange(&g_flushingBy[i], me, 0) == 0) slot = i;
    if (slot >= 0) g_flushing[slot] = own;
    g_queueFlush(own, a, b);
    if (slot >= 0) g_flushing[slot] = nullptr, InterlockedExchange(&g_flushingBy[slot], 0);
}
void QueueReset(void* q) {
    QueueOp op(t_queueOp == 1 ? 1 : 2, _ReturnAddress());  // (begin calls reset)
    void* own = OwnQueue(q);
    QueueLock lock(own == q && !t_ownHit && g_qs[0].n && !CallerOnlyQueue(q));
    g_queueReset(own);
}

void* OwnQueueFor(void* shared) {
    if (!shared) return nullptr;
    for (int i = 0; i < QS().n; ++i)
        if (QS().q[i].shared == shared) return QS().q[i].own;
    if (QS().n == static_cast<int>(std::size(QS().q))) return nullptr;
    void* own = MakeQueue();
    if (!own) return nullptr;
    int n = QS().n;
    QS().q[n] = {shared, own};
    QS().n = n + 1;
    Log("research overlap: private sorted queue %p for the worker (shared %p)", own, shared);
    return own;
}

// --- "shadow pass running" flag ----------------------------------------------------------------
// iShadowMapManager InShadowPass returns a manager flag (+0x7cd) that is set for the shadow
// cascades; render code asks it to pick shadow-only paths. With the cascades on the worker, the
// game thread clears it while the worker still draws them, so the shadow-role thread answers
// "yes" and every other thread gets the flag.
uintptr_t kInShadowPass = 0;
using InShadowPassFn = uint8_t (*)(void* self);
InShadowPassFn g_inShadowPass = nullptr;
// (switch passflag) during an overlap, threads other than the shadow role answer "no": the
// worker's cascade setup sets the shared flag while the main view draws beside it.
volatile bool g_swPassFlag = false;
volatile long g_passFlagSeen = 0;  // a thread without the shadow role read "yes" while a worker drew
uint8_t InShadowPass(void* self) {
    if (overlapstate::ShadowRole()) return 1;
    uint8_t flag = g_inShadowPass(self);
    if (flag && rt::OverlapRunning()) {
        InterlockedIncrement(&g_passFlagSeen);
        if (g_swPassFlag) return 0;
    }
    return flag;
}

// --- graphics engine shadow-mode flag ----------------------------------------------------------
// cGraphicsEngine EngineModeGet / EngineModeSet get / set an engine flag (+0x108) that is on for
// the shadow pass and restored on the game thread while the worker may still be drawing
// cascades. The worker uses its own copy, taken at launch.
uintptr_t kEngineModeGet = 0, kEngineModeSet = 0;
constexpr size_t kEngineMode = 0x108;
using ModeGetFn = uint8_t (*)(void* engine);
using ModeSetFn = void (*)(void* engine, uint8_t v);
ModeGetFn g_modeGet = nullptr;
ModeSetFn g_modeSet = nullptr;
uint8_t g_workerMode[2] = {};  // per shadow slot: written by the game thread before launch, then only by that thread
uint8_t ModeGet(void* engine) { return overlapstate::ShadowRole() ? g_workerMode[t_slot] : g_modeGet(engine); }
void ModeSet(void* engine, uint8_t v) {
    if (overlapstate::ShadowRole()) g_workerMode[t_slot] = v;
    else g_modeSet(engine, v);
}

// --- shared per-instance state the worker must not write ---------------------------------------
// Drawing an object writes instance data into the geometry every instance shares: material
// overrides (MaterialOverrides), day/night visibility flags (DayNightFlags, returns "changed"), and the
// skeleton palette recomputed in place (SkeletonPalette). Both passes write the same values, but
// interleaved between threads one instance's values reach another's draw. The worker skips
// these writes and draws with the values the game thread keeps current.
uintptr_t kMaterialOverrides = 0, kDayNightFlags = 0, kSkeletonPalette = 0,
                    kNodeVisibility = 0;  // (model, visible, child name): shows / hides named nodes
using VoidFn = void (*)(void* obj);
using DayNightFn = uint64_t (*)(void* obj);
using PaletteFn2 = void (*)(void* skeleton, const void* world);
VoidFn g_materialOverrides = nullptr;
DayNightFn g_dayNightFlags = nullptr;
PaletteFn2 g_skeletonPalette = nullptr;
void MaterialOverrides(void* obj) {
    if (!overlapstate::ShadowRole()) g_materialOverrides(obj);
}
uint64_t DayNightFlags(void* obj) { return overlapstate::ShadowRole() ? 0 : g_dayNightFlags(obj); }
// Animation evaluation (AnimationEvaluate: skeleton, time, ...) fills shared scratch globals and the
// skeleton's local matrices; its only consumer on the worker (the palette) is skipped too.
uintptr_t kAnimationEvaluate = 0;
using AnimationEvaluateFn = void (*)(void* skeleton, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
AnimationEvaluateFn g_animationEvaluate = nullptr;
void AnimationEvaluate(void* skeleton, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (overlapstate::ShadowRole()) return;
    g_animationEvaluate(skeleton, a, b, c, d);
}
using NodeVisibilityFn = void (*)(void* model, uint8_t visible, void* name);
NodeVisibilityFn g_nodeVisibility = nullptr;
void NodeVisibility(void* model, uint8_t visible, void* name) {
    if (overlapstate::ShadowRole()) return;
    g_nodeVisibility(model, visible, name);
}
// The skip lost one-time palettes: a model's animation setup (cAnimObjectRender) computes the
// starting palette once, and when the shadow worker ran it first, the skeleton kept an empty
// palette and the main view drew a skinned body with it collapsed (Haarlem, 20 Nov: the bodies of
// parked SLT units vanished at their near level of detail, only the rigid end parts stayed; vanilla,
// one thread and overlap=0 kept them). The worker still never writes the palette: it records the
// call (with a copy of the world matrix) and the game thread makes it right after the shadow pass
// has joined. Per-frame calls give the values the main view writes anyway.
struct PaletteCall {
    void* skeleton;
    float world[16];
};
constexpr int kPaletteCalls = 4096;
PaletteCall g_paletteCalls[kPaletteCalls];
volatile LONG g_paletteCallCount = 0;
volatile LONG g_paletteDeferred = 0, g_paletteReplayed = 0, g_paletteDropped = 0, g_paletteFirst = 0;
// A palette never computed: the skeleton's palette (+0x48) starts with the world matrix, whose last
// element is 1 once it was written. A new skeleton (a model that just switched to its near level of
// detail) has none yet, and the main view would draw it before the join (one frame without the
// body, 20 Nov run with the deferred calls only). Such a palette is made at once on the worker:
// nobody else writes it yet.
bool PaletteNeverMade(void* skeleton) {
    __try {
        const float* palette = *reinterpret_cast<float* const*>(static_cast<uint8_t*>(skeleton) + 0x48);
        return palette && palette[15] != 1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void SkeletonPalette(void* skeleton, const void* world) {
    if (!overlapstate::ShadowRole()) return g_skeletonPalette(skeleton, world);
    if (!skeleton || !world) return;
    if (PaletteNeverMade(skeleton)) InterlockedIncrement(&g_paletteFirst);  // (counted only: making it here raced, 20 Nov)
    LONG i = InterlockedIncrement(&g_paletteCallCount) - 1;
    if (i >= kPaletteCalls) {
        InterlockedIncrement(&g_paletteDropped);
        return;
    }
    g_paletteCalls[i].skeleton = skeleton;
    memcpy(g_paletteCalls[i].world, world, sizeof g_paletteCalls[i].world);
    InterlockedIncrement(&g_paletteDeferred);
}
// Game thread, after the shadow pass joined (no worker draws now).
void ReplayPalettes() {
    LONG n = InterlockedExchange(&g_paletteCallCount, 0);
    if (n > kPaletteCalls) n = kPaletteCalls;
    // One call per skeleton, the last one recorded (newest first, a small open-address set).
    static void* seen[8192];
    memset(seen, 0, sizeof seen);
    for (LONG i = n - 1; i >= 0; --i) {
        void* s = g_paletteCalls[i].skeleton;
        size_t h = (reinterpret_cast<uintptr_t>(s) >> 4) * 0x9E3779B97F4A7C15ull >> 51;
        bool dup = false;
        while (seen[h]) {
            if (seen[h] == s) { dup = true; break; }
            h = (h + 1) & 8191;
        }
        if (dup) continue;
        seen[h] = s;
        g_skeletonPalette(s, g_paletteCalls[i].world);
        InterlockedIncrement(&g_paletteReplayed);
    }
}
// A scenery object with numbers or swapped textures (signs, platform numbers) rebuilds its model's
// replacement list at every draw: the list is emptied (its nodes freed) and filled again. The
// depth-only shadow draw does not use those textures, and two shadow threads drawing the same sign
// freed the same nodes (split2: heap corruption on the first frames). The shadow role leaves the
// list as the main view keeps it.
constexpr sites::Fingerprint kNumberTextures = {0xdea87b6860dbe60full, 24};
VoidFn g_numberTextures = nullptr;
void NumberTextures(void* object) {
    if (!overlapstate::ShadowRole()) g_numberTextures(object);
}

// The main-view draw of such an object (model type 0xe: signs, numbered objects) puts the
// instance's replacement materials into the model every instance shares (MaterialOverrides),
// draws, and puts the originals back. A shadow thread that draws another instance of that model
// in between takes a replacement material into its sorted queue. The owner frees its replacements
// at its next draw, which can be in the same frame: the shadow thread then holds a freed material
// (runs split-8min third attempt and fork-split-ab: write fault in material setup, the material's
// counter held leftover data, its reference count was -1).
//
// Locks, one per model (by hash): a main-view draw takes its model's lock only when the object has
// replacements (it is the only kind that writes the shared model). A shadow-role draw of such a
// model always takes it, so it never reads the model inside that window and only sees the
// originals. One lock for all models made every draw of this model type wait on every other
// (run locks-two: 9,100 waits and 10.8 ms of waiting per shadow pass).
constexpr sites::Fingerprint kOverrideDrawA = {0xaaa9c1de292c9a62ull, 16};
constexpr sites::Fingerprint kOverrideDrawB = {0x8284b7fa16ece8fbull, 16};
constexpr size_t kObjectModel = 8, kModelType = 0x38, kObjectReplacements = 0x50;
constexpr int kModelWithOverrides = 0xe;
using OverrideDrawFn = void (*)(void* a, void* object, void* c, uint8_t d);
OverrideDrawFn g_overrideDraw[2] = {};
constexpr size_t kOverrideLocks = 64;  // power of two
CRITICAL_SECTION g_overrideLock[kOverrideLocks];
// The model, if it is of the type whose instances can carry replacements.
uint8_t* ModelWithOverrides(void* object) {
    if (!object) return nullptr;
    uint8_t* model;
    memcpy(&model, static_cast<uint8_t*>(object) + kObjectModel, 8);
    int type = 0;
    if (model) memcpy(&type, model + kModelType, 4);
    return type == kModelWithOverrides ? model : nullptr;
}
// The object's replacement list (a ring with a head node) has entries.
bool HasReplacements(void* object) {
    void** head;
    memcpy(&head, static_cast<uint8_t*>(object) + kObjectReplacements, 8);
    return head && *head != head;
}
// A shadow-role draw of an instance WITH replacements also writes the shared model (the engine's
// function puts the replacements in for every pass). A main-view draw of another instance WITHOUT
// replacements took no lock: its queue items copy the slot values the model holds at that moment,
// so inside the shadow thread's window they copied the other instance's values. Seen by the user
// on the UK route since the first split: on a gantry with two signals of one model, the signal at
// green was drawn with the red signal's lamp and housing for single frames (capture set
// 230446-set4, frames 7, 31, 33, 54, 55, 58 of 60). [fixes] override_lock_all=1 (default): while a
// shadow thread draws, every main-view draw of such a model takes the model's lock.
// g_overrideWindow marks an open shadow-role window per lock; g_overrideRaces counts main-view
// draws without replacements that ran with no lock while the window of their lock was open.
volatile bool g_overrideLockAll = true;
volatile LONG g_overrideWindow[kOverrideLocks] = {};
volatile LONG g_overrideRaces = 0, g_overrideMainNoRepl = 0;
template <int K>
void OverrideDraw(void* a, void* object, void* c, uint8_t d) {
    SignBiasEnd biasEnd;  // (on every way out of this function)
    uint8_t* model = ModelWithOverrides(object);
    // (With the scenery thread there are two main-view threads: each takes the slot values the
    // model holds at that moment into its queue item, so a draw without replacements must stay out
    // of the other thread's window too.)
    bool shadow = overlapstate::ShadowRole();
    bool reader = shadow || tscfix::overlap::ForkActive() || (g_overrideLockAll && tscfix::overlap::Overlapping());
    if (!model) return g_overrideDraw[K](a, object, c, d);
    uintptr_t h = reinterpret_cast<uintptr_t>(model);
    size_t index = ((h >> 6) ^ (h >> 14)) & (kOverrideLocks - 1);
    bool replacements = HasReplacements(object);
    if (!reader && !replacements) {
        if (tscfix::overlap::Overlapping()) {
            InterlockedIncrement(&g_overrideMainNoRepl);
            if (g_overrideWindow[index]) InterlockedIncrement(&g_overrideRaces);
        }
        return g_overrideDraw[K](a, object, c, d);
    }
    if (!shadow && !replacements && tscfix::overlap::Overlapping()) {  // (the case the lock is new for)
        InterlockedIncrement(&g_overrideMainNoRepl);
        if (g_overrideWindow[index]) InterlockedIncrement(&g_overrideRaces);  // it now waits for that window to close
    }
    SharedRenderLock lock(&g_overrideLock[index], kLockOverride);
    if (shadow && replacements) InterlockedIncrement(&g_overrideWindow[index]);
    g_overrideDraw[K](a, object, c, d);
    if (shadow && replacements) InterlockedDecrement(&g_overrideWindow[index]);
}

void* g_hookTargets[64];
int g_hookTargetCount = 0;

bool Hook(uintptr_t rva, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(g_base + rva);
    bool ok = MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    if (ok && g_hookTargetCount < 64) g_hookTargets[g_hookTargetCount++] = target;
    if (!ok) Log("research overlap: cannot hook %llx", static_cast<unsigned long long>(rva));
    return ok;
}

}  // namespace

// The current culling volume (cHcCullingVolume, 0xc4 bytes: plane count + 12 planes), which the
// binder points at (+0x40): the shadow cascades rewrite its planes, so the worker culls with a
// private copy.
constexpr size_t kBinderCulling = 0x40, kCullingSize = 0xc4;
alignas(16) uint8_t g_workerCulling[256];

// The scenery thread draws the main view's scenery while the game thread draws track, terrain and
// trains: two main-view walks at the same time, on the same two engine flags. A wrong "tile inside
// the frustum" flag culls objects for a frame (objects blinking in and out), a wrong light flag
// changes lights. [experiments] fork_private_flags (default 1): the scenery thread keeps its own
// copies for the length of its job, as the shadow pass does.
volatile bool g_forkPrivateFlags = true;
volatile LONG g_forkRunning = 0;           // the scenery thread is inside its job
thread_local unsigned t_forkJob = 0;       // jobs this thread began (scenery thread only)
// (measurement) accesses to the shared flags while the scenery thread runs: [0] game thread, [1] scenery thread
volatile LONG g_sharedFlagUse[2] = {};
void LogSeedReads();
void LogLeftovers();
void LogSharedFlagUse() {
    LONG game = InterlockedExchange(&g_sharedFlagUse[0], 0), scenery = InterlockedExchange(&g_sharedFlagUse[1], 0);
    if (game || scenery)
        Log("research overlap: shared walk flags (tile in frustum, light) used while the scenery thread ran, last 600 passes: game thread %ld, "
            "scenery thread %ld%s", game, scenery, g_forkPrivateFlags ? " (the scenery thread has its own copies)" : "");
}

thread_local unsigned t_flagPass = 0;  // shadow passes this thread began (private walk flags reseed)

extern volatile unsigned g_shadowPassId;
void EndWorkerPass() {
    CloseModifierOf(binder::Binder());
    static unsigned passes = 0;  // (shadow thread only)
    if (++passes % 600 == 0) LogSharedUse(), LogSharedQueueUse(), LogLockWaits();
    if (passes % 6000 == 0) refcensus::LogUse();
    if (passes % 600 == 0) lodprobe::LogUse();
    if (passes % 600 == 0) LogSharedFlagUse();
    if (passes % 600 == 0) LogSeedReads();
    if (passes % 600 == 0) LogLeftovers();
    if (passes % 600 == 0) {
        LONG races = InterlockedExchange(&g_overrideRaces, 0), plain = InterlockedExchange(&g_overrideMainNoRepl, 0);
        if (plain || races)
            Log("research overlap: main-view draws of a sign or signal model without replacements: %ld (last 600 passes), of which %ld came while a "
                "shadow thread had another instance's replacements in that model (%s)",
                plain, races, g_overrideLockAll ? "they waited for the lock" : "NO lock: they copied those values");
    }
    if (passes % 600 == 0)
        if (long seen = InterlockedExchange(&g_passFlagSeen, 0))
            Log("research overlap: the shared shadow-pass flag was read as set by a thread without the shadow role %ld times (last 600 passes)%s",
                seen, g_swPassFlag ? "; answered no" : "");
    if (passes % 600 == 0)
        if (long saves = rt::ParkSaves()) Log("research overlap: device state saved and put back around %ld parked direct calls (last 600 passes)", saves);
    TSCFIX_TRACE(trace::kOverlap, trace::kWorkerPassEnd, static_cast<uint64_t>(g_shadowPassId));
    trace::SetFlag(1, false);
}
void BeginWorkerPass() {
    if (!g_base) return;  // not installed (binder-only configuration)
    ++g_shadowPassId;
    trace::SetFlag(1, true);
    TSCFIX_TRACE(trace::kOverlap, trace::kWorkerPassBegin, static_cast<uint64_t>(g_shadowPassId));
    // Private queues the worker fills but never flushes start empty each pass.
    for (int i = 0; i < QS().n; ++i) g_queueReset(QS().q[i].own);
    if (g_tileDraw) ShadowTilesNextUpdate();
    ++t_flagPass;
}

void BeforeShadowLaunch() { RefreshTileBounds(); }

// --- geometry library LOD distance override (switch lodov) -------------------------------------
// cGeometryLibrary keeps a LOD distance override (+0x10, accessors LodOvSet / LodOvGet) that is
// set per shadow cascade and restored on the game thread while the worker still draws (and the
// main view reads the worker's value meanwhile). The shadow
// role keeps its own value for the current shadow pass; the shared field is the game thread's.
// Default on (shadow_private_lod): the remaining distant-detail differences were the shadow
// cascades' override leaking into the main view (lodCullPass1).
volatile bool g_swLodOv = true;
uintptr_t kLodOvSet = 0, kLodOvGet = 0;
volatile uint32_t g_shadowLodOv[2] = {};  // per shadow slot
volatile unsigned g_shadowLodOvPass[2] = {}, g_shadowPassId = 0;
void LodOvSet(void* self, uint32_t v) {
    if (g_swLodOv && overlapstate::ShadowRole()) {
        g_shadowLodOv[t_slot] = v, g_shadowLodOvPass[t_slot] = g_shadowPassId;
        return;
    }
    // (diagnosis) a forced LOD distance reaching the shared field: who wrote it.
    if (v != 0xffffffffu) {
        static volatile long lines = 0;
        if (InterlockedIncrement(&lines) <= 40)
            Log("lod override: SHARED field set to %d (was %d) by tid %lu caller rva %llx", static_cast<int>(v),
                *reinterpret_cast<int*>(static_cast<uint8_t*>(self) + 0x10), GetCurrentThreadId(),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base));
    }
    memcpy(static_cast<uint8_t*>(self) + 0x10, &v, 4);
}
uint32_t LodOvGet(void* self) {
    if (g_swLodOv && overlapstate::ShadowRole() && g_shadowLodOvPass[t_slot] == g_shadowPassId && g_shadowPassId)
        return g_shadowLodOv[t_slot];
    uint32_t v;
    memcpy(&v, static_cast<uint8_t*>(self) + 0x10, 4);
    // (diagnosis) the shared field read while it holds a forced distance: by whom.
    if (v != 0xffffffffu) {
        static volatile long lines = 0;
        long n = InterlockedIncrement(&lines);
        if (n <= 40 || n % 20000 == 0)
            Log("lod override: shared field read as %d by tid %lu (shadow role %d) caller rva %llx [read #%ld]", static_cast<int>(v),
                GetCurrentThreadId(), overlapstate::ShadowRole() ? 1 : 0,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base), n);
    }
    return v;
}
// --- graphics engine cull-inversion flag (switch cullinv) --------------------------------------
// cGraphicsEngine CullInvGet / CullInvSet get / set a flag (+0x1d5) that picks the cull mode of
// material binds; the shadow pass flips it around some draws while the main view binds materials
// beside it.
// The shadow role keeps its own value (seeded from the engine's at each shadow pass).
// With two shadow threads it must: each flips the flag around its cab draw and puts back what it
// read, and interleaved that leaves the shared flag inverted for the main view.
volatile bool g_swCullInv = false;
uintptr_t kCullInvGet = 0, kCullInvSet = 0;
volatile uint8_t g_shadowCullInv[2] = {};  // per shadow slot
volatile unsigned g_shadowCullInvPass[2] = {};
uint8_t CullInvGet(void* self) {
    uint8_t v = static_cast<uint8_t*>(self)[0x1d5];
    if ((g_swCullInv || g_splitShadow) && overlapstate::ShadowRole() && g_shadowPassId)
        return g_shadowCullInvPass[t_slot] == g_shadowPassId ? g_shadowCullInv[t_slot] : v;
    return v;
}
void CullInvSet(void* self, uint8_t v) {
    if ((g_swCullInv || g_splitShadow) && overlapstate::ShadowRole() && g_shadowPassId) {
        g_shadowCullInv[t_slot] = v, g_shadowCullInvPass[t_slot] = g_shadowPassId;
        return;
    }
    static_cast<uint8_t*>(self)[0x1d5] = v;
}

// Every vtable entry pointing at `from` (in the engine's .rdata) now points at `to`.
int RedirectVtableEntries(uintptr_t from, void* to) {
    auto* base = reinterpret_cast<uint8_t*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (memcmp(sec[s].Name, ".rdata", 6)) continue;
        for (DWORD p = sec[s].VirtualAddress; p + 8 <= sec[s].VirtualAddress + sec[s].Misc.VirtualSize; p += 8) {
            auto* slot = reinterpret_cast<uintptr_t*>(base + p);
            if (*slot != from) continue;
            DWORD old;
            VirtualProtect(slot, 8, PAGE_READWRITE, &old);
            *slot = reinterpret_cast<uintptr_t>(to);
            VirtualProtect(slot, 8, old, &old);
            ++n;
        }
    }
    return n;
}

void PrepareWorker(uint8_t* binderCopy) {
    if (!g_base) return;
    // The engine's queue and the one the binder points at; the worker's binder copy points
    // at the worker's own.
    while (QS().spares < 2) {  // (the shadow thread is idle: only this thread touches the spares)
        void* spare = MakeQueue();
        if (!spare) break;
        int n = QS().spares;
        QS().spare[n] = spare;
        QS().spares = n + 1;
    }
    void* engine = reinterpret_cast<EngineGetFn>(g_base + kEngineGet)();
    if (engine) {
        OwnQueueFor(*reinterpret_cast<void**>(static_cast<uint8_t*>(engine) + kEngineQueue));
        for (size_t field : kEngineQueueField)
            OwnQueueFor(*reinterpret_cast<void**>(static_cast<uint8_t*>(engine) + field));
        g_workerMode[0] = g_workerMode[1] = static_cast<uint8_t*>(engine)[kEngineMode];
    }
    if (binderCopy) {
        void* shared;
        memcpy(&shared, binderCopy + kBinderQueue, 8);
        if (shared && shared != g_forkQueue) g_binderQueue = shared;
        if (void* own = OwnQueueFor(shared)) memcpy(binderCopy + kBinderQueue, &own, 8);
        uint8_t* volume;
        memcpy(&volume, binderCopy + kBinderCulling, 8);
        if (volume && volume != g_workerCulling) {
            memcpy(g_workerCulling, volume, kCullingSize);
            uint8_t* own = g_workerCulling;
            memcpy(binderCopy + kBinderCulling, &own, 8);
        }
    }
}

// --- second shadow thread (experiments.cpp: some shadow maps drawn beside the others) ----------
// It draws with its own binder copy (taken from the first shadow thread's when the maps are
// handed out), its own sorted queues (slot 1's table), culling volume and per-pass values.
uint8_t* g_splitCopy = nullptr;
void* g_splitBinderQueue = nullptr;
alignas(16) uint8_t g_splitCulling[256];

void SetSplit(bool on) { g_splitShadow = on; }
void SetOverrideLockAll(bool on) { g_overrideLockAll = on; }  // (in-session A/B)

// Game thread, before the shadow pass launches (after PrepareWorker): slot 1's queues.
void PrepareSplit() {
    if (!g_base || !g_splitShadow) return;
    if (!g_splitCopy) g_splitCopy = binder::NewCopy();
    SlotScope slot(1);
    while (QS().spares < 2) {
        void* spare = MakeQueue();
        if (!spare) break;
        int n = QS().spares;
        QS().spare[n] = spare;
        QS().spares = n + 1;
    }
    if (void* engine = reinterpret_cast<EngineGetFn>(g_base + kEngineGet)()) {
        OwnQueueFor(*reinterpret_cast<void**>(static_cast<uint8_t*>(engine) + kEngineQueue));
        for (size_t field : kEngineQueueField)
            OwnQueueFor(*reinterpret_cast<void**>(static_cast<uint8_t*>(engine) + field));
    }
    if (g_binderQueue) g_splitBinderQueue = OwnQueueFor(g_binderQueue);
}

void ResetOwnBinder();
// First shadow thread, before it starts its maps: the second thread's binder copy is a snapshot
// of this thread's, closed (no material bound: the second thread's commands are replayed after
// all of this thread's).
bool SplitSnapshot() {
    if (!g_splitCopy || !g_splitBinderQueue) return false;
    ResetOwnBinder();
    binder::SnapshotFromCurrent(g_splitCopy);
    memcpy(g_splitCopy + kBinderQueue, &g_splitBinderQueue, 8);
    uint8_t* volume;
    memcpy(&volume, g_splitCopy + kBinderCulling, 8);
    if (volume) {
        memcpy(g_splitCulling, volume, kCullingSize);
        uint8_t* own = g_splitCulling;
        memcpy(g_splitCopy + kBinderCulling, &own, 8);
    }
    return true;
}
// Second shadow thread, around its maps.
void SplitEnter() {
    t_slot = 1;
    t_lastQueue = {};
    binder::UseCopy(g_splitCopy);
    for (int i = 0; i < QS().n; ++i) g_queueReset(QS().q[i].own);
    ++t_flagPass;
}
void SplitLeave() {
    ResetOwnBinder();
    CloseModifierOf(binder::Binder()), binder::UseCopy(nullptr);
}

// --- forked render module (overlap::SetForkHooks) -------------------------------------------
// A module forked onto the second worker (the main-view scenery) draws with its own binder
// copy, taken on the game thread at the fork: its sorted queue (+0x28, opened and flushed inside
// the module) and its culling volume (+0x40) are private; everything else it shares as the game
// thread did.
uint8_t* g_forkCopy = nullptr;
alignas(16) uint8_t g_forkCulling[256];

// (measurement, [experiments] scenery_leftovers=1) What the scenery leaves in the draw binder for
// the modules after it. Default build: the binder before and after the scenery module on the game
// thread. Scenery thread: its binder copy at the start and at the end of the job, and the game
// thread's binder when the job is joined. Counted per 8-byte word of the binder.
constexpr size_t kBinderWords = 0x19f0 / 8;
volatile bool g_sceneryLeftovers = false;
uint64_t g_binderBefore[kBinderWords], g_forkBefore[kBinderWords], g_forkAfter[kBinderWords];
volatile LONG g_wordsStock[kBinderWords], g_wordsFork[kBinderWords], g_wordsJoin[kBinderWords];
volatile LONG g_leftoverFrames[2] = {};
volatile bool g_forkAfterValid = false;
// The copy as taken, against the game thread's binder at the scenery module's own place in the
// engine's order: the words that differ are stale in the copy (set by a module that the engine
// runs between the two moments).
uint64_t g_forkTaken[kBinderWords];
volatile LONG g_wordsSlot[kBinderWords];
volatile LONG g_slotJobs = 0;
// The engine's matrix pipeline (cHcMatrixPipeline) sits in the binder at +0x100: 49 matrices of
// 64 bytes (from-space * 7 + to-space) and one "valid" byte per matrix at +0x1980. Derived
// matrices are made on demand and marked valid. A matrix in a copy is wrong when its valid byte
// is set and its value is not the value the engine's own order gives the scenery module.
constexpr int kMatrices = 49;
uint64_t g_unGated[kBinderWords];  // the game thread's binder at the first module after the shadow launch
volatile LONG g_wrongValid[2][kMatrices];  // [copy taken at the overlap start, copy as taken now]
void CountWrongValid(const uint64_t* copy, const uint64_t* now, volatile LONG* counts) {
    const uint8_t* valid = reinterpret_cast<const uint8_t*>(copy) + 0x1980;
    for (int i = 0; i < kMatrices; ++i)
        if (valid[i] && memcmp(copy + (0x100 + i * 0x40) / 8, now + (0x100 + i * 0x40) / 8, 0x40)) InterlockedIncrement(&counts[i]);
}
void CountDiff(const uint64_t* a, const uint64_t* b, volatile LONG* counts) {
    for (size_t i = 0; i < kBinderWords; ++i)
        if (a[i] != b[i]) InterlockedIncrement(&counts[i]);
}
// (measurement) The screen-size values of the binder (+0x740, +0x758 as floats) when the game
// thread starts each render module, and when the scenery thread's copy is taken.
void LeftoverScreenSize(const char* where) {
    static volatile LONG lines = 0;
    if (!g_sceneryLeftovers || InterlockedIncrement(&lines) > 90 || strstr(where, "HeadLightFlare")) return;
    float w, h;
    uint64_t m[2], f[3];
    memcpy(&w, binder::Global() + 0x744, 4);
    memcpy(&h, binder::Global() + 0x758, 4);
    memcpy(m, binder::Global() + 0x4c0, 16);
    memcpy(f, binder::Global() + 0x1980, 24);
    Log("scenery leftovers: screen-size values in the game thread's binder at %s: %.0f, %.0f; +4c0 %llx %llx; +1980 %llx %llx %llx", where, w, h,
        static_cast<unsigned long long>(m[0]), static_cast<unsigned long long>(m[1]), static_cast<unsigned long long>(f[0]),
        static_cast<unsigned long long>(f[1]), static_cast<unsigned long long>(f[2]));
}
// (measurement, [experiments] binder_watch=<hex offsets, up to 4, comma separated>) Which engine
// code reads or writes these words of the game thread's binder, and in which render module. Uses
// the processor's data breakpoints on the game thread for a bounded number of hits.
size_t g_watchOffsets[4];
int g_watchCount = 0;
volatile LONG g_watchHitsLeft = 0, g_watchArmed = 0, g_watchSeen = 0;
const char* volatile g_watchModule = "";
struct WatchHit {
    uintptr_t rva;
    int slot;
    const char* module;
    LONG count;
    uint64_t value;
};
WatchHit g_watchHits[160];
void WatchSetRegisters(CONTEXT* c, bool on) {
    c->Dr7 = 0;
    for (int i = 0; on && i < g_watchCount; ++i) {
        (&c->Dr0)[i] = reinterpret_cast<DWORD64>(binder::Global() + g_watchOffsets[i]);
        c->Dr7 |= 1ull << (i * 2);              // local enable
        c->Dr7 |= 3ull << (16 + i * 4);         // read or write
        // (length: one byte; the offsets name single bytes or the first byte of a word)
    }
}
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 hit = info->ContextRecord->Dr6 & 0xf;
    info->ContextRecord->Dr6 = 0;
    uintptr_t rva = static_cast<uintptr_t>(info->ContextRecord->Rip) - g_base;
    const char* module = g_watchModule;
    for (int i = 0; i < g_watchCount; ++i) {
        if (!(hit & (1ull << i))) continue;
        uint64_t value = 0;  // (the handler must not touch the watched words itself)
        LONG n = g_watchSeen;
        int k = 0;
        for (; k < n; ++k)
            if (g_watchHits[k].rva == rva && g_watchHits[k].slot == i && g_watchHits[k].module == module) break;
        if (k == n && n < 160) g_watchHits[n] = {rva, i, module, 0, value}, g_watchSeen = n + 1;
        if (k < 160) ++g_watchHits[k].count;
    }
    if (InterlockedDecrement(&g_watchHitsLeft) <= 0) WatchSetRegisters(info->ContextRecord, false);
    return EXCEPTION_CONTINUE_EXECUTION;
}
void WatchArm() {  // game thread
    if (!g_watchCount || InterlockedCompareExchange(&g_watchArmed, 1, 0)) return;
    AddVectoredExceptionHandler(1, &WatchHandler);
    g_watchHitsLeft = 60000;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    WatchSetRegisters(&c, true);
    SetThreadContext(GetCurrentThread(), &c);
    Log("binder watch: armed on the game thread for %d offset(s)", g_watchCount);
}
void LogWatch() {
    static LONG logged = 0;
    if (!g_watchArmed || g_watchHitsLeft > 0 && g_watchSeen == logged) return;
    LONG n = g_watchSeen;
    if (n == logged) return;
    for (LONG k = logged; k < n; ++k)
        Log("binder watch: +%zx touched %ld times by code before rva %llx in %s (word then: %llx)", g_watchOffsets[g_watchHits[k].slot],
            g_watchHits[k].count, static_cast<unsigned long long>(g_watchHits[k].rva), g_watchHits[k].module,
            static_cast<unsigned long long>(g_watchHits[k].value));
    logged = n;
}
void LeftoverModuleBegin(const char* className) {
    g_watchModule = className;
    if (g_watchCount && strstr(className, "cShadowMapManager")) {
        static int frames = 0;
        if (++frames == 400) WatchArm();
    }
    LeftoverScreenSize(className);
    if (g_sceneryLeftovers && strstr(className, "cSceneryManager")) memcpy(g_binderBefore, binder::Global(), sizeof g_binderBefore);
    if (g_sceneryLeftovers && strstr(className, "cDepthPass")) memcpy(g_unGated, binder::Global(), sizeof g_unGated);
}
void LeftoverModuleEnd(const char* className) {
    if (!g_sceneryLeftovers || !strstr(className, "cSceneryManager")) return;
    uint64_t now[kBinderWords];
    memcpy(now, binder::Global(), sizeof now);
    CountDiff(g_binderBefore, now, g_wordsStock);
    if (InterlockedIncrement(&g_leftoverFrames[0]) % 200 == 1)
        Log("scenery leftovers: values, game thread before -> after the scenery: +e0 %llx -> %llx; +c8 %llx -> %llx; +740 %llx -> %llx; +4c0 %llx -> %llx; "
            "+1980 %llx -> %llx",
            g_binderBefore[0xe0 / 8], now[0xe0 / 8], g_binderBefore[0xc8 / 8], now[0xc8 / 8], g_binderBefore[0x740 / 8], now[0x740 / 8],
            g_binderBefore[0x4c0 / 8], now[0x4c0 / 8], g_binderBefore[0x1980 / 8], now[0x1980 / 8]);
}
void LogLeftovers() {
    LogWatch();
    if (!g_sceneryLeftovers) return;
    auto line = [](const char* what, volatile LONG* counts, LONG frames) {
        if (!frames) return;
        char text[900];
        int n = 0, words = 0;
        for (size_t i = 0; i < kBinderWords; ++i) {
            LONG c = InterlockedExchange(&counts[i], 0);
            if (!c) continue;
            ++words;
            if (n < 840) n += sprintf_s(text + n, sizeof text - n, " +%zx:%ld", i * 8, c);
        }
        Log("scenery leftovers: %s, %ld jobs, %d binder words differ (offset:jobs):%s", what, frames, words, n ? text : " none");
    };
    LONG stock = InterlockedExchange(&g_leftoverFrames[0], 0), fork = InterlockedExchange(&g_leftoverFrames[1], 0);
    line("game thread's binder after the scenery module against before it", g_wordsStock, stock);
    line("scenery thread's binder copy at the end of its job against its start", g_wordsFork, fork);
    line("game thread's binder at the join against the scenery thread's copy at its end", g_wordsJoin, fork);
    LONG slotJobs = InterlockedExchange(&g_slotJobs, 0);
    line("game thread's binder at the scenery module's place against the scenery thread's copy as taken (stale words)", g_wordsSlot, slotJobs);
    for (int k = 0; k < 2 && slotJobs; ++k) {
        char text[700];
        int n = 0, wrong = 0;
        for (int i = 0; i < kMatrices; ++i) {
            LONG c = InterlockedExchange(&g_wrongValid[k][i], 0);
            if (!c) continue;
            ++wrong;
            if (n < 640) n += sprintf_s(text + n, sizeof text - n, " %d>%d (+%x): %ld", i / 7, i % 7, 0x100 + i * 0x40, c);
        }
        Log("scenery leftovers: matrices marked valid with a wrong value, copy taken %s, %ld jobs: %d%s",
            k ? "after the terrain module (as now)" : "at the first module after the shadow launch", slotJobs, wrong, n ? text : "");
    }
}

void ForkSlot() {
    if (!g_sceneryLeftovers) return;
    uint64_t now[kBinderWords];
    memcpy(now, binder::Global(), sizeof now);
    CountDiff(g_forkTaken, now, g_wordsSlot);
    CountWrongValid(g_unGated, now, g_wrongValid[0]), CountWrongValid(g_forkTaken, now, g_wrongValid[1]);
    if (InterlockedIncrement(&g_slotJobs) % 200 != 2) return;
    char text[1900];
    int n = 0;
    for (size_t i = 0; i < kBinderWords && n < 1800; ++i)
        if (g_forkTaken[i] != now[i])
            n += sprintf_s(text + n, sizeof text - n, " +%zx %llx|%llx", i * 8, static_cast<unsigned long long>(g_forkTaken[i]),
                           static_cast<unsigned long long>(now[i]));
    Log("scenery leftovers: stale words, copy as taken | game thread at the scenery module's place:%s", n ? text : " none");
}
void ForkPrepare(DWORD) {
    LeftoverScreenSize("the moment the scenery thread's copy is taken");
    if (!g_forkCopy) g_forkCopy = binder::NewCopy();
    if (!g_forkQueue) g_forkQueue = MakeQueue();
    binder::SnapshotInto(g_forkCopy);
    if (g_sceneryLeftovers) memcpy(g_forkTaken, g_forkCopy, sizeof g_forkTaken);
    if (g_forkQueue) memcpy(g_forkCopy + kBinderQueue, &g_forkQueue, 8);
    uint8_t* volume;
    memcpy(&volume, g_forkCopy + kBinderCulling, 8);
    if (volume) {
        memcpy(g_forkCulling, volume, kCullingSize);
        uint8_t* own = g_forkCulling;
        memcpy(g_forkCopy + kBinderCulling, &own, 8);
    }
}
// (Its model draws compute skeleton palettes into a buffer of its own: the train and scenery
// tasks can draw the same model at the same moment.)
void ForkEnter() {
    binder::UseCopy(g_forkCopy), overlapstate::SetPrivateDraw(true);
    if (g_sceneryLeftovers && g_forkCopy) memcpy(g_forkBefore, g_forkCopy, sizeof g_forkBefore);
    ++t_forkJob;
    InterlockedExchange(&g_forkRunning, 1);
}
// The forked module's part of the command stream ends closed (no material left bound), and the
// game thread closes its own before the fork is taken and before it continues behind the fork:
// each part is replayed after another thread's, so none may rely on what its recorder left open.
void ResetOwnBinder() {  // (the binder method runs on the calling thread's copy when it has one)
    if (g_binderReset) g_binderReset(reinterpret_cast<void*>(g_base + kBinderGlobal), nullptr, 0);
}
void ForkKeepFinalFlags();
void ForkJoined();
void ForkLeave() {
    if (g_sceneryLeftovers && g_forkCopy) {
        // (after the engine's own end-of-module work, before our reset of the copy)
        memcpy(g_forkAfter, g_forkCopy, sizeof g_forkAfter);
        CountDiff(g_forkBefore, g_forkAfter, g_wordsFork);
        InterlockedIncrement(&g_leftoverFrames[1]);
        g_forkAfterValid = true;
    }
    ForkKeepFinalFlags();
    InterlockedExchange(&g_forkRunning, 0);
    ResetOwnBinder();
    CloseModifierOf(binder::Binder()), binder::UseCopy(nullptr);
    overlapstate::SetPrivateDraw(false);
}
void ForkCloseCaller() { ResetOwnBinder(); }

// --- entity reference counts ------------------------------------------------------------------
// cEntity::AddRef (EntityAddRef) / Release (EntityRelease) and cTenaciousEntity's (TenaciousAddRef /
// TenaciousRelease) count at +8, not atomically. The shadow cascades take and drop references
// on the camera entity while the game thread does too: a lost increment frees the camera early
// and the viewport (+0x28 points into it) calls into freed memory. The counts become atomic.
// AddRef keeps the engine's overflow assertion (>= 50000); Release at zero sets the count back
// to 1 and lets the original decrement it and destroy the entity, as it does on its own.
uintptr_t kEntityAddRef = 0, kEntityRelease = 0, kTenaciousAddRef = 0,
                    kTenaciousRelease = 0;
using RefFn = void (*)(void* self);
RefFn g_entityAddRef = nullptr, g_entityRelease = nullptr, g_tenaciousAddRef = nullptr, g_tenaciousRelease = nullptr;

template <RefFn* Original>
void AtomicAddRef(void* self) {
    auto* count = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(self) + 8);
    if (*count >= 0xC350) return (*Original)(self);  // the engine's overflow report
    _InterlockedIncrement(count);
}
template <RefFn* Original>
void AtomicRelease(void* self) {
    auto* count = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(self) + 8);
    if (_InterlockedDecrement(count) != 0) return;
    *count = 1;  // last reference: the original takes it to zero and destroys
    (*Original)(self);
}

namespace {

// --- the shadow walk's private flags -----------------------------------------------------------
// The scenery walk keeps a "tile inside the frustum" flag (engine +0x278) and a light flag
// (+0x390) in the engine while it draws; both walks write them.
uintptr_t kCullFlagSet = 0, kCullFlagGet = 0, kLightFlagSet = 0, kLightFlagGet = 0;
using FlagSetFn = void (*)(void* self, uint8_t v);
using FlagGetFn = uint8_t (*)(void* self);
FlagSetFn g_cullFlagSet = nullptr, g_lightFlagSet = nullptr;
FlagGetFn g_cullFlagGet = nullptr, g_lightFlagGet = nullptr;
thread_local uint8_t t_cullFlag = 0, t_lightFlag = 0;

// The shadow pass's walk keeps private copies, seeded per pass: otherwise it writes the flags
// the main view's walk reads beside it (most of the render-side differences while driving).
// [experiments] shadow_private_flags (default 1).
volatile bool g_swShadowFlags = true;
thread_local unsigned t_flagsSeeded = 0;
// The light flag (cLightManager +0x390) says "this draw takes the lights". A model draw sets it per
// object (two engine functions) and the material bind reads it. Every engine
// pass ends with the flag set to 1, so in the engine's own order a pass starts with 1. A worker
// thread's private copy took its start value from the shared flag at that moment, which another
// thread's object draw may have at 0. The passes read the flag before they write it (shadow
// threads about 10,000 times per pass, the scenery thread 64 times per job), so that start value
// was used. [experiments] light_flag_start_one (default 1): the private copy starts at 1.
volatile bool g_lightFlagStartOne = true;
volatile LONG g_seedWasZero[2] = {};  // (measurement) the shared flag was 0 at the start: [0] shadow pass, [1] scenery job
// (measurement) The shared light flag at the moment the game thread itself starts a render module,
// per module: the start value a module has in the engine's own order.
void* volatile g_lightManager = nullptr;  // the object the flag accessors were last called on
struct ModuleStart {
    char name[48];
    volatile LONG zero, one;
};
ModuleStart g_moduleStart[12];
void NoteModuleStart(const char* className) {
    void* self = g_lightManager;
    if (!self || !g_lightFlagGet) return;
    uint8_t flag = g_lightFlagGet(self);
    for (ModuleStart& m : g_moduleStart) {
        if (!m.name[0]) strncpy_s(m.name, className, _TRUNCATE);
        if (strncmp(m.name, className, sizeof m.name - 1)) continue;
        InterlockedIncrement(flag ? &m.one : &m.zero);
        return;
    }
}
uint8_t SeedLightFlag(void* self, int kind) {
    g_lightManager = self;
    uint8_t shared = g_lightFlagGet(self);
    if (!shared) InterlockedIncrement(&g_seedWasZero[kind]);
    // Shadow passes (kind 0) start with the shared value, as the engine's own shadow pass does.
    // The scenery job (kind 1) starts with 1: measured in the default build, every render module of
    // the main view starts with the flag at 1 (600 of 600 frames, run lightflag-default).
    return kind == 1 && g_lightFlagStartOne ? uint8_t{1} : shared;
}
bool OwnFlags(void* self) {
    if (overlapstate::ShadowRole()) {
        if (!g_swShadowFlags || !t_flagPass) return false;
        if (t_flagsSeeded != t_flagPass) {
            t_flagsSeeded = t_flagPass;
            t_cullFlag = g_cullFlagGet(self), t_lightFlag = SeedLightFlag(self, 0);
        }
        return true;
    }
    if (!g_forkRunning) return false;
    bool scenery = tscfix::overlap::OnForkWorker();
    if (scenery && g_forkPrivateFlags && t_forkJob) {
        unsigned tag = 0x80000000u | t_forkJob;
        if (t_flagsSeeded != tag) {
            t_flagsSeeded = tag;
            t_cullFlag = g_cullFlagGet(self), t_lightFlag = SeedLightFlag(self, 1);
        }
        return true;
    }
    InterlockedIncrement(&g_sharedFlagUse[scenery ? 1 : 0]);
    return false;
}
// (measurement) Reads of a private flag before this pass or job wrote it: then the value it was
// seeded with matters. [0] cull flag, [1] light flag; per kind of thread: [0] shadow pass, [1] scenery thread.
volatile LONG g_seedReads[2][2] = {};
thread_local unsigned t_cullWritten = 0, t_lightWritten = 0;  // the seed tag (t_flagsSeeded) the flag was last written under
// The scenery thread's flags when its job ends, and the objects they belong to. In the engine's
// own order the modules after the scenery see the values the scenery left (its last flush sets
// the light flag to 1). With private copies those writes stayed on the scenery thread, and the
// shared light flag stayed at 0 from the game thread's pass start: every shadow pass and scenery
// job of the next frames started from 0 (1,200 and 600 of 600, run lightflag-four). The game thread
// puts the scenery's final values into the shared flags when it has passed the scenery's place.
void* volatile g_cullObject = nullptr;
volatile LONG g_forkFinal = -1;  // -1: none; else bit 0 cull flag, bit 1 light flag
void CullFlagSet(void* self, uint8_t v) {
    g_cullObject = self;
    if (OwnFlags(self)) t_cullFlag = v, t_cullWritten = t_flagsSeeded;
    else g_cullFlagSet(self, v);
}
uint8_t CullFlagGet(void* self) {
    if (!OwnFlags(self)) return g_cullFlagGet(self);
    if (t_cullWritten != t_flagsSeeded) InterlockedIncrement(&g_seedReads[0][overlapstate::ShadowRole() ? 0 : 1]);
    return t_cullFlag;
}
void LightFlagSet(void* self, uint8_t v) {
    g_lightManager = self;
    if (OwnFlags(self)) t_lightFlag = v, t_lightWritten = t_flagsSeeded;
    else g_lightFlagSet(self, v);
}
uint8_t LightFlagGet(void* self) {
    if (!OwnFlags(self)) return g_lightFlagGet(self);
    if (t_lightWritten != t_flagsSeeded) InterlockedIncrement(&g_seedReads[1][overlapstate::ShadowRole() ? 0 : 1]);
    return t_lightFlag;
}
}  // namespace
// Scenery thread, at the end of its job (still with its private flags).
void ForkKeepFinalFlags() {
    if (!g_forkPrivateFlags || t_flagsSeeded != (0x80000000u | t_forkJob)) return;  // the job did not use the flags
    InterlockedExchange(&g_forkFinal, (t_cullFlag ? 1 : 0) | (t_lightFlag ? 2 : 0));
}
// Game thread, after the scenery thread's job (overlap::ForkHooks::joined).
void ForkJoined() {
    if (g_sceneryLeftovers && g_forkAfterValid) {
        uint64_t now[kBinderWords];
        memcpy(now, binder::Global(), sizeof now);
        CountDiff(g_forkAfter, now, g_wordsJoin);
        g_forkAfterValid = false;
        static LONG joins = 0;
        if (InterlockedIncrement(&joins) % 200 == 1)
            Log("scenery leftovers: values, scenery thread start -> end | game thread at the join: +e0 %llx -> %llx | %llx; +c8 %llx -> %llx | %llx; "
                "+740 %llx -> %llx | %llx; +758 %llx | %llx; +770 %llx | %llx; +4c0 %llx -> %llx | %llx; +1980 %llx -> %llx | %llx",
                g_forkBefore[0xe0 / 8], g_forkAfter[0xe0 / 8], now[0xe0 / 8], g_forkBefore[0xc8 / 8], g_forkAfter[0xc8 / 8], now[0xc8 / 8],
                g_forkBefore[0x740 / 8], g_forkAfter[0x740 / 8], now[0x740 / 8], g_forkAfter[0x758 / 8], now[0x758 / 8], g_forkAfter[0x770 / 8],
                now[0x770 / 8], g_forkBefore[0x4c0 / 8], g_forkAfter[0x4c0 / 8], now[0x4c0 / 8], g_forkBefore[0x1980 / 8], g_forkAfter[0x1980 / 8],
                now[0x1980 / 8]);
    }
    LONG final = InterlockedExchange(&g_forkFinal, -1);
    if (final < 0) return;
    if (void* cull = g_cullObject) g_cullFlagSet(cull, static_cast<uint8_t>(final & 1));
    if (void* light = g_lightManager) g_lightFlagSet(light, static_cast<uint8_t>((final >> 1) & 1));
}
void LogSeedReads() {
    LONG a = InterlockedExchange(&g_seedReads[0][0], 0), b = InterlockedExchange(&g_seedReads[0][1], 0), c = InterlockedExchange(&g_seedReads[1][0], 0),
         d = InterlockedExchange(&g_seedReads[1][1], 0);
    {
        char line[700];
        int n = 0;
        for (ModuleStart& m : g_moduleStart)
            if (m.name[0] && n < 620)
                n += sprintf_s(line + n, sizeof line - n, " %s 0:%ld 1:%ld;", m.name, InterlockedExchange(&m.zero, 0), InterlockedExchange(&m.one, 0));
        if (n) Log("research overlap: shared light flag when the game thread starts a render module (value: times), last 600 passes:%s", line);
    }
    Log("research overlap: skeleton palettes the shadow pass asked for, last 600 passes: %ld not made before (counted), %ld recorded, %ld made after the join, %ld dropped (list full)",
        InterlockedExchange(&g_paletteFirst, 0), InterlockedExchange(&g_paletteDeferred, 0), InterlockedExchange(&g_paletteReplayed, 0),
        InterlockedExchange(&g_paletteDropped, 0));
    LONG z0 = InterlockedExchange(&g_seedWasZero[0], 0), z1 = InterlockedExchange(&g_seedWasZero[1], 0);
    if (z0 || z1)
        Log("research overlap: the shared light flag was 0 when a pass took its start value, last 600 passes: %ld shadow passes, %ld scenery "
            "jobs%s", z0, z1, g_lightFlagStartOne ? " (the passes started at 1)" : "");
    if (a || b || c || d)
        Log("research overlap: walk flags read before the pass wrote them (the seeded value was used), last 600 passes: tile-in-frustum flag "
            "%ld on shadow threads and %ld on the scenery thread; light flag %ld and %ld", a, b, c, d);
}
namespace {

// Sorted queue (cHcSGOffsetRenderer): 512 buckets at +0xe8 (16 bytes: head, tail) of circular
// lists through a sentinel at bucket - 0x30 (element +0x30 next).
constexpr size_t kBuckets = 0xe8, kBucketCount = 512;
inline uint8_t*& Next(uint8_t* e) { return *reinterpret_cast<uint8_t**>(e + 0x30); }
constexpr size_t kElemSlotSnapshot = 0x10, kElemSlotSnapshotBytes = 0x20;
void ClearSlotSnapshots(void* q) {
    auto* t = static_cast<uint8_t*>(q);
    for (size_t b = 0; b < kBucketCount; ++b) {
        uint8_t* sentinel = t + kBuckets + b * 16 - 0x30;
        for (uint8_t* e = Next(sentinel); e && e != sentinel; e = Next(e))
            memset(e + kElemSlotSnapshot, 0, kElemSlotSnapshotBytes);
    }
}
// The shadow pass's private walk flags (see OwnFlags).
bool InstallShadowFlags() {
    return Hook(kCullFlagSet, reinterpret_cast<void*>(&CullFlagSet), reinterpret_cast<void**>(&g_cullFlagSet)) &&
           Hook(kCullFlagGet, reinterpret_cast<void*>(&CullFlagGet), reinterpret_cast<void**>(&g_cullFlagGet)) &&
           Hook(kLightFlagSet, reinterpret_cast<void*>(&LightFlagSet), reinterpret_cast<void**>(&g_lightFlagSet)) &&
           Hook(kLightFlagGet, reinterpret_cast<void*>(&LightFlagGet), reinterpret_cast<void**>(&g_lightFlagGet));
}

}  // namespace

// Lock call sites whose use of the locked data is known (see d3d::AddVerifiedLockSite).
void RegisterLockSites() {
    // Geometry stream / index lock helpers: generic. The instanced batch build also locks the
    // model's buffers to read them (not registered); its fills of the batch buffers it has just
    // created (stream 0, indices, stream 1) are write-only.
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kLockWrapperRet0, d3d::LockSiteKind::LockWrapper);
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kLockWrapperRet1, d3d::LockSiteKind::LockWrapper);
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kBatchFillRet0, d3d::LockSiteKind::WriteOnlyViaWrapper);
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kBatchFillRet1, d3d::LockSiteKind::WriteOnlyViaWrapper);
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kBatchFillRet2, d3d::LockSiteKind::WriteOnlyViaWrapper);
    // Sky middleware's vertex update: write-only to the range it locks.
    constexpr sites::Fingerprint kSkyVertexUpdate = {0x7e908890cd06f6a8ull, 16};
    d3d::AddVerifiedLockSite("SilverLiningDirectX9-MT-DLL.dll", kSkyVertexUpdate, 0x4d, d3d::LockSiteKind::WriteOnlyRange);
    // Rain on the windscreen (cWeatherGlassRenderer): its own dynamic texture, read and written
    // by the CPU each frame.
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kShadowTexLockRet0, d3d::LockSiteKind::CpuOnlyTexture);
    d3d::AddVerifiedLockSite("GameManagerVC64.dll", kShadowTexLockRet1, d3d::LockSiteKind::CpuOnlyTexture);
    // GetRenderState callers that save a state, set their own, draw and restore the saved value
    // (cull mode / z-enable / z-write), never inspecting it.
    // Particles (cParticleManagerSingleton::vf12): saves six states, restores them at the end.
    for (uintptr_t ret : kInlineRets)
        rt::AddSaveRestoreSite("GameManagerVC64.dll", ret);
}

// Overlap off: the engine runs its own code (hooks and binder patches removed); on: back in.
bool SetEngineHooks(bool on) {
    static bool enabled = true;
    if (on == enabled) return true;
    if (!binder::SetPatched(on)) return false;
    for (int i = 0; i < g_hookTargetCount; ++i)
        on ? MH_QueueEnableHook(g_hookTargets[i]) : MH_QueueDisableHook(g_hookTargets[i]);
    MH_ApplyQueued();
    enabled = on;
    return true;
}

// Fills the engine locations from engine_sites.h (fingerprints, offsets, RTTI names). False if
// any is missing: then nothing is hooked (another game version).
bool ResolveEngineSites(HMODULE engine) {
    size_t n = 0;
    const uintptr_t* got = engine::Resolved(engine, &n);
    for (size_t i = 0; i < engine::kSiteCount; ++i)
        if (!got[i]) Log("research: engine site %zu not found", i);
    Log("research: engine sites resolved %zu of %d", n, int(engine::kSiteCount));
    if (n != engine::kSiteCount) return false;
    kTileWalk = got[engine::TileWalk];
    kTileDraw = got[engine::TileDraw];
    kLodOvSet = got[engine::LodOverrideSet], kLodOvGet = got[engine::LodOverrideGet];
    kCullInvGet = got[engine::CullInvGet], kCullInvSet = got[engine::CullInvSet];
    kRsmClear = got[engine::RsmClear];
    kRsmAdd = got[engine::RsmAdd];
    kRsmDraw = got[engine::RsmDraw];
    kNetworkRender = got[engine::NetworkRender];
    kTerrainDraw = got[engine::TerrainDraw];
    kTerrainPatches = got[engine::TerrainPatches];
    kInstEntry = got[engine::InstEntry];
    kInstAdd = got[engine::InstAdd];
    kInstFlush = got[engine::InstFlush];
    kInstDraw = got[engine::InstDraw];
    kInstBuild = got[engine::InstBuild];
    kBinderReset = got[engine::BinderReset];
    kBinderGlobal = got[engine::BinderGlobal];
    kSelectTechnique = got[engine::SelectTechnique];
    kMaterialDtor = got[engine::MaterialDtor];
    kMaterialDraw = got[engine::MaterialDraw];
    kDepthModifier = got[engine::DepthModifier];
    kModifierGet = got[engine::ModifierGet], kModifierClear = got[engine::ModifierClear], kModifierSet = got[engine::ModifierSet];
    kDepthModifierRestore = got[engine::DepthModifierRestore];
    kOptionsGet = got[engine::OptionsGet];
    kProfileGet = got[engine::ProfileGet];
    kRecordGet = got[engine::RecordGet];
    kRecordAdd = got[engine::RecordAdd];
    kRecordRemove = got[engine::RecordRemove];
    kLightCreate = got[engine::LightCreate];
    kLightDestroy = got[engine::LightDestroy];
    kLightClear = got[engine::LightClear];
    kLightWalk = got[engine::LightWalk];
    kQueueCtor = got[engine::QueueCtor];
    kQueueInsert = got[engine::QueueInsert];
    kQueueBegin = got[engine::QueueBegin];
    kQueueFlush = got[engine::QueueFlush];
    kQueueReset = got[engine::QueueReset];
    kEngineGet = got[engine::EngineGet];
    kAllocator = got[engine::Allocator];
    kInShadowPass = got[engine::InShadowPass];
    kEngineModeGet = got[engine::EngineModeGet];
    kEngineModeSet = got[engine::EngineModeSet];
    kMaterialOverrides = got[engine::MaterialOverrides];
    kDayNightFlags = got[engine::DayNightFlags];
    kSkeletonPalette = got[engine::SkeletonPalette];
    kNodeVisibility = got[engine::NodeVisibility];
    kAnimationEvaluate = got[engine::AnimationEvaluate];
    kEntityAddRef = got[engine::EntityAddRef];
    kEntityRelease = got[engine::EntityRelease];
    kTenaciousAddRef = got[engine::TenaciousAddRef];
    kTenaciousRelease = got[engine::TenaciousRelease];
    kCullFlagSet = got[engine::CullFlagSet];
    kCullFlagGet = got[engine::CullFlagGet];
    kLightFlagSet = got[engine::LightFlagSet];
    kLightFlagGet = got[engine::LightFlagGet];
    kQueueGet[0] = got[engine::QueueGet0];
    kQueueGet[1] = got[engine::QueueGet1];
    kQueueGet[2] = got[engine::QueueGet2];
    kLockWrapperRet0 = got[engine::LockWrapperRet0];
    kLockWrapperRet1 = got[engine::LockWrapperRet1];
    kBatchFillRet0 = got[engine::BatchFillRet0];
    kBatchFillRet1 = got[engine::BatchFillRet1];
    kBatchFillRet2 = got[engine::BatchFillRet2];
    kShadowTexLockRet0 = got[engine::ShadowTexLockRet0];
    kShadowTexLockRet1 = got[engine::ShadowTexLockRet1];
    kInlineRets[0] = got[engine::InlineRet0];
    kInlineRets[1] = got[engine::InlineRet1];
    kInlineRets[2] = got[engine::InlineRet2];
    kInlineRets[3] = got[engine::InlineRet3];
    kInlineRets[4] = got[engine::InlineRet4];
    kInlineRets[5] = got[engine::InlineRet5];
    kInlineRets[6] = got[engine::InlineRet6];
    kInlineRets[7] = got[engine::InlineRet7];
    kInlineRets[8] = got[engine::InlineRet8];
    kInlineRets[9] = got[engine::InlineRet9];
    kInlineRets[10] = got[engine::InlineRet10];
    kInlineRets[11] = got[engine::InlineRet11];
    kInlineRets[12] = got[engine::InlineRet12];
    kInlineRets[13] = got[engine::InlineRet13];
    kInlineRets[14] = got[engine::InlineRet14];
    kInlineRets[15] = got[engine::InlineRet15];
    return true;
}

bool Install(HMODULE engine) {
    if (!ResolveEngineSites(engine)) return false;
    RegisterLockSites();
    // Hooks and binder patches stay in: idle they cost no measurable time (open line: binder
    // only 55.5, hooks only 54.7, stock 53.9-58.3 fps), and switching them stutters ~100 ms.
    (void)&SetEngineHooks;
    tscfix::overlap::SetForkHooks({&ForkPrepare, &ForkEnter, &ForkLeave, &ForkCloseCaller, &ForkJoined, &ForkSlot});
    tscfix::overlap::SetShadowJoinedHook(&ReplayPalettes);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(engine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(engine) + dos->e_lfanew);
    (void)nt;
    g_base = reinterpret_cast<uintptr_t>(engine);
    InitializeCriticalSection(&g_serviceLock);
    InitializeCriticalSection(&g_serviceWaitedLock);
    InitializeCriticalSection(&g_queueLock);
    InitializeCriticalSection(&g_firstUseLock);
    InitializeCriticalSection(&g_networkLock);
    InitializeCriticalSection(&g_terrainLock);
    for (auto& lock : g_overrideLock) InitializeCriticalSection(&lock);
    InitializeCriticalSection(&g_mainFlushLock);
    InitializeCriticalSection(&g_zSortLock);
    MH_Initialize();
    bool ok = Hook(kTileWalk, reinterpret_cast<void*>(&TileWalk), reinterpret_cast<void**>(&g_tileWalk));
    if (glitch::Enabled()) Hook(kTileDraw, reinterpret_cast<void*>(&TileDraw), reinterpret_cast<void**>(&g_tileDraw));
    glitch::RegisterSwitch("passflag", [](bool on) { g_swPassFlag = on; });
    glitch::RegisterSwitch("shadowflags", [](bool on) { g_swShadowFlags = on; });
    glitch::RegisterSwitch("tilelock", [](bool on) { g_swTileLock = on; });
    glitch::RegisterSwitch("tilerefresh", [](bool on) { g_swTileRefresh = on; });
    glitch::RegisterSwitch("lodov", [](bool on) { g_swLodOv = on; });
    glitch::RegisterSwitch("cullinv", [](bool on) { g_swCullInv = on; });
    overlap::SetBeforeShadowHook(&RefreshTileBounds);
    {
        int a = kLodOvSet ? RedirectVtableEntries(g_base + kLodOvSet,
                                                                    reinterpret_cast<void*>(&LodOvSet)) : 0;
        int b = kLodOvGet ? RedirectVtableEntries(g_base + kLodOvGet,
                                                                    reinterpret_cast<void*>(&LodOvGet)) : 0;
        Log("research overlap: LOD override accessors redirected (%d / %d vtable entries)", a, b);
        int c = kCullInvGet ? RedirectVtableEntries(g_base + kCullInvGet, reinterpret_cast<void*>(&CullInvGet)) : 0;
        int d = kCullInvSet ? RedirectVtableEntries(g_base + kCullInvSet, reinterpret_cast<void*>(&CullInvSet)) : 0;
        Log("research overlap: cull-inversion accessors redirected (%d / %d vtable entries)", c, d);
    }
    glitch::RegisterSwitch("instlock", [](bool on) { g_swInstLock = on; });
    glitch::RegisterSwitch("shadowsnap", [](bool on) { g_shadowClearSnapshots = on; });
    glitch::RegisterSwitch("forcetex", [](bool on) { g_swForceTex = on; });
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_swShadowFlags = tscfix::config::Int("experiments", "shadow_private_flags", 1, ini) != 0;
    }
    ok &= Hook(kRsmClear, reinterpret_cast<void*>(&RsmClear), reinterpret_cast<void**>(&g_rsmClear));
    ok &= Hook(kRsmAdd, reinterpret_cast<void*>(&RsmAdd), reinterpret_cast<void**>(&g_rsmAdd));
    ok &= Hook(kRsmDraw, reinterpret_cast<void*>(&RsmDraw), reinterpret_cast<void**>(&g_rsmDraw));
    ok &= Hook(kInstEntry, reinterpret_cast<void*>(&InstEntry), reinterpret_cast<void**>(&g_instEntry));
    ok &= Hook(kInstAdd, reinterpret_cast<void*>(&InstAdd), reinterpret_cast<void**>(&g_instAdd));
    ok &= Hook(kInstFlush, reinterpret_cast<void*>(&InstFlush), reinterpret_cast<void**>(&g_instFlush));
    ok &= Hook(kInstBuild, reinterpret_cast<void*>(&InstBuild), reinterpret_cast<void**>(&g_instBuild));
    {
        void* a = sites::Find(engine, kOverrideDrawA);
        void* b = sites::Find(engine, kOverrideDrawB);
        if (a && b) {
            ok &= Hook(reinterpret_cast<uintptr_t>(a) - g_base, reinterpret_cast<void*>(&OverrideDraw<0>),
                       reinterpret_cast<void**>(&g_overrideDraw[0]));
            ok &= Hook(reinterpret_cast<uintptr_t>(b) - g_base, reinterpret_cast<void*>(&OverrideDraw<1>),
                       reinterpret_cast<void**>(&g_overrideDraw[1]));
        } else {
            Log("research overlap: draws with material overrides not found; they are not kept apart between threads");
        }
    }
    if (void* insert = sites::Find(engine, kZSortInsert))
        ok &= Hook(reinterpret_cast<uintptr_t>(insert) - g_base, reinterpret_cast<void*>(&ZSortInsert),
                   reinterpret_cast<void**>(&g_zSortInsert));
    else
        Log("research overlap: z-sorted queue insert not found; it is not locked between threads");
    if (void* refill = sites::Find(engine, kInstRefill))
        ok &= Hook(reinterpret_cast<uintptr_t>(refill) - g_base, reinterpret_cast<void*>(&InstRefill),
                   reinterpret_cast<void**>(&g_instRefill));
    else
        Log("research overlap: instance list refill not found; it is not serialized between threads");
    ok &= Hook(kSelectTechnique, reinterpret_cast<void*>(&SelectTechnique), reinterpret_cast<void**>(&g_selectTechnique));
    ok &= Hook(kEngineModeGet, reinterpret_cast<void*>(&ModeGet), reinterpret_cast<void**>(&g_modeGet));
    ok &= Hook(kEngineModeSet, reinterpret_cast<void*>(&ModeSet), reinterpret_cast<void**>(&g_modeSet));
    ok &= Hook(kMaterialOverrides, reinterpret_cast<void*>(&MaterialOverrides),
               reinterpret_cast<void**>(&g_materialOverrides));
    ok &= Hook(kDayNightFlags, reinterpret_cast<void*>(&DayNightFlags), reinterpret_cast<void**>(&g_dayNightFlags));
    ok &= Hook(kAnimationEvaluate, reinterpret_cast<void*>(&AnimationEvaluate),
               reinterpret_cast<void**>(&g_animationEvaluate));
    ok &= Hook(kNodeVisibility, reinterpret_cast<void*>(&NodeVisibility), reinterpret_cast<void**>(&g_nodeVisibility));
    ok &= Hook(kSkeletonPalette, reinterpret_cast<void*>(&SkeletonPalette),
               reinterpret_cast<void**>(&g_skeletonPalette));
    ok &= Hook(kTerrainDraw, reinterpret_cast<void*>(&TerrainDraw), reinterpret_cast<void**>(&g_terrainDraw));
    ok &= Hook(kTerrainPatches, reinterpret_cast<void*>(&TerrainPatches), reinterpret_cast<void**>(&g_terrainPatches));
    {
        char flagsIni[MAX_PATH];
        GetModuleFileNameA(nullptr, flagsIni, MAX_PATH);
        if (char* slash = strrchr(flagsIni, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - flagsIni), "tscfix.ini");
        g_shadowClearSnapshots = tscfix::config::Int("experiments", "shadow_clear_slot_snapshots", 1, flagsIni) != 0;
        g_swForceTex = tscfix::config::Int("experiments", "force_material_textures", 1, flagsIni) != 0;
        g_swLodOv = tscfix::config::Int("experiments", "shadow_private_lod", 1, flagsIni) != 0;
        g_signBias = -tscfix::config::Int("graphics", "sign_text_depth_bias", 32, flagsIni) / 16777216.0f;
        g_signSlope = -tscfix::config::Int("graphics", "sign_text_slope_bias", 100, flagsIni) / 100.0f;
        g_signToggleKey = tscfix::config::Int("graphics", "sign_text_toggle_key", 0, flagsIni);
        if (g_signBias != 0 || g_signSlope != 0)
            Log("pipeline: sign text depth bias %g, slope %g", g_signBias, g_signSlope);
    }
    ok &= Hook(kNetworkRender, reinterpret_cast<void*>(&NetworkRender), reinterpret_cast<void**>(&g_networkRender));
    ok &= Hook(kEntityAddRef, reinterpret_cast<void*>(&AtomicAddRef<&g_entityAddRef>),
               reinterpret_cast<void**>(&g_entityAddRef));
    ok &= Hook(kEntityRelease, reinterpret_cast<void*>(&AtomicRelease<&g_entityRelease>),
               reinterpret_cast<void**>(&g_entityRelease));
    ok &= Hook(kTenaciousAddRef, reinterpret_cast<void*>(&AtomicAddRef<&g_tenaciousAddRef>),
               reinterpret_cast<void**>(&g_tenaciousAddRef));
    ok &= Hook(kTenaciousRelease, reinterpret_cast<void*>(&AtomicRelease<&g_tenaciousRelease>),
               reinterpret_cast<void**>(&g_tenaciousRelease));
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
    g_skipDepthModifier = tscfix::config::Int("experiments", "skip_depth_modifier", 0, ini) != 0;
    g_forkOwnInstances = tscfix::config::Int("experiments", "fork_own_instances", 1, ini) != 0;
    g_holdFlashMs = tscfix::config::Int("experiments", "hold_flash_ms", 0, ini);
    if (tscfix::config::Int("experiments", "pass_flag_private", 0, ini)) g_swPassFlag = true;
    g_zSortShadowDirect = tscfix::config::Int("experiments", "zsort_shadow_direct", 1, ini) != 0;
    g_overrideLockAll = tscfix::config::Int("fixes", "override_lock_all", 1, ini) != 0 &&
                        tscfix::config::Int("experiments", "override_lock_all", 1, ini) != 0;  // (test runs switch it off here)
    g_forkPrivateFlags = tscfix::config::Int("experiments", "fork_private_flags", 1, ini) != 0;
    if (tscfix::config::Int("experiments", "inst_clone_lock", 0, ini)) g_swInstLock = true;
    g_lightFlagStartOne = tscfix::config::Int("experiments", "light_flag_start_one", 1, ini) != 0;
    g_sceneryLeftovers = tscfix::config::Int("experiments", "scenery_leftovers", 0, ini) != 0;
    {
        char watch[96];
        tscfix::config::Str("experiments", "binder_watch", "", watch, sizeof watch, ini);
        for (char* p = watch; *p && g_watchCount < 4;) {
            char* end;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            if (v < 0x19f0) g_watchOffsets[g_watchCount++] = v;
            p = *end ? end + 1 : end;
        }
    }
    tscfix::overlap::SetModuleBeginHook([](const char* name) { NoteModuleStart(name), LeftoverModuleBegin(name); });
    tscfix::overlap::SetModuleEndHook(&LeftoverModuleEnd);
    g_modifierClones = tscfix::config::Int("experiments", "modifier_clones", 1, ini) != 0;
    g_lightDump = tscfix::config::Int("experiments", "light_dump", 0, ini);
    if (g_skipDepthModifier || g_modifierClones) {
        ok &= Hook(kDepthModifier, reinterpret_cast<void*>(&DepthModifier), reinterpret_cast<void**>(&g_depthModifier));
        ok &= Hook(kDepthModifierRestore, reinterpret_cast<void*>(&DepthModifierRestore),
                   reinterpret_cast<void**>(&g_depthModifierRestore));
    }
    if (g_modifierClones) {
        // Get is one of the binder's stubbed methods (its detour runs after the binder swap); the
        // other two are reached with the thread's binder already.
        bool reg = binder::RegisterMethodDetour(kModifierGet, reinterpret_cast<void*>(&ModifierGet),
                                                reinterpret_cast<void**>(&g_modifierGet));
        ok &= Hook(kModifierClear, reinterpret_cast<void*>(&ModifierClear), reinterpret_cast<void**>(&g_modifierClear));
        ok &= Hook(kModifierSet, reinterpret_cast<void*>(&ModifierSet), reinterpret_cast<void**>(&g_modifierSet));
        Log("research overlap: depth modifier clones per binder %s", reg ? "on" : "NOT registered");
    }
    ok &= Hook(kInShadowPass, reinterpret_cast<void*>(&InShadowPass), reinterpret_cast<void**>(&g_inShadowPass));
    ok &= Hook(kQueueGet[0], reinterpret_cast<void*>(&QueueGet0), reinterpret_cast<void**>(&g_queueGet[0]));
    ok &= Hook(kQueueGet[1], reinterpret_cast<void*>(&QueueGet1), reinterpret_cast<void**>(&g_queueGet[1]));
    ok &= Hook(kQueueGet[2], reinterpret_cast<void*>(&QueueGet2), reinterpret_cast<void**>(&g_queueGet[2]));
    ok &= Hook(kQueueInsert, reinterpret_cast<void*>(&QueueInsert), reinterpret_cast<void**>(&g_queueInsert));
    ok &= Hook(kQueueBegin, reinterpret_cast<void*>(&QueueBegin), reinterpret_cast<void**>(&g_queueBegin));
    ok &= Hook(kQueueFlush, reinterpret_cast<void*>(&QueueFlush), reinterpret_cast<void**>(&g_queueFlush));
    ok &= Hook(kMaterialDraw, reinterpret_cast<void*>(&MaterialDraw), reinterpret_cast<void**>(&g_materialDraw));
    if (void* numbers = sites::Find(engine, kNumberTextures))
        ok &= Hook(reinterpret_cast<uintptr_t>(numbers) - g_base, reinterpret_cast<void*>(&NumberTextures),
                   reinterpret_cast<void**>(&g_numberTextures));
    if (void* setup = sites::Find(engine, kMaterialSetup))
        ok &= Hook(reinterpret_cast<uintptr_t>(setup) - g_base, reinterpret_cast<void*>(&MaterialSetup),
                   reinterpret_cast<void**>(&g_materialSetup));
    else
        Log("research overlap: material setup not found; first uses are only serialized when drawing");
    ok &= Hook(kQueueReset, reinterpret_cast<void*>(&QueueReset), reinterpret_cast<void**>(&g_queueReset));
    ok &= Hook(kOptionsGet, reinterpret_cast<void*>(&OptionsGet), reinterpret_cast<void**>(&g_optionsGet));
    ok &= Hook(kProfileGet, reinterpret_cast<void*>(&ProfileGet), reinterpret_cast<void**>(&g_profileGet));
    ok &= Hook(kRecordGet, reinterpret_cast<void*>(&RecordGet), reinterpret_cast<void**>(&g_recordGet));
    ok &= Hook(kRecordAdd, reinterpret_cast<void*>(&RecordAdd), reinterpret_cast<void**>(&g_recordAdd));
    ok &= Hook(kRecordRemove, reinterpret_cast<void*>(&RecordRemove), reinterpret_cast<void**>(&g_recordRemove));
    ok &= Hook(kLightCreate, reinterpret_cast<void*>(&LightCreate), reinterpret_cast<void**>(&g_lightCreate));
    ok &= Hook(kLightDestroy, reinterpret_cast<void*>(&LightDestroy), reinterpret_cast<void**>(&g_lightDestroy));
    ok &= Hook(kLightClear, reinterpret_cast<void*>(&LightClear), reinterpret_cast<void**>(&g_lightClear));
    ok &= Hook(kLightWalk, reinterpret_cast<void*>(&LightWalk), reinterpret_cast<void**>(&g_lightWalk));
    const uintptr_t* sitesGot = engine::Resolved(reinterpret_cast<HMODULE>(g_base));
    kCreatePointLight = sitesGot[engine::CreatePointLight];
    if (kCreatePointLight)
        ok &= Hook(kCreatePointLight, reinterpret_cast<void*>(&CreatePointLight),
                   reinterpret_cast<void**>(&g_createPointLight));
    ok &= InstallShadowFlags();
    g_instDraw = reinterpret_cast<InstDrawFn>(g_base + kInstDraw);
    g_binderReset = reinterpret_cast<BinderResetFn>(g_base + kBinderReset);
    Log("research overlap: per-thread clones %s (atomic entity reference counts, scenery tile walk and its flags, render-state managers, instancing, service locks, sorted queues, shadow-pass flag, engine shadow mode, worker read-only instance state, network and terrain locks)",
        ok ? "installed" : "incomplete");
    return ok;
}

void AddPresets() {
    // Render modules whose update runs beside the frame (each has its own guard in this file).
    config::Set("experiments", "overlap_modules",
                ".?AVcDepthPass@cSilverLiningWeather@@,.?AVcTerrainManager@@,.?AVcPlatformManager@@,"
                ".?AVcTrafficManager@@,.?AVcNetworkRenderer@Network@@,.?AVcConsistManager@@,.?AVcSceneryManager@@");
}

}  // namespace tscfix::pipeline
