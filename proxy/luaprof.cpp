#include "luaprof.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "sites.h"

namespace tscfix::luaprof {
namespace {

// lua_pcall in the engine's Lua 5.0.2 (game version v79.0a).
constexpr sites::Fingerprint kLuaPcall = {0xedb323c66b5a810cull, 16};

// Public Lua 5.0.2 layouts on x64 (lstate.h, lobject.h).
constexpr size_t kStateTop = 0x10;          // lua_State::top
constexpr size_t kTObjectSize = 16;         // { int tt; Value value; }
constexpr int kTypeFunction = 6;            // LUA_TFUNCTION
constexpr size_t kClosureIsC = 10;          // ClosureHeader::isC
constexpr size_t kLClosureProto = 24;       // LClosure::p
constexpr size_t kProtoSource = 64;         // Proto::source (TString*)
constexpr size_t kProtoLineDefined = 96;    // Proto::lineDefined
constexpr size_t kTStringChars = 24;        // getstr(ts) == (char*)(ts + 1)

using PcallFn = int (*)(void* L, int nargs, int nresults, int errfunc);
PcallFn g_original = nullptr;
DWORD g_mainThread = 0;
volatile bool g_active = false;

struct Stats {
    std::string name;
    uint64_t calls = 0;
    int64_t inclusive = 0;
    int64_t self = 0;
};
std::unordered_map<const void*, Stats> g_stats;

struct Active { const void* key; int64_t start; int64_t child; };
Active g_stack[64];
int g_depth = 0;

// Identifies the Lua function about to be called; guarded against bad memory.
const void* Identify(void* L, int nargs, char (&name)[200]) {
    __try {
        BYTE* top = *reinterpret_cast<BYTE**>(static_cast<BYTE*>(L) + kStateTop);
        BYTE* func = top - static_cast<size_t>(nargs + 1) * kTObjectSize;
        if (*reinterpret_cast<int*>(func) != kTypeFunction) return nullptr;
        BYTE* closure = *reinterpret_cast<BYTE**>(func + 8);
        if (closure[kClosureIsC]) {
            strcpy_s(name, "(C function)");
            return closure;
        }
        BYTE* proto = *reinterpret_cast<BYTE**>(closure + kLClosureProto);
        BYTE* source = *reinterpret_cast<BYTE**>(proto + kProtoSource);
        int line = *reinterpret_cast<int*>(proto + kProtoLineDefined);
        sprintf_s(name, "%.160s:%d", source ? reinterpret_cast<char*>(source + kTStringChars) : "?", line);
        return proto;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int HookedPcall(void* L, int nargs, int nresults, int errfunc) {
    if (!g_active || GetCurrentThreadId() != g_mainThread || g_depth >= 64)
        return g_original(L, nargs, nresults, errfunc);

    char name[200] = "?";
    const void* key = Identify(L, nargs, name);
    auto& st = g_stats[key];
    if (st.name.empty()) st.name = key ? name : "(unidentified)";

    g_stack[g_depth++] = {key, Now(), 0};
    int result = g_original(L, nargs, nresults, errfunc);
    Active a = g_stack[--g_depth];
    int64_t elapsed = Now() - a.start;
    st.calls++;
    st.inclusive += elapsed;
    st.self += elapsed - a.child;
    if (g_depth > 0) g_stack[g_depth - 1].child += elapsed;
    return result;
}

}  // namespace

bool Install(HMODULE engine, DWORD mainThreadId) {
    g_mainThread = mainThreadId;
    void* target = sites::Find(engine, kLuaPcall);
    if (!target) {
        Log("luaprof: lua_pcall not found (game version changed?); disabled");
        return false;
    }
    MH_Initialize();
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedPcall), reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("luaprof: hook failed");
        return false;
    }
    Log("luaprof: hooked lua_pcall");
    return true;
}

void SetActive(bool active) { g_active = active; }

void WriteReport(const char* path, double seconds) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double ms = 1000.0 / f.QuadPart;
    std::vector<const Stats*> rows;
    double totalSelf = 0;
    for (auto& [k, s] : g_stats) {
        rows.push_back(&s);
        totalSelf += s.self * ms;
    }
    std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a->self > b->self; });
    FILE* out = nullptr;
    if (fopen_s(&out, path, "w") || !out) return;
    fprintf(out, "# seconds=%.2f lua_self_ms=%.1f (%.1f%% of wall time)\n", seconds, totalSelf,
            100.0 * totalSelf / (seconds * 1000.0));
    fprintf(out, "# self_ms\tincl_ms\tcalls\tself_us_per_call\tfunction\n");
    for (auto s : rows)
        fprintf(out, "%.1f\t%.1f\t%llu\t%.1f\t%s\n", s->self * ms, s->inclusive * ms, s->calls,
                s->calls ? s->self * ms * 1000.0 / s->calls : 0.0, s->name.c_str());
    fclose(out);
}

}  // namespace tscfix::luaprof
