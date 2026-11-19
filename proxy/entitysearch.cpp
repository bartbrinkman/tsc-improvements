// [fixes] entity_search_cache: the entity-tree search behind script Call("child:f") / Call("*:f").
//
// The engine's search (root script, child name or null for "*", function, holder, handled flag)
// visits the tree depth first on every call: a node whose name matches is dispatched (the
// engine's per-node dispatch, kept as is), then, while not handled, the node is asked for its
// container interface and its two child vectors are visited, each child asked for its script
// interface. The returned value is the last visited subtree's (a quirk the replay keeps: after a
// handler in the first vector the second vector's first child is still visited and resets it).
//
// Here the tree is remembered per root (script pointers, interned name pointers, child vectors).
// Before each use the whole snapshot is checked by reading the same fields (under an exception
// guard). Any difference rebuilds it, before anything was dispatched. A handler can add or remove
// a child while a search runs: the two engine functions that do so are counted, and a replay that
// sees the count change stops there (it does not visit nodes of a tree that has changed).
//
// Checks: verify mode (the engine searches, its dispatches are recorded, the replay must agree):
// 21.2 million searches, 0 mismatches (run dispatch-verify, 14 Nov 2026). On/off in one drive:
// frame time -3.4% [-4.4, -2.1] (run dispatch-ab).
#include "entitysearch.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "engine_sites.h"
#include "sites.h"

