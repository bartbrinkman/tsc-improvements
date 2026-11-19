// [experiments] lod_probe=1 (measurement only): what the engine's level-of-detail selector does on
// the threads that draw the main picture.
//
// The selector (a node with one child per level) reads its inputs from the
// draw binder: a forced level (+0x128, -1 = choose by distance), a level bias (+0x144), a distance
// scale (+0x148) and a "distance given" flag (+0x121). It stores the level it chose in the node
// (+0x44) when the thread is not in the shadow pass. Each drawing thread has its own binder, so
// the main picture's inputs must stay at their normal values and, while the train stands, a
// node's level must not change.
//
// The probe logs, for threads without the shadow role: calls with a forced level or a bias, and
// nodes whose stored level changes from one call to the next, with the inputs of that call.
#include "lodprobe.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "binder.h"
#include "engine_sites.h"
#include "log.h"
#include "overlap.h"
#include "overlapstate.h"
#include "sites.h"

namespace tscfix::lodprobe {
namespace {

constexpr sites::Fingerprint kLodSelect = {0xebf959af1193fc7dull, 24};
// (The walk may pass more than the node: the other register arguments go through unchanged.)
using SelectFn = void (*)(void* node, void* a2, void* a3, void* a4);
SelectFn g_select = nullptr;

std::atomic<uint64_t> g_calls[3], g_forced[3], g_biased[3], g_changed[3], g_scaleOdd[3];
std::atomic<uint32_t> g_scaleUsual{0};  // the distance scale the game thread uses (float bits), learned from its first calls
std::atomic<int> g_lines{0};

// lod_probe=2: level changes per instance (stage 1 of the LOD fade). A node is shared by all
// instances of a model, so an instance is the node plus the place it is drawn at (the translation
// of the node's matrix for this draw, to 0.5 m). A change is the same instance drawn at another
// level than at its last draw, not more than 2 s ago.
int g_mode = 1;
struct InstKey {
    void* node;
    int32_t x, y, z;
    bool operator==(const InstKey& o) const { return node == o.node && x == o.x && y == o.y && z == o.z; }
};
struct InstKeyHash {
    size_t operator()(const InstKey& k) const {
        uint64_t h = reinterpret_cast<uintptr_t>(k.node) * 0x9e3779b97f4a7c15ull;
        h ^= static_cast<uint32_t>(k.x) * 0x85ebca6bull + static_cast<uint32_t>(k.y) * 0xc2b2ae35ull + static_cast<uint32_t>(k.z) * 0x27d4eb2full;
        return static_cast<size_t>(h ^ (h >> 31));
    }
};
struct InstState {
    int32_t level;
    double seen;         // ms
    int32_t levelBefore; // the level it had before its latest change
    double changedAt;    // ms of that change
};
// An excursion: an instance goes from level A to B and back to A within 70 ms (two to four frames).
// A real change of distance does not do that; a wrong input for one frame does.
std::atomic<uint64_t> g_excursions{0};
std::atomic<int> g_excursionLines{0};
double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}
thread_local std::unordered_map<InstKey, InstState, InstKeyHash>* t_instances = nullptr;
std::atomic<uint64_t> g_instDraws{0}, g_instSeenAgain{0}, g_toFiner{0}, g_toCoarser{0}, g_appeared{0}, g_vanishedLevel{0};
std::atomic<uint64_t> g_tracked{0};
void NoteInstance(void* node, const uint8_t* ctx, int32_t level, int32_t forced, int32_t bias, float scale) {
    const uint8_t* n = static_cast<const uint8_t*>(node);
    int32_t index;
    memcpy(&index, n + 0x40, 4);
    const float* palette = *reinterpret_cast<const float* const*>(ctx + 0x10);
    const float* world = *reinterpret_cast<const float* const*>(ctx + 0x20);
    const float* m = index == -1 ? world : ctx[2] ? palette : palette + static_cast<ptrdiff_t>(index) * 16;
    if (!m) return;
    InstKey key{node, static_cast<int32_t>(m[12] * 2.0f), static_cast<int32_t>(m[13] * 2.0f), static_cast<int32_t>(m[14] * 2.0f)};
    if (!t_instances) t_instances = new std::unordered_map<InstKey, InstState, InstKeyHash>();
    double now = NowMs();
    g_instDraws.fetch_add(1, std::memory_order_relaxed);
    auto it = t_instances->find(key);
    if (it == t_instances->end()) {
        if (t_instances->size() > 400000) t_instances->clear();  // (the place moved with the world origin: start again)
        (*t_instances)[key] = {level, now, level, -1e9};
        g_tracked.store(t_instances->size(), std::memory_order_relaxed);
        return;
    }
    if (now - it->second.seen <= 2000) {
        g_instSeenAgain.fetch_add(1, std::memory_order_relaxed);
        if (level != it->second.level) {
            if (level == -1) g_vanishedLevel.fetch_add(1, std::memory_order_relaxed);
            else if (it->second.level == -1) g_appeared.fetch_add(1, std::memory_order_relaxed);
            else (level < it->second.level ? g_toFiner : g_toCoarser).fetch_add(1, std::memory_order_relaxed);
            if (level == it->second.levelBefore && now - it->second.changedAt < 70.0) {
                g_excursions.fetch_add(1, std::memory_order_relaxed);
                if (g_excursionLines.fetch_add(1) < 300)
                    Log("lod probe: excursion: node %p at (%.1f, %.1f, %.1f): level %d for %.0f ms, then back to %d; this call: forced %d, bias %d, scale %.3f; "
                        "%s thread, overlap %d",
                        node, m[12], m[13], m[14], it->second.level, now - it->second.changedAt, level, forced, bias, scale,
                        overlap::OnForkWorker() ? "scenery" : "game", overlap::Overlapping() ? 1 : 0);
            }
            it->second.levelBefore = it->second.level, it->second.changedAt = now;
        }
    }
    it->second.level = level, it->second.seen = now;
}

int Role() { return overlapstate::ShadowRole() ? 2 : overlap::OnForkWorker() ? 1 : 0; }

void Select(void* node, void* a2, void* a3, void* a4) {
    int role = Role();
    if (role == 2) return g_select(node, a2, a3, a4);  // shadow pass: other inputs by design, and it stores no level
    auto* n = static_cast<uint8_t*>(node);
    // The selector reads through a pointer the binder holds at +0x20 (an engine global).
    const uint8_t* holder = binder::Binder();
    const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(holder + 0x20);
    if (!b) return g_select(node, a2, a3, a4);
    {
        static std::atomic<int> told[3];
        if (told[role].fetch_add(1) == 0)
            Log("lod probe: %s thread: binder %p, the object with the LOD inputs %p (%s the binder)", role ? "scenery" : "game", holder, b,
                b >= holder && b < holder + 0x19f0 ? "inside" : "OUTSIDE");
    }
    int32_t forced, bias, before, after, levels = 0;
    uint32_t scale;
    memcpy(&forced, b + 0x128, 4);
    memcpy(&bias, b + 0x144, 4);
    memcpy(&scale, b + 0x148, 4);
    uint8_t given = b[0x121];
    memcpy(&before, n + 0x44, 4);
    g_select(node, a2, a3, a4);
    memcpy(&after, n + 0x44, 4);
    if (const int32_t* table = *reinterpret_cast<int32_t* const*>(n + 0x28)) levels = *table;
    if (g_mode == 2) {
        float s;
        memcpy(&s, &scale, 4);
        NoteInstance(node, b, after, forced, bias, s);
        return;
    }
    g_calls[role].fetch_add(1, std::memory_order_relaxed);
    uint32_t usual = g_scaleUsual.load(std::memory_order_relaxed);
    if (!usual && role == 0) g_scaleUsual.store(scale, std::memory_order_relaxed), usual = scale;
    bool oddScale = usual && scale != usual;
    bool changed = before != after && before != -1;
    if (forced != -1) g_forced[role].fetch_add(1, std::memory_order_relaxed);
    if (bias) g_biased[role].fetch_add(1, std::memory_order_relaxed);
    if (oddScale) g_scaleOdd[role].fetch_add(1, std::memory_order_relaxed);
    if (changed) g_changed[role].fetch_add(1, std::memory_order_relaxed);
    if ((forced != -1 || bias || changed) && g_lines.fetch_add(1) < 400) {
        float s;
        memcpy(&s, &scale, 4);
        Log("lod probe: %s thread, node %p (%d levels): level %d -> %d; forced %d, bias %d, scale %.3f, distance given %u, overlap %d",
            role ? "scenery" : "game", node, levels, before, after, forced, bias, s, given, overlap::Overlapping() ? 1 : 0);
    }
}

}  // namespace

