// [fixes] script_call_cache: the script Call() binding's lookup of its own entity and script.
//
// Each Call() reads two Lua globals (mEntityAddress, mScriptAddress) to find the calling entity
// and script. They change only when a script state is set up or re-bound, so they are cached per
// lua_State (the owner's +0x10) and evicted in hooks on those two functions. A hit makes the
// call with the cached pair; a miss runs the original function.
#include "scriptcall.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <unordered_set>

#include "controllookup.h"
#include "log.h"
#include "nodeskip.h"
#include "sites.h"

namespace tscfix::scriptcall {
namespace {

constexpr sites::Fingerprint kCall = {0x0efeb0032e05dac8ull, 24};
constexpr sites::Fingerprint kSetup = {0xe8ca4cb83352c830ull, 32};
constexpr sites::Fingerprint kRebind = {0x62ff6d1df97b95a5ull, 32};
constexpr sites::Fingerprint kIsString = {0xf8cb0e3243d8eb27ull, 16};
constexpr sites::Fingerprint kToString = {0x0a30d89ea727f90dull, 16};
constexpr sites::Fingerprint kRemove = {0xc12a69350f0b6bbdull, 16};
constexpr sites::Fingerprint kPushString = {0x53abff03c4bfbc9eull, 24};
constexpr sites::Fingerprint kGetTable = {0x891ab60567a77109ull, 16};
constexpr sites::Fingerprint kIsUserdata = {0x0841d8892e2eab19ull, 16};
constexpr sites::Fingerprint kToUserdata = {0x680d8d1c211e1713ull, 16};
constexpr int kGlobals = -10001;  // LUA_GLOBALSINDEX (Lua 5.0)

using CallFn = int (*)(void* L);
using OwnerFn = uint64_t (*)(void* owner, uint64_t a, uint64_t b);
using IdxIntFn = int (*)(void* L, int idx);
using IdxPtrFn = void* (*)(void* L, int idx);
using IdxFn = void (*)(void* L, int idx);
using PushFn = void (*)(void* L, const char* s);
CallFn g_call = nullptr;
OwnerFn g_setup = nullptr, g_rebind = nullptr;
IdxIntFn g_isString = nullptr, g_isUserdata = nullptr;
IdxPtrFn g_toString = nullptr, g_toUserdata = nullptr;
IdxFn g_remove = nullptr, g_getTable = nullptr;
PushFn g_pushString = nullptr;
volatile bool g_active = false;

// lua_State -> (entity, script). Open addressing; inserts and evictions take the lock, lookups
// read under it too (script calls come from one thread; the lock is uncontended).
struct Entry {
    void* L;
    void* entity;
    void* script;
    void* entityVt;  // vtables when cached: an object being destroyed (or still being built) has
    void* scriptVt;  // another one, and the cached pair is then not used
};
constexpr size_t kSlots = 4096;  // power of two; script states in a session: a few hundred
Entry g_table[kSlots];
SRWLOCK g_lock = SRWLOCK_INIT;
uint64_t g_hits = 0, g_misses = 0, g_evictions = 0;

size_t SlotOf(void* L) { return (reinterpret_cast<uintptr_t>(L) >> 4) * 0x9E3779B97F4A7C15ull >> 52 & (kSlots - 1); }
bool Find(void* L, Entry& out) {
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (size_t i = SlotOf(L), n = 0; n < kSlots && g_table[i].L; i = (i + 1) & (kSlots - 1), ++n)
        if (g_table[i].L == L) {
            out = g_table[i], found = true;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return found;
}
void Insert(void* L, void* entity, void* script) {
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = SlotOf(L), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n)
        if (!g_table[i].L || g_table[i].L == L) {
            g_table[i] = {L, entity, script, *static_cast<void**>(entity), *static_cast<void**>(script)};
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
}
// Linear probing: deleting means re-inserting the rest of the cluster.
void Evict(void* L) {
    nodeskip::ScriptChanged();
    controllookup::PairsChanged();  // a pair of (Lua state, script) from before this is not used again
    if (!L) return;
    AcquireSRWLockExclusive(&g_lock);
    size_t i = SlotOf(L);
    for (size_t n = 0; n < kSlots && g_table[i].L && g_table[i].L != L; ++n) i = (i + 1) & (kSlots - 1);
    if (g_table[i].L == L) {
        ++g_evictions;
        g_table[i] = {};
        for (size_t j = (i + 1) & (kSlots - 1); g_table[j].L; j = (j + 1) & (kSlots - 1)) {
            Entry e = g_table[j];
            g_table[j] = {};
            for (size_t k = SlotOf(e.L);; k = (k + 1) & (kSlots - 1))
                if (!g_table[k].L) {
                    g_table[k] = e;
                    break;
                }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The engine's lookup of one global holding a userdata (stack left as it was).
void* GlobalUserdata(void* L, const char* name) {
    g_pushString(L, name);
    g_getTable(L, kGlobals);
    void* v = g_isUserdata(L, -1) == 1 ? g_toUserdata(L, -1) : nullptr;
    g_remove(L, -1);
    return v;
}

int g_verifyEvery = 0;  // [experiments] script_call_verify: every Nth hit also does the real lookup
volatile long g_verifyMismatches = 0;
uint64_t g_staleVt = 0;
int CallInner(void* L, uintptr_t frame) {
    if (!g_active) return g_call(L);
    Entry e;
    bool found = Find(L, e);
    if (found && (*static_cast<void**>(e.entity) != e.entityVt || *static_cast<void**>(e.script) != e.scriptVt)) {
        found = false;  // destroyed or rebuilt meanwhile: the engine's own lookup, cached again
        if (++g_staleVt <= 20) Log("scriptcall: cached entity %p changed its vtable; not used", e.entity);
    }
    if (found && g_verifyEvery && (g_hits % g_verifyEvery) == 0) {
        void* entity = GlobalUserdata(L, "mEntityAddress");
        void* script = entity ? GlobalUserdata(L, "mScriptAddress") : nullptr;
        if (entity != e.entity || script != e.script) {
            long n = InterlockedIncrement(&g_verifyMismatches);
            if (n <= 20) Log("scriptcall: verify mismatch: cached %p/%p, globals %p/%p", e.entity, e.script, entity, script);
            found = false;
        }
    }
    if (!found) {
        ++g_misses;
        void* entity = GlobalUserdata(L, "mEntityAddress");
        void* script = entity ? GlobalUserdata(L, "mScriptAddress") : nullptr;
        if (entity && script) Insert(L, entity, script);
        return g_call(L);  // the engine's own path this time
    }
    if (g_isString(L, 1) != 1) return g_call(L);  // also false on an empty stack (no arguments)
    ++g_hits;
    if ((g_hits & 0xfffff) == 0)
        Log("scriptcall: %llu hits, %llu misses, %llu evictions", static_cast<unsigned long long>(g_hits),
            static_cast<unsigned long long>(g_misses), static_cast<unsigned long long>(g_evictions));
    void* name = g_toString(L, 1);
    g_remove(L, 1);
    // The call with the cached pair: the script referenced for its duration, the entity's call
    // entry (vtable +0x88) given the name and a reference holder.
    auto** script = static_cast<void***>(e.script);
    auto addRef = reinterpret_cast<void (*)(void*)>((*script)[0]);
    auto release = reinterpret_cast<void (*)(void*)>((*script)[1]);
    addRef(script);
    addRef(script);
    void* holder = script;
    auto** entity = static_cast<void***>(e.entity);
    controllookup::SetCurrent(controllookup::Pair(L, e.script, frame));  // (Call() below puts the previous pair back)
    int r = reinterpret_cast<int (*)(void*, void*, void**)>((*entity)[0x88 / 8])(entity, name, &holder);
    release(script);
    return r;
}

// The call, with the node-set skip around it (nodeskip.h).
int Call(void* L) {
    int verdict = nodeskip::Enter(L, g_toString);
    if (verdict == nodeskip::kSkip) return 0;  // the call would change nothing and returns nothing
    // The control lookup (controllookup.h) reads the Lua stack of the call that runs now. The pair is
    // known only on the path with the remembered entity and script; a call inside this one sets its own.
    //
    // A Lua error that goes through this function skips the restore below, and the pair of the call
    // that ended stays set. The same rule as in nodeskip::Enter: the machine stack grows down, so
    // a call that still runs has its frame above this one. A pair with its frame at this address
    // or below it belongs to a call that is gone, and it is not put back. The control lookup has
    // the same test for a lookup that comes before the next Call() (controllookup.h).
    volatile char mark = 0;
    uintptr_t frame = reinterpret_cast<uintptr_t>(&mark);
    controllookup::Current outer = controllookup::SetCurrent({nullptr, nullptr});
    if (outer.L && outer.frame <= frame) outer = {};
    int r = CallInner(L, frame);
    controllookup::SetCurrent(outer);
    nodeskip::Leave(verdict, r);
    return r;
}

uint64_t Setup(void* owner, uint64_t a, uint64_t b) {
    void* L = nullptr;
    if (owner) memcpy(&L, static_cast<uint8_t*>(owner) + 0x10, 8);
    Evict(L);
    uint64_t r = g_setup(owner, a, b);
    if (owner) memcpy(&L, static_cast<uint8_t*>(owner) + 0x10, 8);
    Evict(L);
    return r;
}
uint64_t Rebind(void* owner, uint64_t a, uint64_t b) {
    void* L = nullptr;
    if (owner) memcpy(&L, static_cast<uint8_t*>(owner) + 0x10, 8);
    Evict(L);
    uint64_t r = g_rebind(owner, a, b);
    Evict(L);
    return r;
}

}  // namespace

bool Install(HMODULE engine) {
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_verifyEvery = GetPrivateProfileIntA("experiments", "script_call_verify", 0, ini);
    }
    void* call = sites::Find(engine, kCall);
    void* setup = sites::Find(engine, kSetup);
    void* rebind = sites::Find(engine, kRebind);
    g_isString = reinterpret_cast<IdxIntFn>(sites::Find(engine, kIsString));
    g_toString = reinterpret_cast<IdxPtrFn>(sites::Find(engine, kToString));
    g_remove = reinterpret_cast<IdxFn>(sites::Find(engine, kRemove));
    g_pushString = reinterpret_cast<PushFn>(sites::Find(engine, kPushString));
    g_getTable = reinterpret_cast<IdxFn>(sites::Find(engine, kGetTable));
    g_isUserdata = reinterpret_cast<IdxIntFn>(sites::Find(engine, kIsUserdata));
    g_toUserdata = reinterpret_cast<IdxPtrFn>(sites::Find(engine, kToUserdata));
    if (!call || !setup || !rebind || !g_isString || !g_toString || !g_remove || !g_pushString || !g_getTable ||
        !g_isUserdata || !g_toUserdata) {
        Log("scriptcall: engine functions not found (game version changed?); disabled");
        return false;
    }
    controllookup::SetToString(g_toString);
    MH_Initialize();
    bool ok = MH_CreateHook(setup, reinterpret_cast<void*>(&Setup), reinterpret_cast<void**>(&g_setup)) == MH_OK &&
              MH_CreateHook(rebind, reinterpret_cast<void*>(&Rebind), reinterpret_cast<void**>(&g_rebind)) == MH_OK &&
              MH_CreateHook(call, reinterpret_cast<void*>(&Call), reinterpret_cast<void**>(&g_call)) == MH_OK &&
              MH_EnableHook(setup) == MH_OK && MH_EnableHook(rebind) == MH_OK && MH_EnableHook(call) == MH_OK;
    Log("scriptcall: Call() self-lookup cache %s", ok ? "installed" : "NOT installed");
    return ok;
}

void SetActive(bool on) { g_active = on; }

}  // namespace tscfix::scriptcall