namespace tscfix::entitysearch {
namespace {

constexpr sites::Fingerprint kScan = {0xa625e0483501d932ull, 24};
constexpr sites::Fingerprint kDispatchOne = {0x277ecab80d75215cull, 16};
constexpr size_t kName = 0xb0, kNameChars = 0x14, kVecA = 0x40, kVecB = 0x78, kElem = 0x10;

using ScanFn = uint32_t (*)(void* script, const char* child, const char* func, void** holder, char* handled);
using DispatchFn = uint32_t (*)(void* script, const char* func, void** holder, char* handled);
constexpr sites::Fingerprint kChildAdd = {0x4a987eedfe86e293ull, 32};
constexpr sites::Fingerprint kChildRemove = {0x04dd2c57928ff07cull, 16};
using ChildFn = void (*)(void* container, void* child, void* extra);
ChildFn g_childAdd = nullptr, g_childRemove = nullptr;
volatile LONG g_treeChanges = 0;  // children added to or removed from any container
volatile bool g_treeHooks = false;
void* g_scanAddress = nullptr;
void ChildAdd(void* container, void* child, void* extra) {
    InterlockedIncrement(&g_treeChanges);
    g_childAdd(container, child, extra);
}
void ChildRemove(void* container, void* child, void* extra) {
    InterlockedIncrement(&g_treeChanges);
    g_childRemove(container, child, extra);
}
ScanFn g_scan = nullptr;
DispatchFn g_dispatchOne = nullptr;
uint64_t g_scriptIid[2], g_containerIid[2];
int g_mode = 0;

void** VT(void* o) { return *static_cast<void***>(o); }
void AddRef(void* o) { reinterpret_cast<void (*)(void*)>(VT(o)[0])(o); }
void Release(void* o) { reinterpret_cast<void (*)(void*)>(VT(o)[1])(o); }
void* Query(void* o, const uint64_t* iid) { return reinterpret_cast<void* (*)(void*, const uint64_t*)>(VT(o)[5])(o, iid); }

// --- snapshot ---------------------------------------------------------------------------------
struct Node {
    void* script;
    void* vtable;
    void* name;       // interned name object (+0xb0)
    void* container;  // null: none
    void* vec[4];     // container: A begin/end, B begin/end
    std::vector<void*> childEntity[2];
    std::vector<int> child[2];  // node index per child element (-1: no script interface)
};
struct Tree {
    std::vector<Node> nodes;
    int inUse = 0;  // replays running on it (a handler can search the same root again)
};
thread_local std::unordered_map<void*, Tree>* t_trees = nullptr;  // per root script, per script thread

const char* NameOf(void* nameObj) {
    return nameObj ? static_cast<const char*>(nameObj) + kNameChars : "";
}
int Build(Tree& t, void* script, int depth) {
    if (depth > 64) return -1;
    int idx = static_cast<int>(t.nodes.size());
    t.nodes.push_back({});
    {
        Node& n = t.nodes[idx];
        n.script = script;
        n.vtable = VT(script);
        memcpy(&n.name, static_cast<uint8_t*>(script) + kName, 8);
        n.container = Query(script, g_containerIid);
    }
    void* container = t.nodes[idx].container;
    if (!container) return idx;
    auto* c = static_cast<uint8_t*>(container);
    void* vec[4];
    memcpy(&vec[0], c + kVecA, 8), memcpy(&vec[1], c + kVecA + 8, 8), memcpy(&vec[2], c + kVecB, 8), memcpy(&vec[3], c + kVecB + 8, 8);
    memcpy(t.nodes[idx].vec, vec, sizeof vec);
    for (int v = 0; v < 2; ++v)
        for (auto* e = static_cast<uint8_t*>(vec[2 * v]); e != vec[2 * v + 1]; e += kElem) {
            void* entity;
            memcpy(&entity, e, 8);
            void* childScript = entity ? Query(entity, g_scriptIid) : nullptr;
            int k = childScript ? Build(t, childScript, depth + 1) : -1;
            if (childScript && k < 0) return -1;
            t.nodes[idx].childEntity[v].push_back(entity);
            t.nodes[idx].child[v].push_back(k);
        }
    return idx;
}
bool ValidUnguarded(const Tree& t) {
    for (const Node& n : t.nodes) {
        void* name;
        memcpy(&name, static_cast<uint8_t*>(n.script) + kName, 8);
        if (VT(n.script) != n.vtable || name != n.name) return false;
        if (!n.container) continue;
        auto* c = static_cast<uint8_t*>(n.container);
        void* vec[4];
        memcpy(&vec[0], c + kVecA, 8), memcpy(&vec[1], c + kVecA + 8, 8), memcpy(&vec[2], c + kVecB, 8), memcpy(&vec[3], c + kVecB + 8, 8);
        if (memcmp(vec, n.vec, sizeof vec)) return false;
        for (int v = 0; v < 2; ++v) {
            auto* e = static_cast<uint8_t*>(vec[2 * v]);
            for (void* want : n.childEntity[v]) {
                void* entity;
                memcpy(&entity, e, 8);
                if (entity != want) return false;
                e += kElem;
            }
        }
    }
    return true;
}
bool Valid(const Tree& t) {
    __try {
        return ValidUnguarded(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint64_t g_builds = 0, g_uses = 0, g_mismatches = 0, g_verified = 0, g_stopped = 0, g_changedInSearch = 0;
Tree* TreeFor(void* root) {
    if (!t_trees) t_trees = new std::unordered_map<void*, Tree>();
    auto& trees = *t_trees;
    auto it = trees.find(root);
    if (it != trees.end() && Valid(it->second)) return &it->second;
    if (it != trees.end() && it->second.inUse) return nullptr;  // stale but busy: the engine searches
    Tree t;
    if (Build(t, root, 0) < 0) return nullptr;
    ++g_builds;
    if (trees.size() > 20000) {
        for (auto i = trees.begin(); i != trees.end();) i = i->second.inUse ? std::next(i) : trees.erase(i);
    }
    return &(trees[root] = std::move(t));
}

// --- replay -----------------------------------------------------------------------------------
// One per-node dispatch: the engine's (replace) or the next recorded one (verify).
struct Recorded {
    void* script;
    uint32_t result;
    char handledAfter;
};
thread_local std::vector<Recorded>* t_record = nullptr;  // engine search being recorded
thread_local const std::vector<Recorded>* t_replay = nullptr;
thread_local size_t t_replayPos = 0;
thread_local bool t_replayBad = false;

uint32_t DispatchOne(void* script, const char* func, void** holder, char* handled);
uint32_t DispatchNode(void* script, const char* func, void* holder, char* handled) {
    if (t_replay) {  // verify: the recorded dispatch must be this one
        if (t_replayPos >= t_replay->size() || (*t_replay)[t_replayPos].script != script) {
            t_replayBad = true;
            return 0;
        }
        const Recorded& r = (*t_replay)[t_replayPos++];
        *handled = r.handledAfter;
        return r.result;
    }
    if (holder) AddRef(holder);
    void* copy = holder;
    return DispatchOne(script, func, &copy, handled);  // it releases its copy
}
thread_local LONG t_changesAtStart = 0;  // g_treeChanges when the outermost replay started
thread_local bool t_stopped = false;     // the tree changed under a replay: it visits nothing more
uint32_t Replay(const Tree& t, int i, const char* child, const char* func, void* holder, char* handled) {
    const Node& n = t.nodes[i];
    uint32_t r = 0;
    if (!child || !strcmp(NameOf(n.name), child)) {
        r = DispatchNode(n.script, func, holder, handled);
        if (!t_replay && g_treeChanges != t_changesAtStart) t_stopped = true;  // (a handler changed a tree)
    }
    if (!*handled && !t_stopped && n.container) {
        for (int v = 0; v < 2; ++v)
            for (int k : n.child[v]) {
                if (k >= 0) r = Replay(t, k, child, func, holder, handled);
                if (*handled || t_stopped) break;
            }
    }
    return r;
}

// --- hooks ------------------------------------------------------------------------------------
thread_local int t_depth = 0;       // inside the engine's search (verify), nested searches pass through
thread_local int t_inDispatch = 0;  // inside a per-node dispatch (a handler's own Call()s)
thread_local int t_recordBase = 0;  // t_inDispatch when the recorded search started
uint32_t DispatchOne(void* script, const char* func, void** holder, char* handled) {
    bool record = t_record && t_inDispatch == t_recordBase;  // made by the recorded search itself
    ++t_inDispatch;
    uint32_t r = g_dispatchOne(script, func, holder, handled);
    --t_inDispatch;
    if (record) t_record->push_back({script, r, *handled});
    return r;
}
uint32_t Scan(void* root, const char* child, const char* func, void** holder, char* handled) {
    if (g_mode == 0 || t_depth > 0 || !root) {
        ++t_depth;
        uint32_t r = g_scan(root, child, func, holder, handled);
        --t_depth;
        return r;
    }
    Tree* t = TreeFor(root);
    if (!t) return g_scan(root, child, func, holder, handled);
    ++g_uses;
    if (g_mode == 2) {
        void* h = holder ? *holder : nullptr;
        bool outer = t->inUse == 0 && !t_stopped;
        LONG savedStart = t_changesAtStart;
        bool savedStopped = t_stopped;
        t_changesAtStart = g_treeChanges, t_stopped = false;
        ++t->inUse;
        uint32_t r = Replay(*t, 0, child, func, h, handled);
        --t->inUse;
        if (t_stopped) ++g_stopped;
        t_changesAtStart = savedStart, t_stopped = savedStopped;
        (void)outer;
        if ((g_uses & 0x3fffff) == 0)
            Log("entitysearch: %llu searches from the remembered tree, %llu tree builds, %llu stopped because a tree changed",
                static_cast<unsigned long long>(g_uses), static_cast<unsigned long long>(g_builds),
                static_cast<unsigned long long>(g_stopped));
        if (h) Release(h);  // the engine's search releases the holder it was given
        return r;
    }
    // verify: the engine searches (its dispatches recorded), then the replay must agree
    char handledIn = *handled;
    LONG changesBefore = g_treeChanges;
    std::vector<Recorded> rec;
    t_record = &rec;
    t_recordBase = t_inDispatch;
    ++t_depth;
    uint32_t r = g_scan(root, child, func, holder, handled);
    --t_depth;
    t_record = nullptr;
    if (g_treeChanges != changesBefore) {  // a handler changed a tree during the engine's search
        ++g_changedInSearch;
        return r;
    }
    char h2 = handledIn;
    t_replay = &rec, t_replayPos = 0, t_replayBad = false;
    uint32_t mine = Replay(*t, 0, child, func, nullptr, &h2);
    bool bad = t_replayBad || t_replayPos != rec.size() || mine != r || h2 != *handled;
    t_replay = nullptr;
    ++g_verified;
    if (bad && ++g_mismatches <= 20)
        Log("entitysearch: verify mismatch: %s:%s, engine %u dispatches result %u handled %d, replay used %zu result %u "
            "handled %d", child ? child : "*", func, static_cast<unsigned>(rec.size()), r, *handled, t_replayPos, mine,
            h2);
    if ((g_verified & 0x3ffff) == 0)
        Log("entitysearch: %llu searches verified, %llu mismatches, %llu tree builds, %zu trees, %llu searches during which a tree changed",
            static_cast<unsigned long long>(g_verified), static_cast<unsigned long long>(g_mismatches),
            static_cast<unsigned long long>(g_builds), t_trees ? t_trees->size() : 0,
            static_cast<unsigned long long>(g_changedInSearch));
    return r;
}

}  // namespace

bool Install(HMODULE engine, int mode) {
    g_mode = mode;
    void* scan = sites::Find(engine, kScan);
    void* one = sites::Find(engine, kDispatchOne);
    void* add = sites::Find(engine, kChildAdd);
    void* remove = sites::Find(engine, kChildRemove);
    const uintptr_t* got = engine::Resolved(engine);
    uintptr_t scriptIid = got[engine::ScriptIid], containerIid = got[engine::ContainerIid];  // interface ids (engine data)
    if (!scan || !one || !add || !remove || !scriptIid || !containerIid) {
        Log("entitysearch: engine functions not found (game version changed, or another hook such as the script call census is on them); disabled");
        return false;
    }
    auto* base = reinterpret_cast<uint8_t*>(engine);
    memcpy(g_scriptIid, base + scriptIid, 16), memcpy(g_containerIid, base + containerIid, 16);
    MH_Initialize();
    bool ok = MH_CreateHook(one, reinterpret_cast<void*>(&DispatchOne), reinterpret_cast<void**>(&g_dispatchOne)) == MH_OK &&
              MH_CreateHook(scan, reinterpret_cast<void*>(&Scan), reinterpret_cast<void**>(&g_scan)) == MH_OK &&
              MH_CreateHook(add, reinterpret_cast<void*>(&ChildAdd), reinterpret_cast<void**>(&g_childAdd)) == MH_OK &&
              MH_CreateHook(remove, reinterpret_cast<void*>(&ChildRemove), reinterpret_cast<void**>(&g_childRemove)) == MH_OK &&
              MH_EnableHook(add) == MH_OK && MH_EnableHook(remove) == MH_OK && MH_EnableHook(one) == MH_OK &&
              MH_EnableHook(scan) == MH_OK;
    g_treeHooks = ok;
    if (ok) g_scanAddress = scan;
    Log("entitysearch: entity search cache %s (mode %d: %s)", ok ? "installed" : "NOT installed", mode,
        mode == 1 ? "verify" : mode == 2 ? "replace" : "off");
    return ok;
}

void SetMode(int mode) { g_mode = mode; }

LONG TreeChanges() { return g_treeHooks ? g_treeChanges : -1; }
void* SearchFunction() { return g_scanAddress; }

}  // namespace tscfix::entitysearch