bool Install(HMODULE engine, int mode) {
    g_mode = mode;
    // (The table of engine sites is resolved before the binder rewrites instructions; the selector's
    // first bytes hold one of those, so a search by fingerprint now would not find it.)
    uintptr_t rva = engine::Resolved(engine)[engine::LodSelectNode];
    void* fn = rva ? reinterpret_cast<uint8_t*>(engine) + rva : sites::Find(engine, kLodSelect);
    MH_Initialize();
    MH_STATUS created = fn ? MH_CreateHook(fn, reinterpret_cast<void*>(&Select), reinterpret_cast<void**>(&g_select)) : MH_UNKNOWN;
    MH_STATUS enabled = created == MH_OK ? MH_EnableHook(fn) : MH_UNKNOWN;
    bool ok = enabled == MH_OK;
    if (!ok) g_select = nullptr;
    Log("lod probe: %s (function %p, create %d, enable %d)", ok ? "on" : "NOT installed", fn, static_cast<int>(created),
        static_cast<int>(enabled));
    return ok;
}

void LogUse() {
    if (!g_select) return;
    if (g_mode == 2) {
        static ULONGLONG last = GetTickCount64();
        ULONGLONG now = GetTickCount64();
        double seconds = (now - last) / 1000.0;
        last = now;
        uint64_t draws = g_instDraws.exchange(0), again = g_instSeenAgain.exchange(0), finer = g_toFiner.exchange(0),
                 coarser = g_toCoarser.exchange(0), appeared = g_appeared.exchange(0), vanished = g_vanishedLevel.exchange(0);
        Log("lod probe: instances, last %.1f s: %llu selections, %llu of an instance seen in the last 2 s; level changes per second: "
            "%.1f to finer, %.1f to coarser, %.1f from none, %.1f to none; %llu instances followed; excursions (A, B, A within 70 ms) so far: %llu",
            seconds, static_cast<unsigned long long>(draws), static_cast<unsigned long long>(again), finer / seconds, coarser / seconds,
            appeared / seconds, vanished / seconds, static_cast<unsigned long long>(g_tracked.load()),
            static_cast<unsigned long long>(g_excursions.load()));
        return;
    }
    static const char* const kName[2] = {"game thread", "scenery thread"};
    for (int r = 0; r < 2; ++r) {
        uint64_t calls = g_calls[r].exchange(0);
        if (!calls) continue;
        Log("lod probe: %s, last 600 shadow passes: %llu selections, %llu with a forced level, %llu with a bias, %llu with another "
            "distance scale, %llu changed the node's level",
            kName[r], static_cast<unsigned long long>(calls), static_cast<unsigned long long>(g_forced[r].exchange(0)),
            static_cast<unsigned long long>(g_biased[r].exchange(0)), static_cast<unsigned long long>(g_scaleOdd[r].exchange(0)),
            static_cast<unsigned long long>(g_changed[r].exchange(0)));
    }
}

}  // namespace tscfix::lodprobe
