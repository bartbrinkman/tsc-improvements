// tscfix: runtime performance and automation layer for Train Simulator Classic.
// Loaded as a version.dll proxy; see docs/IP-POLICY.md for what may live here.
#include <windows.h>
#include <psapi.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include <MinHook.h>

#include "trace.h"
#include "ab.h"
#include "d3dlayer.h"
#include "experiments.h"
#include "framelog.h"
#include "overlap.h"
#include "overlapstate.h"
#include "config.h"
#include "glitch.h"
#include "scriptcall.h"
#define TSCFIX_SCRIPTCALL 1
#if __has_include("research_scriptcall_census.h")
#include "research_scriptcall_census.h"
#define TSCFIX_SCRIPTCALL_CENSUS 1
#endif
#include "engine_sites.h"
#include "entitysearch.h"
#include "qicache.h"
#define TSCFIX_QICACHE 1
#include "log.h"
#include "lookup.h"
#include "luaprof.h"
#include "flickercap.h"
#include "controllookup.h"
#include "funclookup.h"
#include "loadclock.h"
#include "nodelookup.h"
#include "nodeskip.h"
#include "postfx.h"
#include "resolve.h"
#include "dynheap.h"
#include "lodprobe.h"
#include "refcensus.h"
#include "safecounts.h"
#include "patches.h"
#include "physicsfix.h"
#include "schedwatch.h"
#include "scripttiming.h"
#include "fastpaths.h"
#include "profiler.h"
#include "richpresence.h"
#include "rtdevice.h"
#include "rtti.h"
#include "shadowcull.h"
#include "sites.h"
#if __has_include("binder.h")  // the engine-specific parts
#include "binder.h"
#include "pipeline.h"
#include "tracer.h"
#define TSCFIX_RESEARCH_BINDER 1
#endif

using namespace tscfix;

namespace {

// --- Scaleform GFx 4.x public API (from the SDK headers, not from the game) ----
// class ExternalInterface : public State {
//     virtual ~ExternalInterface();
//     virtual void Callback(Movie*, const char* methodName, const Value* args, unsigned argCount) = 0;
// };
constexpr int kCallbackSlot = 1;
constexpr const char* kExternalInterfaceType = ".?AVExternalInterface@GFx@Scaleform@@";

// GFx::Value: { ObjectInterface* pObjectInterface; ValueType Type; ValueUnion mValue; UPInt DataAux; }
struct GfxValue {
    void* objectInterface;
    uint32_t type;
    uint32_t pad;
    union {
        int32_t i;
        uint32_t u;
        double n;
        bool b;
        const char* s;
        const char** sManaged;
        const wchar_t* ws;
        void* data;
    } v;
    uintptr_t dataAux;
};
static_assert(sizeof(GfxValue) == 32, "GFx::Value layout");

enum : uint32_t {
    VT_Undefined, VT_Null, VT_Boolean, VT_Int, VT_UInt, VT_Number, VT_String, VT_StringW,
    VT_Object, VT_Array, VT_DisplayObject, VT_Closure,
    VTC_ManagedBit = 0x40, VTC_TypeMask = 0x8F,
};

using CallbackFn = void (*)(void* self, void* movie, const char* method, const GfxValue* args, unsigned argc);

struct HookedTable { void** vtable; CallbackFn original; };
HookedTable g_tables[16];
int g_tableCount = 0;

CallbackFn OriginalFor(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    for (int i = 0; i < g_tableCount; ++i)
        if (g_tables[i].vtable == vt) return g_tables[i].original;
    return nullptr;
}

// Formats one value into buf; guarded because string pointers come from the UI runtime.
void DescribeInto(const GfxValue& val, char (&buf)[300]) {
    uint32_t t = val.type & VTC_TypeMask & 0x0F;
    __try {
        switch (t) {
            case VT_Undefined: strcpy_s(buf, "undefined"); break;
            case VT_Null: strcpy_s(buf, "null"); break;
            case VT_Boolean: strcpy_s(buf, val.v.b ? "true" : "false"); break;
            case VT_Int: sprintf_s(buf, "%d", val.v.i); break;
            case VT_UInt: sprintf_s(buf, "%uu", val.v.u); break;
            case VT_Number: sprintf_s(buf, "%g", val.v.n); break;
            case VT_String: {
                const char* s = (val.type & VTC_ManagedBit) ? *val.v.sManaged : val.v.s;
                sprintf_s(buf, "\"%.256s\"", s ? s : "");
                break;
            }
            case VT_StringW: sprintf_s(buf, "L\"%.128ls\"", val.v.ws ? val.v.ws : L""); break;
            default: sprintf_s(buf, "<type %u @%p>", val.type, val.v.data); break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sprintf_s(buf, "<unreadable type %u>", val.type);
    }
}

std::string Describe(const GfxValue& val) {
    char buf[300];
    DescribeInto(val, buf);
    return buf;
}

// --- Configuration (tscfix.ini next to the game executable) ----------------------
struct Config {
    bool logUi = false;            // [debug] log_ui: log every menu callback
    bool heapBudget = true;        // [fixes] engine_heap_budget: the engine heap's budget is the machine's commit limit, not 2 GB
    bool crashRecord = true;       // [debug] crash_record: a crash is recorded in the log (module, stack, registers, memory use)
    bool crashLog = false;         // [debug] crash_log: log faulting module+offset and stack of serious exceptions
    bool autostartQuickDrive = false;  // [automation] autostart_quickdrive: skip the menu
    int autostartDelayMs = 1000;   // [automation] autostart_delay_ms
    std::string autostartTrigger;  // [automation] trigger: menu call that means "main menu is built"
    std::string autostartSequence; // [automation] sequence: menu calls that start the Quick Drive
    bool profile = false;          // [profile] enable: sample the main thread on request
    int profileIntervalUs = 1000;  // [profile] interval_us
    bool profileLua = false;       // [profile] lua: time Lua script calls during profiling
    bool profileD3d = false;       // [profile] d3d: count Direct3D/D3DX calls during profiling
    bool effectNoSaveState = false;  // [fixes] effect_nosavestate: D3DX effects skip state save/restore
    bool stateFilter = false;      // [fixes] state_filter: drop redundant Direct3D state calls
    bool physicsIndex = false;     // [fixes] physics_index: O(1) collideable removal (tile-unload stutter)
    bool abPhysics = false;
    bool abCameraRest = false;     // [ab] feature=camera_rest ([fixes] camera_rest: the standing cab camera is kept bit for bit)
    bool scriptTiming = false;     // [fixes] script_timing: skip script call timing while its overlay is off
    bool scriptCallCache = false;  // [fixes] script_call_cache (research): Call() self lookup cached per Lua state
    bool abScriptCallCache = false;
    bool qiCache = false;          // [fixes] qi_cache (research): entity-container interface query cached per thread
    bool abQiCache = false;
    bool abScriptTiming = false;
    bool fastStrings = false;      // [fixes] fast_strings: faster engine string compare (same results)
    bool multicore = true;         // [fixes] multicore: start the game as with -Multicore (engine job threads)
    bool richPresence = true;      // [fixes] rich_presence_async: Steam rich presence off the game thread
    bool abFastStrings = false;
    bool fastNodeLookup = false;   // [fixes] fast_node_lookup: a model's node by name from a remembered index
    bool abNodeLookup = false;
    bool abControlLookup = false;  // A/B feature control_lookup ([fixes] fast_control_lookup)
    bool preciseLoadClock = false; // [fixes] precise_load_clock: the loading fibers' slice clock counts true milliseconds
    bool abLoadClock = false;
    bool fiberTime = false;        // [experiments] fiber_time: log the game thread's time in the loading fibers per frame
    bool abFunctionLookup = false;
    bool abNodeSetSkip = false;
    int collideableList = 0;       // local: list offset in the physics manager
    int renderThread = 0;          // [render] thread: 0 off, 1 pass-through device wrapper, 2 threaded
    bool runtimeLock = true;       // [render] mt_lock: create the device with D3DCREATE_MULTITHREADED
    bool force9On12 = false;       // [render] force_9on12: diagnostic, DX9 build through D3D9On12
    bool default9On12Args = false; // [render] default_9on12_args: diagnostic, drop the DX12 build's 9On12 overrides
    int swapEffect = 0;            // [render] swap_effect: diagnostic, override D3DPRESENT_PARAMETERS::SwapEffect
    int shadowEvery = 0;           // [experiments] shadow_every: refresh the shadow map every N frames (0 = off)
    bool abShadow = false;         // shadow_skip is under A/B control (toggled live instead of always on)
    bool sceneryProbe = false;     // [experiments] scenery_probe: log scenery render calls per frame
    bool shadowCull = false;       // [experiments] shadow_cull: contribution culling of shadow casters (probe when A/B)
    bool abShadowCull = false;
    bool abBlendOverTime = false;  // blend_over_time is under A/B control (installed, switched by the measurement window)
    bool abGlowGain = false;       // glow_far_gain_percent is under A/B control
    bool abSteadyGlow = false;     // steady_glow is under A/B control (installed, switched by the measurement window)
    shadowcull::Config cull;
    bool lookupProbe = false;      // [experiments] lookup_probe: log script child-call lookups per frame
    bool speedLimits = false;      // [experiments] speed_limits: publish the player's speed limits
    uintptr_t speedLimitCaller = 0;  // local: call site that serves the player's train
    bool shadowStagger = false;    // [experiments] shadow_stagger: nearest cascade every frame, others in turn
    bool abStagger = false;        // shadow_cascades is under A/B control
    bool shadowWorker = false;     // [experiments] shadow_worker: shadow update on a worker thread
    bool mapSplit = false;         // [threading] shadow_threads=2: the widest shadow map on a second shadow thread
    bool abMapSplit = false;       // ... under A/B control (feature shadow_threads)
    int mapSplitMap = -1;          // [experiments] shadow_threads_map: which map (-1: index 0, the one drawn last)
    bool overlapCensus = false;    // [experiments] overlap_census: log render module order and time
    bool overlap = false;          // [experiments] overlap: shadow pass alongside later render modules
    bool abOverlap = false;        // overlap is under A/B control
    char overlapModules[512] = ""; // [experiments] overlap_modules: modules allowed alongside
    bool overlapState = false;     // [experiments] overlap_state: per-thread render scratch state (on with overlap)
    bool abOverlapState = false;
    bool binder = false;           // [research] binder: per-thread draw binder for the shadow pass
    bool abBinder = false;
    bool abShadowWorker = false;   // shadow_worker is under A/B control
    bool shadowTrace = false;      // [experiments] shadow_trace: hooks installed, staggering off, one-frame trace
    int shadowLod = 0;             // [experiments] shadow_lod: extra LOD levels for shadow casters (0 = off)
    bool abShadowLod = false;      // shadow_lod is under A/B control
    int lodBiasOffset = 0;         // local: LOD bias field in the render-settings object
    int cascadeOffset = 0, cascadeStride = 0, cascadeCount = 0;  // local: cascade matrix array
    bool patches = false;          // [patches] enable: prefetch caves from the local site list
    bool abPatches = false;        // the caves are under A/B control
    char iniPath[MAX_PATH] = {};
} g_cfg;
char g_glitchVariants[256] = "";  // [experiments] glitch_variants (switch names)
int g_glitchSlice = 150;

DWORD g_mainThreadId = 0;          // the thread that loads the engine (and runs the game loop)

// --- Crash log: first-chance view of serious exceptions, before the game's own crash reporter ---
void LogAddress(const char* what, DWORD64 addr) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        Log("crash:   %s %s+0x%llx", what, base ? base + 1 : name, addr - reinterpret_cast<DWORD64>(m));
    } else {
        Log("crash:   %s 0x%llx", what, addr);
    }
}

// --- -Multicore by default ---------------------------------------------------------------------
// The engine asks its command line for options through one lookup function; when it asks for
// -Multicore it starts its job worker threads. This DLL is loaded by the engine module (before
// the engine initializes), so the lookup is hooked right away and answers yes for -Multicore.
constexpr sites::Fingerprint kOptionLookup = {0xc59670d86ed1e54full, 16};  // (command line, option) -> bool
using OptionLookupFn = bool (*)(void* commandLine, const char* option);
OptionLookupFn g_optionLookup = nullptr;

bool OptionLookup(void* commandLine, const char* option) {
    if (option && _stricmp(option, "-Multicore") == 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("config: -Multicore on (engine job threads)");
        }
        return true;
    }
    return g_optionLookup(commandLine, option);
}

bool EnableMulticore() {
    HMODULE engine = GetModuleHandleA("GameManagerVC64.dll");
    if (!engine) engine = GetModuleHandleA("GameManagerDX12_VC64.dll");
    void* lookup = engine ? sites::Find(engine, kOptionLookup) : nullptr;
    if (!lookup) return false;
    MH_Initialize();
    return MH_CreateHook(lookup, reinterpret_cast<void*>(&OptionLookup), reinterpret_cast<void**>(&g_optionLookup)) ==
               MH_OK &&
           MH_EnableHook(lookup) == MH_OK;
}

// The class name of a thrown C++ exception (MSVC x64 throw information), or "?".
const char* CxxTypeName(const EXCEPTION_RECORD* r) {
    __try {
        if (r->NumberParameters < 4) return "?";
        auto* base = reinterpret_cast<const uint8_t*>(r->ExceptionInformation[3]);
        auto* info = reinterpret_cast<const int32_t*>(r->ExceptionInformation[2]);  // ThrowInfo
        auto* array = reinterpret_cast<const int32_t*>(base + info[3]);            // catchable types
        if (array[0] < 1) return "?";
        auto* type = reinterpret_cast<const int32_t*>(base + array[1]);            // first catchable type
        return reinterpret_cast<const char*>(base + type[1] + 16);                 // type descriptor name
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

// A thread of ours that runs engine code (shadow worker, forked module worker): the engine has
// no catch frame of its own there, so a C++ exception it throws is ours to report.
bool OnOwnWorker() {
    DWORD ids[2] = {};
    overlap::WorkerThreadIds(ids);
    DWORD t = GetCurrentThreadId();
    return t && (t == ids[0] || t == ids[1]);
}

// How much memory the game uses, and how much the machine has left. The game reports nearly every
// crash as "out of memory" (its error handler turns any access violation into that message), so
// the record says what memory use really was. One line a minute shows the trend before a crash.
// The render engine allocates through its own "PassThru" heap: malloc with a byte budget, set to
// 2 GB when the heap is set up (mov r8d, 0x80000000 before the call, in the function located
// here). An allocation larger than what is left of the budget fails, however much memory the
// machine has. The heap object keeps bytes in use at +0x50 and budget left at +0x58.
constexpr sites::Fingerprint kEngineHeapSetup = {0x4ea310d7ebdd04adull, 16};
uint8_t* EngineHeap() {
    static uint8_t* heap = nullptr;
    static bool looked = false;
    if (looked) return heap;
    HMODULE engine = GetModuleHandleA("GameManagerVC64.dll");
    if (!engine) return nullptr;  // not loaded yet: look again later
    looked = true;
    auto* fn = static_cast<uint8_t*>(sites::Find(engine, kEngineHeapSetup));
    static const uint8_t kBudget[] = {0x41, 0xB8, 0x00, 0x00, 0x00, 0x80};  // mov r8d, 0x80000000
    for (int i = 0; fn && i < 0x100; ++i) {
        if (memcmp(fn + i, kBudget, sizeof kBudget)) continue;
        for (int k = i + 6; k < i + 24; ++k)
            if (fn[k] == 0x48 && fn[k + 1] == 0x8D && fn[k + 2] == 0x0D) {  // lea rcx, [rip+disp32]: the heap
                int32_t disp;
                memcpy(&disp, fn + k + 3, 4);
                heap = fn + k + 7 + disp;
                break;
            }
        break;
    }
    Log("memory: engine heap %s", heap ? "found" : "not found (game version changed?)");
    return heap;
}

// --- engine heap budget from the host ----------------------------------------------------------
// The heap's setup call (heap, name, budget in bytes) is hooked before the engine initializes: the
// "PassThru" heap gets the machine's commit limit (RAM plus page file) as its budget instead of
// 2 GB. malloc itself then decides whether memory is left. [fixes] engine_heap_budget=0 keeps 2 GB.
constexpr sites::Fingerprint kHeapInit = {0xceb313ac2921fa1full, 32};
using HeapInitFn = uint8_t (*)(void* heap, const char* name, uint64_t budget);
HeapInitFn g_heapInit = nullptr;
uint64_t g_heapBudget = 0x80000000ull;
// Every heap the engine sets up through this call, with its name and budget: "System", "PassThru",
// "Streaming Heap" (64 MB, twice), "Streaming Sub Heap" and "DeltaTemp" (16 MB). The memory lines in
// the log give each one's use and the highest use seen, so a run shows which budget comes close.
struct KnownHeap {
    uint8_t* heap;
    char name[24];
    uint64_t budget;
    std::atomic<uint64_t> peak;
};
KnownHeap g_heaps[12];
std::atomic<int> g_heapCount{0};
void NoteHeap(void* heap, const char* name, uint64_t budget) {
    int i = g_heapCount.load();
    if (i >= 12) return;
    g_heaps[i].heap = static_cast<uint8_t*>(heap);
    strncpy_s(g_heaps[i].name, name ? name : "?", _TRUNCATE);
    g_heaps[i].budget = budget;
    g_heapCount.store(i + 1);
    Log("memory: engine heap \"%s\" set up with a budget of %.1f MB", g_heaps[i].name, budget / 1048576.0);
}
// (every second, from the memory thread) the highest use of each heap
void NoteStaticHeaps();
void SampleHeaps() {
    static int calls = 0;
    if (++calls == 20) NoteStaticHeaps();  // (the engine has set them up by then)
    int n = g_heapCount.load();
    for (int i = 0; i < n; ++i) {
        uint64_t used;
        memcpy(&used, g_heaps[i].heap + 0x50, 8);
        if (used > g_heaps[i].peak.load(std::memory_order_relaxed)) g_heaps[i].peak.store(used, std::memory_order_relaxed);
    }
}
// Two more heaps are set up by other functions, inside memory taken from the System heap: "Dynamic"
// (4 MB, a Doug Lea allocator) and "LoadBuffer" (32 KB, a stack). They are global objects of the
// engine. For the log they are found by address and accepted only when the name in the object fits
// (research: the addresses are found by fingerprint).
void NoteStaticHeaps() {
    static bool done = false;
    if (done) return;
    HMODULE engine = GetModuleHandleA("GameManagerVC64.dll");
    if (!engine) return;
    done = true;
    const uintptr_t* got = tscfix::engine::Resolved(engine);
    const struct { uintptr_t rva; const char* name; } kStatic[] = {{got[tscfix::engine::HeapDynamic], "Dynamic"}, {got[tscfix::engine::HeapLoadBuffer], "LoadBuffer"}};
    for (const auto& s : kStatic) {
        if (!s.rva) continue;
        uint8_t* heap = reinterpret_cast<uint8_t*>(engine) + s.rva;
        __try {
            if (strncmp(reinterpret_cast<const char*>(heap + 0x10), s.name, 0x40) != 0) continue;
            uint64_t used, left;
            memcpy(&used, heap + 0x50, 8);
            memcpy(&left, heap + 0x58, 8);
            NoteHeap(heap, s.name, used + left);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}
void LogHeaps(const char* tag) {
    NoteStaticHeaps();
    int n = g_heapCount.load();
    for (int i = 0; i < n; ++i) {
        uint64_t used;
        int64_t left;
        memcpy(&used, g_heaps[i].heap + 0x50, 8);
        memcpy(&left, g_heaps[i].heap + 0x58, 8);
        Log("%s: heap \"%s\": %.1f MB in use (highest %.1f), %.1f MB of its budget left (budget %.1f MB)", tag, g_heaps[i].name,
            used / 1048576.0, g_heaps[i].peak.load() / 1048576.0, left / 1048576.0, g_heaps[i].budget / 1048576.0);
    }
}
uint8_t HeapInit(void* heap, const char* name, uint64_t budget) {
    if (name && !strcmp(name, "PassThru") && budget == 0x80000000ull) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPageFile > budget) budget = ms.ullTotalPageFile;
        g_heapBudget = budget;
        Log("memory: engine heap budget %.0f MB (the machine's commit limit) instead of 2048 MB", budget / 1048576.0);
    }
    // The other heaps of this kind have small fixed budgets: "Streaming Heap" 64 MB (tile and asset
    // streaming; 46.5 MB of it in use on a Haarlem drive), its sub heap 6.4 MB, "System" 8 MB,
    // "DeltaTemp" 16 MB. The budget is only a limit: memory comes from malloc when it is asked for.
    // A request above what is left gets a null pointer back (engine allocator), and the streaming
    // heap has no out-of-memory handler, so the caller goes on with null. They get 16 times their
    // budget (at most 1 GB: the engine handles these sizes as signed 32-bit numbers).
    if (name && budget && budget <= 0x4000000ull &&
        (!strcmp(name, "Streaming Heap") || !strcmp(name, "Streaming Sub Heap") || !strcmp(name, "System") || !strcmp(name, "DeltaTemp"))) {
        uint64_t raised = budget * 16;
        if (raised > 0x40000000ull) raised = 0x40000000ull;
        Log("memory: engine heap \"%s\": budget %.1f MB instead of %.1f MB", name, raised / 1048576.0, budget / 1048576.0);
        budget = raised;
    }
    uint8_t ok = g_heapInit(heap, name, budget);
    NoteHeap(heap, name, budget);
    return ok;
}
// The sound system takes one block of 100 MB when it starts and runs its own allocator inside it
// ("cRailSimSoundSystem Heap"). When that is full the engine stops ("cRailSimSoundSystem out of
// memory"). A Haarlem drive had 75.9 MB of it in use at its highest. The size is a constant in the
// setup function, in two instructions (the allocation and the heap setup). Both get 512 MB. The
// change is made only when each instruction is found exactly once in its known form.
constexpr sites::Fingerprint kSoundSetup = {0x13aa28b0f1e06fefull, 24};
bool RaiseSoundHeap(HMODULE engine) {
    auto* fn = static_cast<uint8_t*>(sites::Find(engine, kSoundSetup));
    if (!fn) return false;
    static const uint8_t alloc[] = {0xb9, 0x00, 0x00, 0x40, 0x06};        // mov ecx, 0x6400000
    static const uint8_t setup[] = {0x41, 0xb9, 0x00, 0x00, 0x40, 0x06};  // mov r9d, 0x6400000
    uint8_t* at[2] = {};
    int found[2] = {};
    for (int i = 0; i < 0xa0; ++i) {
        if (!memcmp(fn + i, setup, sizeof setup)) at[1] = fn + i + 2, ++found[1], i += 5;
        else if (!memcmp(fn + i, alloc, sizeof alloc)) at[0] = fn + i + 1, ++found[0], i += 4;
    }
    if (found[0] != 1 || found[1] != 1) return false;
    const uint32_t size = 0x20000000;  // 512 MB
    for (uint8_t* p : at) {
        DWORD old;
        if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(p, &size, 4);
        VirtualProtect(p, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 4);
    }
    return true;
}
bool InstallHeapBudget() {
    HMODULE engine = GetModuleHandleA("GameManagerVC64.dll");
    void* init = engine ? sites::Find(engine, kHeapInit) : nullptr;
    if (engine) Log("memory: sound system heap %s", RaiseSoundHeap(engine) ? "512 MB instead of 100 MB" : "not changed (setup code not found in its known form)");
    if (!init) return false;
    MH_Initialize();
    return MH_CreateHook(init, reinterpret_cast<void*>(&HeapInit), reinterpret_cast<void**>(&g_heapInit)) == MH_OK &&
           MH_EnableHook(init) == MH_OK;
}

// --- what the game's crash dialog says ----------------------------------------------------------
// The engine's fatal-error handler starts ErrorReporter.exe with an exception code and a text. It
// passes its "out of memory" code and its low-memory advice for every access violation and for
// every stop without an exception (pure virtual call, invalid parameter). Here the reporter gets
// the code of the exception this proxy recorded, and a text that says what happened, where, and
// whether memory was short.
struct LastCrash {
    volatile LONG set;
    DWORD code, thread;
    ULONG_PTR info0, info1;
    char where[96];
    char type[96];
    ULONGLONG tick;
};
LastCrash g_lastCrash{};
void Describe(DWORD64 addr, char* out, size_t cap) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        sprintf_s(out, cap, "%s+0x%llx", base ? base + 1 : name, addr - reinterpret_cast<DWORD64>(m));
    } else {
        sprintf_s(out, cap, "address 0x%llx (no module)", addr);
    }
}
const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "a file or page could not be read (in-page error)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_BREAKPOINT: return "engine assertion";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun or fast fail";
    case 0xC0000017: return "Windows could not give the game more memory";
    case 0xE06D7363: return "unhandled C++ exception";
    default: return "exception";
    }
}
using CreateProcessWFn = BOOL (WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                         LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessWFn g_createProcessW = nullptr;
// Builds the reporter's command line: code, the two arguments the engine gave (dump folder, title),
// the engine's first sentence ("Fatal error in mode ..."), then the facts. Kept short: the
// reporter formats its text in a small buffer.
bool ReporterCommand(const wchar_t* original, wchar_t* out, size_t cap) {
    static const wchar_t kExe[] = L"ErrorReporter.exe ";
    const size_t n = wcslen(kExe);
    if (!original || _wcsnicmp(original, kExe, n)) return false;
    DWORD engineCode = wcstoul(original + n, nullptr, 16);
    const wchar_t* q[6] = {};  // the quotes around arguments 2, 3 and 4
    int quotes = 0;
    for (const wchar_t* p = original + n; *p && quotes < 6; ++p)
        if (*p == L'"') q[quotes++] = p;
    if (quotes < 6) return false;
    wchar_t first[160] = L"";
    const wchar_t* end = wcsstr(q[4] + 1, L"\n\n");
    if (end && end < q[5]) wcsncpy_s(first, q[4] + 1, end - q[4] - 1 < 150 ? end - q[4] - 1 : 150);

    bool recorded = g_lastCrash.set && GetTickCount64() - g_lastCrash.tick < 60000;
    DWORD code = recorded ? g_lastCrash.code : engineCode == 0xC000FFF6 ? 0xC0000409 : engineCode;
    char what[420];
    if (recorded && g_lastCrash.code == EXCEPTION_ACCESS_VIOLATION)
        sprintf_s(what, "What happened: access violation in %s. The game tried to %s address 0x%llx.", g_lastCrash.where,
                  g_lastCrash.info0 == 1 ? "write" : g_lastCrash.info0 == 8 ? "run code at" : "read",
                  static_cast<unsigned long long>(g_lastCrash.info1));
    else if (recorded)
        sprintf_s(what, "What happened: %s%s%s in %s (code 0x%08lX).", ExceptionName(g_lastCrash.code),
                  g_lastCrash.type[0] ? " of type " : "", g_lastCrash.type, g_lastCrash.where, g_lastCrash.code);
    else
        sprintf_s(what, "What happened: the game stopped itself without a processor exception (pure virtual call, "
                        "invalid parameter or abort in the C runtime).");

    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    uint64_t used = 0, left = 0;
    uint8_t* heap = EngineHeap();
    if (heap && !heap[8]) heap = nullptr;  // (+8: the heap is set up; before that its figures are zero)
    if (heap) memcpy(&used, heap + 0x50, 8), memcpy(&left, heap + 0x58, 8);
    const char* verdict =
        ms.ullAvailPageFile < 512ull << 20 ? "The machine was out of memory."
        : heap && static_cast<int64_t>(left) < 64ll << 20 ? "The game's engine heap budget was used up."
        : recorded && g_lastCrash.code == 0xC0000017 ? "Windows refused a memory request."
        : "Memory was not short.";
    char memory[360];
    if (heap)
        sprintf_s(memory, "Memory: the game used %.1f GB. The machine had %.1f GB free. Engine heap: %.1f GB in use, %.1f GB of "
                          "its budget left. %s", pm.PrivateUsage / gb, ms.ullAvailPageFile / gb, used / gb,
                  static_cast<int64_t>(left) / gb, verdict);
    else
        sprintf_s(memory, "Memory: the game used %.1f GB. The machine had %.1f GB free. %s", pm.PrivateUsage / gb,
                  ms.ullAvailPageFile / gb, verdict);
    Log("crash: error dialog: %s %s", what, memory);
    // (What follows the text is kept: an engine assertion passes its own message as a fifth argument.)
    int len = swprintf_s(out, cap, L"ErrorReporter.exe %08lX %.*s \"%s\n\n%hs\n\n%hs\n\nThe full record is in tscfix.log in the game folder.\"%s",
                         code, static_cast<int>(q[3] - q[0] + 1), q[0], first, what, memory, q[5] + 1);
    return len > 0;
}
BOOL WINAPI CreateProcessWHook(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                               DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    static wchar_t ours[2048];
    static volatile LONG used = 0;
    if (cmd && !app && InterlockedCompareExchange(&used, 1, 0) == 0) {
        if (ReporterCommand(cmd, ours, 2048)) return g_createProcessW(app, ours, pa, ta, inherit, flags, env, dir, si, pi);
        InterlockedExchange(&used, 0);
    }
    return g_createProcessW(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}
// [experiments] crash_dialog_test=1: logs the reporter command lines for three sample cases
// (access violation, a stop without an exception, an engine assertion) without a crash.
void LogSampleDialogs() {
    static const wchar_t* kEngine[] = {
        L"ErrorReporter.exe C000FFF6 " L"\"" L"D:" L"\\" L"Game" L"\\" L"TempDump" L"\"" L" " L"\"" L"RailWorks" L"\"" L" " L"\"" L"Fatal error in mode DRIVE." L"\n" L"\n" L"low memory text" L"\"",
        L"ErrorReporter.exe 80000003 " L"\"" L"D:" L"\\" L"Game" L"\\" L"TempDump" L"\"" L" " L"\"" L"RailWorks" L"\"" L" " L"\"" L"Fatal error in mode LOADING." L"\n" L"\n" L"support text" L"\"" L" " L"\"" L"Assertion failed: sample" L"\"",
    };
    static wchar_t out[2048];
    char narrow[4096];
    auto show = [&](const char* what, const wchar_t* engine) {
        bool ok = ReporterCommand(engine, out, 2048);
        WideCharToMultiByte(CP_UTF8, 0, ok ? out : L"(not rewritten)", -1, narrow, sizeof narrow, nullptr, nullptr);
        for (char* c = narrow; *c; ++c)
            if (*c == '\n') *c = '|';
        Log("crash dialog test, %s: %s", what, narrow);
    };
    g_lastCrash = {};
    show("stop without an exception", kEngine[0]);
    g_lastCrash.code = EXCEPTION_ACCESS_VIOLATION;
    g_lastCrash.info0 = 1;
    g_lastCrash.info1 = 0xffffffff4ecede90ull;
    HMODULE engineModule = GetModuleHandleA("GameManagerVC64.dll");
    Describe(reinterpret_cast<DWORD64>(engineModule) + (engineModule ? tscfix::engine::Resolved(engineModule)[tscfix::engine::CrashSample] : 0), g_lastCrash.where, sizeof g_lastCrash.where);
    g_lastCrash.tick = GetTickCount64();
    g_lastCrash.set = 1;
    show("access violation", kEngine[0]);
    g_lastCrash.code = EXCEPTION_BREAKPOINT;
    show("engine assertion", kEngine[1]);
    g_lastCrash = {};
}

bool InstallErrorMessage() {
    MH_Initialize();
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32"), "CreateProcessW"));
    return target && MH_CreateHook(target, reinterpret_cast<void*>(&CreateProcessWHook),
                                   reinterpret_cast<void**>(&g_createProcessW)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

void LogMemory(const char* tag) {
    if (uint8_t* heap = EngineHeap()) {
        uint64_t used, left;
        memcpy(&used, heap + 0x50, 8);
        memcpy(&left, heap + 0x58, 8);
        Log("%s: engine heap %.0f MB in use, %.0f MB of its budget left", tag, used / 1048576.0,
            static_cast<int64_t>(left) / 1048576.0);
    }
    LogHeaps(tag);
    dynheap::LogUse(tag);
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const double mb = 1024.0 * 1024.0;
    Log("%s: game uses %.0f MB committed (peak %.0f), %.0f MB in RAM; machine: %.0f MB RAM free of %.0f, %.0f MB commit free of %.0f; "
        "handles %lu, GDI %lu, USER %lu",
        tag, pm.PrivateUsage / mb, pm.PeakPagefileUsage / mb, pm.WorkingSetSize / mb, ms.ullAvailPhys / mb, ms.ullTotalPhys / mb,
        ms.ullAvailPageFile / mb, ms.ullTotalPageFile / mb, handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
        GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
}
// [experiments] memory_guard_gb (0 = off): test runs set it. If the game's committed memory goes over
// this size, the game is ended before it takes the machine's memory.
int g_memoryGuardGb = 0;
DWORD WINAPI MemoryLogMain(void*) {
    for (int second = 1;; ++second) {
        Sleep(1000);
        SampleHeaps();
        funclookup::LogUse();
        nodeskip::LogUse();
        controllookup::LogUse();
        if (second % 60 == 0) {  // one plain line per minute: how the game runs
            long frames, over50, over100, worst, activeMs;
            d3d::ReadFrameHealth(&frames, &over50, &over100, &worst, &activeMs);
            PROCESS_MEMORY_COUNTERS_EX mem{};
            mem.cb = sizeof mem;
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof mem);
            if (frames > 300 && activeMs > 5000)  // (a minute that was mostly a loading screen gives no line)
                Log("health: last minute: %.1f frames per second, %ld frames over 50 ms, %ld over 100 ms, longest %ld ms, memory %.1f GB", frames * 1000.0 / activeMs,
                    over50, over100, worst, mem.PrivateUsage / 1073741824.0);
        }
        if (g_memoryGuardGb > 0) {
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
            if (pm.PrivateUsage > static_cast<uint64_t>(g_memoryGuardGb) << 30) {
                Log("memory guard: the game went over %d GB; ending it", g_memoryGuardGb);
                LogMemory("memory guard");
                TerminateProcess(GetCurrentProcess(), 0xC000FFF7);
            }
        }
        if (second % 60 == 0) LogMemory("memory");
    }
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    static volatile LONG logged = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool serious = code >= 0xC0000000 || code == EXCEPTION_BREAKPOINT;  // engine assertions end in int3
    if (code == 0xE06D7363) {
        // Engine C++ exceptions are normal on the game thread (it catches them). On our workers
        // each one is logged with its type and where it was thrown.
        static volatile LONG thrown = 0;
        if (!OnOwnWorker() || InterlockedIncrement(&thrown) > 6) return EXCEPTION_CONTINUE_SEARCH;
        Log("crash: C++ exception %s thrown on worker thread %lu", CxxTypeName(ep->ExceptionRecord), GetCurrentThreadId());
    } else if (!serious) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
#ifdef TSCFIX_RESEARCH_BINDER
    if (tracer::Tracing()) return EXCEPTION_CONTINUE_SEARCH;  // the tracer's own guarded reads
#endif
    if (InterlockedIncrement(&logged) > 12) return EXCEPTION_CONTINUE_SEARCH;
    Log("crash: exception 0x%08lx on thread %lu (info %llx %llx)", code, GetCurrentThreadId(),
        ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0ull,
        ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0ull);
    rt::LogThreadRole();
    LogMemory("crash:   memory");
    {  // (the latest serious exception is the one the engine's handler reports next)
        g_lastCrash.code = code;
        g_lastCrash.thread = GetCurrentThreadId();
        g_lastCrash.info0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
        g_lastCrash.info1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        Describe(ep->ContextRecord->Rip, g_lastCrash.where, sizeof g_lastCrash.where);
        g_lastCrash.type[0] = 0;
        if (code == 0xE06D7363) strncpy_s(g_lastCrash.type, CxxTypeName(ep->ExceptionRecord), _TRUNCATE);
        g_lastCrash.tick = GetTickCount64();
        g_lastCrash.set = 1;
    }
    CONTEXT ctx = *ep->ContextRecord;
    if (code == EXCEPTION_ACCESS_VIOLATION && ctx.Rip >= 0x10000) {
        // What the faulting instruction worked on: the registers, and the memory the pointer-like
        // ones lead to (a crash is then read from its record instead of reproduced).
        Log("crash:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx rsp %llx rbp %llx", ctx.Rax,
            ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8, ctx.R9, ctx.Rsp, ctx.Rbp);
        const struct { const char* name; DWORD64 v; } regs[] = {{"rax", ctx.Rax}, {"rbx", ctx.Rbx}, {"rcx", ctx.Rcx},
                                                                {"rdx", ctx.Rdx}, {"rsi", ctx.Rsi}, {"rdi", ctx.Rdi}};
        for (auto& r : regs) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (r.v < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(r.v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize < r.v + 64)
                continue;
            DWORD64 q[8];
            memcpy(q, reinterpret_cast<void*>(r.v), sizeof q);
            Log("crash:   [%s] %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx", r.name, q[0], q[1], q[2],
                q[3], q[4], q[5], q[6], q[7]);
        }
    }
    if (ctx.Rip < 0x10000) {  // a call through a null or tiny pointer: the caller is on the stack
        LogAddress("at", ctx.Rip);
        Log("crash:   rax %llx rbx %llx rcx %llx rdx %llx r8 %llx r9 %llx rsp %llx rbp %llx", ctx.Rax, ctx.Rbx, ctx.Rcx,
            ctx.Rdx, ctx.R8, ctx.R9, ctx.Rsp, ctx.Rbp);
        // The stack may be broken (a return to 0): list values on it that point into modules.
        int found = 0;
        for (int i = 0; i < 512 && found < 16; ++i) {
            DWORD64 v = reinterpret_cast<DWORD64*>(ctx.Rsp)[i];
            HMODULE m = nullptr;
            if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<const char*>(v), &m) && m) {
                char tag[32];
                sprintf_s(tag, "stack+%x", i * 8);
                LogAddress(tag, v);
                ++found;
            }
        }
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
    }
    for (int frame = 0; frame < 24 && ctx.Rip; ++frame) {
        LogAddress(frame ? "from" : "at", ctx.Rip);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {  // leaf function: return address is on top of the stack
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix.ini");
    strcpy_s(g_cfg.iniPath, path);
    if (GetPrivateProfileIntA("threading", "render", 0, path) != 0) {
        config::UseRenderSplitPreset();
#if __has_include("pipeline.h")
        pipeline::AddPresets();
#endif
    }
    g_cfg.patches = tscfix::config::Int("patches", "enable", 0, path) != 0;
    {
        // glitch_watch_rva (hex): an engine caller of the effect's SetTexture whose material and
        // slot registers are logged with each changed draw.
        char rva[32] = "";
        GetPrivateProfileStringA("experiments", "glitch_watch_rva", "", rva, sizeof rva, path);
        glitch::Configure(tscfix::config::Int("experiments", "glitch_detect", 0, path) != 0, strtoull(rva, nullptr, 16));
        GetPrivateProfileStringA("experiments", "glitch_variants", "", g_glitchVariants, sizeof g_glitchVariants, path);
        g_glitchSlice = tscfix::config::Int("experiments", "glitch_slice", 150, path);
        glitch::ConfigureOracle(tscfix::config::Int("experiments", "oracle", 0, path) != 0);
        glitch::ConfigureOracleStep(tscfix::config::Int("experiments", "oracle_step", 0, path) != 0,
                                    tscfix::config::Int("experiments", "oracle_step_calibrate", 0, path) != 0,
                                    tscfix::config::Int("experiments", "oracle_step_threaded", 1, path));
    }
    g_cfg.logUi = tscfix::config::Int("debug", "log_ui", 0, path) != 0;
    g_cfg.crashLog = tscfix::config::Int("debug", "crash_log", 0, path) != 0;
    g_cfg.crashRecord = tscfix::config::Int("debug", "crash_record", 1, path) != 0;
    g_cfg.heapBudget = tscfix::config::Int("fixes", "engine_heap_budget", 1, path) != 0;
    g_cfg.autostartQuickDrive = tscfix::config::Int("automation", "autostart_quickdrive", 0, path) != 0;
    g_cfg.autostartDelayMs = tscfix::config::Int("automation", "autostart_delay_ms", 1000, path);
    char buf[4096];
    tscfix::config::Str("automation", "trigger", "", buf, sizeof buf, path);
    g_cfg.autostartTrigger = buf;
    tscfix::config::Str("automation", "sequence", "", buf, sizeof buf, path);
    g_cfg.autostartSequence = buf;
    if (g_cfg.autostartQuickDrive && (g_cfg.autostartTrigger.empty() || g_cfg.autostartSequence.empty())) {
        Log("config: autostart needs [automation] trigger and sequence; disabled");
        g_cfg.autostartQuickDrive = false;
    }
    g_cfg.profile = tscfix::config::Int("profile", "enable", 0, path) != 0;
    g_cfg.profileIntervalUs = tscfix::config::Int("profile", "interval_us", 1000, path);
    g_cfg.profileLua = g_cfg.profile && tscfix::config::Int("profile", "lua", 0, path) != 0;
    g_cfg.profileD3d = g_cfg.profile && tscfix::config::Int("profile", "d3d", 0, path) != 0;
    // Measured wins are on by default; set to 0 to turn one off.
    g_cfg.effectNoSaveState = tscfix::config::Int("fixes", "effect_nosavestate", 1, path) != 0;
    g_cfg.stateFilter = tscfix::config::Int("fixes", "state_filter", 0, path) != 0;
    g_cfg.physicsIndex = tscfix::config::Int("fixes", "physics_index", 1, path) != 0;
    g_cfg.scriptTiming = tscfix::config::Int("fixes", "script_timing", 1, path) != 0;
    g_cfg.scriptCallCache = tscfix::config::Int("fixes", "script_call_cache", 1, path) != 0;
    g_cfg.qiCache = tscfix::config::Int("fixes", "qi_cache", 1, path) != 0;
    g_cfg.fastStrings = tscfix::config::Int("fixes", "fast_strings", 0, path) != 0;
    g_cfg.fastNodeLookup = tscfix::config::Int("fixes", "fast_node_lookup", 0, path) != 0;
    g_cfg.preciseLoadClock = tscfix::config::Int("fixes", "precise_load_clock", 0, path) != 0;
    g_cfg.fiberTime = tscfix::config::Int("experiments", "fiber_time", 0, path) != 0;
    g_cfg.multicore = tscfix::config::Int("fixes", "multicore", 1, path) != 0;
    g_cfg.richPresence = tscfix::config::Int("fixes", "rich_presence_async", 1, path) != 0;
    g_cfg.collideableList = tscfix::config::Int("experiments", "collideable_list_field", 0, path);
    g_cfg.renderThread = tscfix::config::Int("render", "thread", 0, path);
    g_cfg.runtimeLock = tscfix::config::Int("render", "mt_lock", 1, path) != 0;
    if (tscfix::config::Int("experiments", "deferred_locks", 0, path)) d3d::SetDeferredLocks(true);
    if (tscfix::config::Int("experiments", "direct_nooverwrite", 0, path)) d3d::SetDirectNoOverwrite(true);
    if (tscfix::config::Int("experiments", "filter_states", 0, path)) rt::SetFilterStates(true);
    if (tscfix::config::Int("experiments", "park_keeps_state", 0, path)) rt::SetParkKeepsState(true);
    g_cfg.force9On12 = tscfix::config::Int("render", "force_9on12", 0, path) != 0;
    g_cfg.default9On12Args = tscfix::config::Int("render", "default_9on12_args", 0, path) != 0;
    g_cfg.swapEffect = tscfix::config::Int("render", "swap_effect", 0, path);
    rt::SetSplitCores(tscfix::config::Int("render", "split_cores", 0, path) != 0);

    // In-session A/B test of one feature ([ab] feature=..., period=frames).
    tscfix::config::Str("ab", "feature", "", buf, sizeof buf, path);
    int period = tscfix::config::Int("ab", "period", 150, path);
    g_cfg.shadowEvery = tscfix::config::Int("experiments", "shadow_every", 0, path);
    g_cfg.sceneryProbe = tscfix::config::Int("experiments", "scenery_probe", 0, path) != 0;
    g_cfg.lookupProbe = tscfix::config::Int("experiments", "lookup_probe", 0, path) != 0;
    g_cfg.shadowCull = tscfix::config::Int("experiments", "shadow_cull", 0, path) != 0;
    char texels[32];
    tscfix::config::Str("experiments", "shadow_cull_texels", "1.0", texels, sizeof texels, path);
    g_cfg.cull.texels = static_cast<float>(atof(texels));
    g_cfg.cull.minCascade = tscfix::config::Int("experiments", "shadow_cull_min_cascade", 1, path);
    g_cfg.cull.resolution = tscfix::config::Int("experiments", "shadow_cull_resolution", 1280, path);
    g_cfg.cull.matrixField = tscfix::config::Int("experiments", "caster_matrix_field", 0, path);
    g_cfg.cull.cascadeField = tscfix::config::Int("experiments", "caster_cascade_field", 0, path);
    g_cfg.speedLimits = tscfix::config::Int("experiments", "speed_limits", 0, path) != 0;
    g_cfg.speedLimitCaller = tscfix::config::Int("experiments", "speed_limit_caller", 0, path);
    // Several features may be listed (a,b,c): all are installed, and each A/B window tests the
    // one bench names (one game session, several measurements).
    for (char* next = buf; next && *next;) {
        char* f = next;
        next = strchr(f, ',');
        if (next) *next++ = 0;
        if (strcmp(f, "render_thread") == 0) ab::Configure(f, period, &rt::SetThreadedActive);
        else if (strcmp(f, "rt_desc_cache") == 0) ab::Configure(f, period, &rt::SetDescCacheActive);
        else if (strcmp(f, "shadow_skip") == 0) {
            if (g_cfg.shadowEvery < 2) g_cfg.shadowEvery = 2;
            g_cfg.abShadow = true;
            ab::Configure(f, period, &experiments::SetShadowSkip);
        } else if (strcmp(f, "shadow_split") == 0) {
            g_cfg.shadowStagger = true;  // installs the cascade hooks
            g_cfg.abStagger = true;
            ab::Configure(f, period, &experiments::SetShadowSplit);
        } else if (strcmp(f, "shadow_lod") == 0) {
            g_cfg.abShadowLod = true;
            ab::Configure(f, period, [](bool on) { experiments::SetShadowLod(on, g_cfg.shadowLod); });
        } else if (strcmp(f, "fork") == 0) {  // the forked render module (scenery) on its own thread or not
            ab::Configure(f, period, &overlap::SetForkActive);
        } else if (strcmp(f, "overlap") == 0) {
            g_cfg.overlap = g_cfg.abOverlap = true;
            ab::Configure(f, period, &overlap::SetActive);
#ifdef TSCFIX_RESEARCH_BINDER
        } else if (strcmp(f, "binder") == 0) {
            g_cfg.binder = g_cfg.abBinder = true;
            ab::Configure(f, period, &binder::SetSubstitute);
#endif
        } else if (strcmp(f, "deferred_locks") == 0) {
            ab::Configure(f, period, &d3d::SetDeferredLocks);
        } else if (strcmp(f, "overlap_state") == 0) {
            g_cfg.overlapState = g_cfg.abOverlapState = true;
            ab::Configure(f, period, &overlapstate::SetActive);
        } else if (strcmp(f, "shadow_threads") == 0) {  // one shadow thread (off) or two (on)
            g_cfg.mapSplit = g_cfg.abMapSplit = true;
            ab::Configure(f, period, &experiments::SetMapSplit);
        } else if (strcmp(f, "shadow_worker") == 0) {
            g_cfg.shadowWorker = g_cfg.abShadowWorker = true;
            ab::Configure(f, period, &experiments::SetShadowWorker);
        } else if (strcmp(f, "shadow_cascades") == 0) {
            g_cfg.shadowStagger = true;
            g_cfg.abStagger = true;
            ab::Configure(f, period, &experiments::SetCascadeStagger);
        } else if (strcmp(f, "shadow_cull") == 0) {
            g_cfg.abShadowCull = true;
            ab::Configure(f, period, &shadowcull::SetActive);
        } else if (strcmp(f, "state_filter") == 0) {
            g_cfg.stateFilter = true;
            ab::Configure(f, period, &d3d::SetFilterActive);
        } else if (strcmp(f, "effect_nosavestate") == 0) {
            g_cfg.effectNoSaveState = true;
            ab::Configure(f, period, &d3d::SetNoSaveStateActive);
        } else if (strcmp(f, "physics_index") == 0) {
            g_cfg.abPhysics = true;
            ab::Configure(f, period, &physicsfix::SetActive);
        } else if (strcmp(f, "camera_rest") == 0) {
            g_cfg.abCameraRest = true;
            ab::Configure(f, period, &physicsfix::SetCameraRestActive);
        } else if (strcmp(f, "script_timing") == 0) {
            g_cfg.abScriptTiming = true;
            ab::Configure(f, period, &scripttiming::SetActive);
#if defined(TSCFIX_QICACHE) && defined(TSCFIX_SCRIPTCALL)
        } else if (strcmp(f, "lookups") == 0) {  // both lookup caches together
            g_cfg.abQiCache = g_cfg.abScriptCallCache = true;
            ab::Configure(f, period, [](bool on) { qicache::SetActive(on), scriptcall::SetActive(on); });
#endif
        } else if (strcmp(f, "dispatch_cache") == 0) {  // replace (on) vs the engine's search (off)
            ab::Configure(f, period, [](bool on) { entitysearch::SetMode(on ? 2 : 0); });
#ifdef TSCFIX_QICACHE
        } else if (strcmp(f, "qi_cache") == 0) {
            g_cfg.abQiCache = true;
            ab::Configure(f, period, &qicache::SetActive);
#endif
#ifdef TSCFIX_SCRIPTCALL
        } else if (strcmp(f, "script_call_cache") == 0) {
            g_cfg.abScriptCallCache = true;
            ab::Configure(f, period, &scriptcall::SetActive);
#endif
        } else if (strcmp(f, "picture") == 0) {
            ab::Configure(f, period, &postfx::SetActive);
        } else if (strcmp(f, "blend_over_time") == 0) {  // the blend over time on (on) or the plain last pass (off)
            g_cfg.abBlendOverTime = true;
            ab::Configure(f, period, &postfx::SetBlendActive);
        } else if (strcmp(f, "steady_glow") == 0) {  // the steady lamp glow (on) or the game's own glow mask (off)
            g_cfg.abSteadyGlow = true;
            ab::Configure(f, period, &postfx::SetSteadyGlowActive);
        } else if (strcmp(f, "glow_gain") == 0) {  // the limited gain of the lamp glow (on) or the game's blur shaders (off)
            g_cfg.abGlowGain = true;
            ab::Configure(f, period, &postfx::SetGlowGainActive);
        } else if (strcmp(f, "override_lock_all") == 0) {
            ab::Configure(f, period, &pipeline::SetOverrideLockAll);
        } else if (strcmp(f, "node_set_skip") == 0) {
            g_cfg.abNodeSetSkip = true;
            ab::Configure(f, period, &nodeskip::SetActive);
        } else if (strcmp(f, "better_resolve") == 0) {
            ab::Configure(f, period, &resolve::SetActive);
        } else if (strcmp(f, "function_lookup") == 0) {
            g_cfg.abFunctionLookup = true;
            ab::Configure(f, period, &funclookup::SetActive);
        } else if (strcmp(f, "control_lookup") == 0) {
            g_cfg.abControlLookup = true;
            ab::Configure(f, period, &controllookup::SetActive);
        } else if (strcmp(f, "node_lookup") == 0) {
            g_cfg.abNodeLookup = true;
            ab::Configure(f, period, &nodelookup::SetActive);
        } else if (strcmp(f, "load_clock") == 0) {  // the engine's tick count (off) or the precise slice clock (on)
            g_cfg.abLoadClock = true;
            ab::Configure(f, period, &loadclock::SetActive);
        } else if (strcmp(f, "fast_strings") == 0) {
            g_cfg.abFastStrings = true;
            ab::Configure(f, period, &fastpaths::SetActive);
        } else if (strncmp(f, "switch:", 7) == 0) {  // a registered switch (glitch::RegisterSwitch), by name
            static char name[64];
            strcpy_s(name, f + 7);
            ab::Configure(f, period, [](bool on) {
                if (glitch::Switch s = glitch::FindSwitch(name)) s(on);
            });
        } else if (strcmp(f, "null") == 0) {  // A/A calibration: switches nothing
            ab::Configure(f, period, [](bool) {});
        } else if (strcmp(f, "prefetch") == 0) {
            g_cfg.patches = true;
            g_cfg.abPatches = true;
            ab::Configure(f, period, &patches::SetActive);
        } else if (f[0]) Log("ab: unknown feature '%s'", f);
    }
    if (tscfix::config::Int("experiments", "shadow_stagger", 0, path)) g_cfg.shadowStagger = true;
    if (tscfix::config::Int("experiments", "shadow_worker", 0, path)) g_cfg.shadowWorker = true;
    if (tscfix::config::Int("threading", "shadow_threads", tscfix::config::Int("experiments", "shadow_threads", 2, path), path) >= 2)  // (default: two shadow threads)
        g_cfg.mapSplit = true;
    g_cfg.mapSplitMap = tscfix::config::Int("experiments", "shadow_threads_map", -1, path);
    g_cfg.overlapCensus = tscfix::config::Int("experiments", "overlap_census", 0, path) != 0;
    if (tscfix::config::Int("experiments", "overlap", 0, path)) g_cfg.overlap = true;
    if (tscfix::config::Int("experiments", "overlap_state", 0, path)) g_cfg.overlapState = true;
    if (tscfix::config::Int("research", "binder", 0, path) || tscfix::config::Int("experiments", "binder", 0, path))
        g_cfg.binder = true;
    tscfix::config::Str("experiments", "overlap_modules", "", g_cfg.overlapModules, sizeof g_cfg.overlapModules, path);
    g_cfg.shadowTrace = tscfix::config::Int("experiments", "shadow_trace", 0, path) != 0;
    g_cfg.shadowLod = tscfix::config::Int("experiments", "shadow_lod", 0, path);
    if (g_cfg.abShadowLod && g_cfg.shadowLod <= 0) g_cfg.shadowLod = 1;
    g_cfg.lodBiasOffset = tscfix::config::Int("experiments", "lod_bias_offset", 0, path);
    int traceCascade = tscfix::config::Int("research", "trace_cascade", -1, path);
    if (traceCascade >= 0) {
        g_cfg.shadowTrace = true;  // installs the cascade hooks without changing behaviour
        experiments::SetResearchTrace(traceCascade, tscfix::config::Int("research", "trace_frame", 600, path));
    }
    g_cfg.cascadeOffset = tscfix::config::Int("experiments", "cascade_matrix_offset", 0, path);
    g_cfg.cascadeStride = tscfix::config::Int("experiments", "cascade_matrix_stride", 0, path);
    g_cfg.cascadeCount = tscfix::config::Int("experiments", "cascade_count", 0, path);
    Log("config: fixes effect_nosavestate=%d physics_index=%d script_timing=%d multicore=%d state_filter=%d "
        "render thread=%d", g_cfg.effectNoSaveState, g_cfg.physicsIndex, g_cfg.scriptTiming, g_cfg.multicore,
        g_cfg.stateFilter, g_cfg.renderThread);
    Log("config: log_ui=%d autostart_quickdrive=%d autostart_delay_ms=%d profile=%d interval_us=%d",
        g_cfg.logUi, g_cfg.autostartQuickDrive, g_cfg.autostartDelayMs, g_cfg.profile, g_cfg.profileIntervalUs);
}

// --- Menu bypass ------------------------------------------------------------------
// The menu starts a Quick Drive by reading the saved selection and then calling
// StartQuickDrive. We replay that on the UI thread once the main menu is up, by
// posting a private message to the game window.
constexpr UINT WM_TSCFIX_AUTOSTART = WM_APP + 0x7F;
void* g_uiSelf = nullptr;
void* g_uiMovie = nullptr;
WNDPROC g_origWndProc = nullptr;
HWND g_gameWnd = nullptr;
bool g_autostartArmed = false;

// One menu call, with no argument or with one string argument (as the menu passes a route id or
// a location name).
void CallUi(const char* method, const char* argument = nullptr) {
    if (auto orig = OriginalFor(g_uiSelf)) {
        if (!argument) {
            Log("autostart: %s", method);
            orig(g_uiSelf, g_uiMovie, method, nullptr, 0);
            return;
        }
        Log("autostart: %s(\"%s\")", method, argument);
        GfxValue value{};
        value.type = 6;  // a string that the caller owns
        value.v.s = argument;
        orig(g_uiSelf, g_uiMovie, method, &value, 1);
    }
}

// The menu call names are configuration ([automation] sequence, comma separated),
// recorded locally with [debug] log_ui; they are not part of this source. An entry "name=text"
// makes the call with that text as its one string argument: this is how a sequence selects the
// route, the start and the destination before it starts the Quick Drive.
void RunAutostart() {
    std::string seq = g_cfg.autostartSequence;
    size_t pos = 0;
    while (pos <= seq.size()) {
        size_t comma = seq.find(',', pos);
        std::string m = seq.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t eq = m.find('=');
        if (m.empty()) {
        } else if (eq == std::string::npos) {
            CallUi(m.c_str());
        } else {
            std::string argument = m.substr(eq + 1);
            CallUi(m.substr(0, eq).c_str(), argument.c_str());
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TSCFIX_AUTOSTART) {
        RunAutostart();
        return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameA(hwnd, cls, sizeof cls) && strcmp(cls, "RailWorks") == 0) {
        g_gameWnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI AutostartTimer(void*) {
    Sleep(g_cfg.autostartDelayMs);
    PostMessageW(g_gameWnd, WM_TSCFIX_AUTOSTART, 0, 0);
    return 0;
}

// Called on the UI thread when the main menu has finished building.
void ArmAutostart(void* self, void* movie) {
    if (g_autostartArmed) return;
    g_autostartArmed = true;
    g_uiSelf = self;
    g_uiMovie = movie;
    EnumWindows(FindGameWindow, 0);
    if (!g_gameWnd) {
        Log("autostart: game window not found");
        return;
    }
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWndProc)));
    Log("autostart: armed, starting Quick Drive in %d ms", g_cfg.autostartDelayMs);
    if (HANDLE t = CreateThread(nullptr, 0, AutostartTimer, nullptr, 0, nullptr)) CloseHandle(t);
}

void HookCallback(void* self, void* movie, const char* method, const GfxValue* args, unsigned argc) {
    if (g_cfg.logUi) {
        std::string line;
        for (unsigned i = 0; i < argc && i < 12; ++i) {
            if (i) line += ", ";
            line += Describe(args[i]);
        }
        Log("ui-call %s(%s) movie=%p", method ? method : "?", line.c_str(), movie);
    }
    if (auto orig = OriginalFor(self)) orig(self, movie, method, args, argc);
    if (g_cfg.autostartQuickDrive && method && g_cfg.autostartTrigger == method) ArmAutostart(self, movie);
}

bool PatchPointer(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void InstallUiHooks(HMODULE engine) {
    auto tables = rtti::FindVTablesDerivedFrom(engine, kExternalInterfaceType);
    Log("found %zu ExternalInterface implementation(s)", tables.size());
    for (void** vt : tables) {
        if (g_tableCount == _countof(g_tables)) break;
        auto orig = reinterpret_cast<CallbackFn>(vt[kCallbackSlot]);
        g_tables[g_tableCount] = {vt, orig};
        if (PatchPointer(&vt[kCallbackSlot], reinterpret_cast<void*>(&HookCallback))) {
            ++g_tableCount;
            Log("hooked ExternalInterface::Callback in table #%d", g_tableCount);
        }
    }
}

// Direct3D must be hooked before the device appears; the DX12 build creates it within
// ~100 ms of start-up. Installed from DllMain when the DLLs are already there, else as
// soon as the start-up thread sees them.
bool g_d3dPending = false;

void InstallD3dIfReady() {
    if (!g_d3dPending || !GetModuleHandleA("d3d9.dll") || !GetModuleHandleA("d3dx9_43.dll")) return;
    g_d3dPending = false;
    d3d::Options o;
    o.census = g_cfg.profileD3d;
    o.effectNoSaveState = g_cfg.effectNoSaveState;
    o.stateFilter = g_cfg.stateFilter;
    o.lodFade = tscfix::config::Int("experiments", "lod_fade", 0, g_cfg.iniPath);
    o.queryTrack = tscfix::config::Int("experiments", "query_track", 0, g_cfg.iniPath) != 0;
    o.samplerProbe = tscfix::config::Int("experiments", "sampler_probe", 0, g_cfg.iniPath) != 0;
    {   // ([experiments] flicker_key is the same switch for test runs)
        int key = tscfix::config::Int("capture", "flicker_key", 0, g_cfg.iniPath);
        if (!key) key = tscfix::config::Int("experiments", "flicker_key", 0, g_cfg.iniPath);
        int frames = tscfix::config::Int("experiments", "flicker_frames", 0, g_cfg.iniPath);
        flickercap::Configure(key, frames ? frames : tscfix::config::Int("capture", "flicker_frames", 60, g_cfg.iniPath));
        flickercap::SetCrop(tscfix::config::Int("experiments", "flicker_crop_x", 0, g_cfg.iniPath), tscfix::config::Int("experiments", "flicker_crop_y", 0, g_cfg.iniPath),
                            tscfix::config::Int("experiments", "flicker_crop_w", 0, g_cfg.iniPath), tscfix::config::Int("experiments", "flicker_crop_h", 0, g_cfg.iniPath));
    }
    {
        resolve::Options r;
        r.kernel = tscfix::config::Int("graphics", "better_resolve", 1, g_cfg.iniPath);  // (default: Lanczos)
        r.linear = tscfix::config::Int("graphics", "better_resolve_linear", 1, g_cfg.iniPath) != 0;
        r.anisotropy = tscfix::config::Int("graphics", "anisotropy", 0, g_cfg.iniPath);
        r.compare = tscfix::config::Int("experiments", "resolve_compare", 0, g_cfg.iniPath) != 0;
        postfx::Options px;
        px.fxaa = tscfix::config::Int("graphics", "fxaa", -1, g_cfg.iniPath);
        px.depthOfField = tscfix::config::Int("graphics", "depth_of_field", 1, g_cfg.iniPath) != 0;
        px.compare = tscfix::config::Int("experiments", "picture_compare", 0, g_cfg.iniPath) != 0;
        px.film = tscfix::config::Int("graphics", "film", 1, g_cfg.iniPath);
        px.lampFade = tscfix::config::Int("graphics", "lamp_fade", 1, g_cfg.iniPath) != 0;
        px.lampFadeFrom = tscfix::config::Int("graphics", "lamp_fade_from_percent", 25, g_cfg.iniPath) / 100.0f;
        px.lampFadeTo = tscfix::config::Int("graphics", "lamp_fade_to_percent", 70, g_cfg.iniPath) / 100.0f;
        px.noLampLight = tscfix::config::Int("experiments", "no_lamp_light", 0, g_cfg.iniPath);
        // Blend over time (off by default). Under A/B it is installed and stays off until a window switches it.
        px.blendOverTime = tscfix::config::Int("graphics", "blend_over_time", 0, g_cfg.iniPath) != 0 || g_cfg.abBlendOverTime;
        px.blendWeight = tscfix::config::Int("experiments", "taa_weight_percent", 75, g_cfg.iniPath) / 100.0f;
        px.stillMove = tscfix::config::Int("experiments", "taa_still_move_um", 500, g_cfg.iniPath) / 1e6f;
        px.stillTurn = tscfix::config::Int("experiments", "taa_still_turn_ppm", 100, g_cfg.iniPath) / 1e6f;
        px.blendLog = tscfix::config::Int("experiments", "taa_log", 0, g_cfg.iniPath) != 0;
        px.showHistory = tscfix::config::Int("experiments", "taa_show_history", 0, g_cfg.iniPath) != 0;
        // Steady glow (off by default). Under A/B it is installed and stays off until a window switches it.
        px.steadyGlow = tscfix::config::Int("graphics", "steady_glow", 0, g_cfg.iniPath);
        if (g_cfg.abSteadyGlow && px.steadyGlow == 0) px.steadyGlow = 1;
        px.steadyGlowKeep = tscfix::config::Int("experiments", "steady_glow_keep_percent", 60, g_cfg.iniPath) / 100.0f;
        px.glowLog = tscfix::config::Int("experiments", "glow_log", 0, g_cfg.iniPath) != 0;
        // Glow gain (100 = the game's behaviour). Under A/B with no value given it is 20; it stays off until a window switches it.
        int glowGain = tscfix::config::Int("graphics", "glow_far_gain_percent", 100, g_cfg.iniPath);
        if (g_cfg.abGlowGain && (glowGain <= 0 || glowGain >= 100)) glowGain = 20;
        px.glowFarGain = glowGain / 100.0f;
        if (g_cfg.abGlowGain) postfx::SetGlowGainActive(false);
        if (g_cfg.abSteadyGlow) postfx::SetSteadyGlowActive(false);
        if (g_cfg.abBlendOverTime) postfx::SetBlendActive(false);
        postfx::Configure(px);
        r.hooksWanted = postfx::Enabled();
        resolve::Configure(r);
    }
    o.lodFadeTest = tscfix::config::Int("experiments", "lod_fade_test", 0, g_cfg.iniPath);
    o.renderThread = g_cfg.renderThread;
    o.runtimeLock = g_cfg.runtimeLock;
    o.force9On12 = g_cfg.force9On12;
    o.default9On12Args = g_cfg.default9On12Args;
    o.swapEffect = g_cfg.swapEffect;
    o.crashLog = g_cfg.crashLog;
    d3d::Install(o);
}

DWORD WINAPI Startup(void*) {
    const char* engines[] = {"GameManagerVC64.dll", "GameManagerDX12_VC64.dll"};
    for (int tries = 0; tries < 60000; ++tries) {
        InstallD3dIfReady();
        for (const char* name : engines) {
            if (HMODULE h = GetModuleHandleA(name)) {
                Log("engine module %s loaded", name);
                InstallUiHooks(h);
                if (g_cfg.profileLua) luaprof::Install(h, g_mainThreadId);
                if (g_cfg.shadowEvery >= 2 && experiments::InstallShadowSkip(h, g_cfg.shadowEvery) && !g_cfg.abShadow)
                    experiments::SetShadowSkip(true);
                if (g_cfg.sceneryProbe) experiments::InstallSceneryProbe(h);
                if ((g_cfg.overlapCensus || g_cfg.overlap) && overlap::Install(h, g_cfg.overlapCensus) && g_cfg.overlap &&
                    overlap::InstallOverlap(g_cfg.overlapModules) &&
                    (overlap::SetDedicatedCore(tscfix::config::Int("experiments", "overlap_core", 0, g_cfg.iniPath)),
                     !g_cfg.abOverlap))
                    overlap::SetActive(true);
                if ((g_cfg.overlapState || g_cfg.overlap) && overlapstate::Install(h) && !g_cfg.abOverlapState)
                    overlapstate::SetActive(true);
#ifdef TSCFIX_RESEARCH_BINDER
                if (g_cfg.overlapState || g_cfg.overlap) pipeline::Install(h);
                // The overlap needs the per-thread binder in both A/B halves (single-threaded it is
                // pixel-identical), so it is on whenever the overlap is configured.
                if ((g_cfg.binder || g_cfg.overlap) && binder::Install(h) &&
                    (g_cfg.overlap || (g_cfg.binder && !g_cfg.abBinder)))
                    binder::SetSubstitute(true);
#endif
                if (glitch::Enabled()) glitch::ConfigureVariants(g_glitchVariants, g_glitchSlice);
                if (g_cfg.lookupProbe) lookup::InstallProbe(h);
                if ((g_cfg.physicsIndex || g_cfg.abPhysics) && physicsfix::Install(h, g_cfg.collideableList) &&
                    !g_cfg.abPhysics)
                    physicsfix::SetActive(true);
                if (tscfix::config::Int("fixes", "brake_hold", 1, g_cfg.iniPath) != 0) physicsfix::InstallBrakeHold(h);
                {
                    int sp = tscfix::config::Int("graphics", "cab_camera_spring_percent", 100, g_cfg.iniPath);
                    int dp = tscfix::config::Int("graphics", "cab_camera_damping_percent", 100, g_cfg.iniPath);
                    bool on = tscfix::config::Int("graphics", "cab_camera_spring", 0, g_cfg.iniPath) != 0;  // default: no spring (no wobble)
                    bool watch = tscfix::config::Int("experiments", "camera_watch", 0, g_cfg.iniPath) != 0 ||
                                 tscfix::config::Int("experiments", "consist_watch", 0, g_cfg.iniPath) != 0;
                    if (sp != 100 || dp != 100 || !on || watch) physicsfix::InstallCabCamera(h, sp, dp, on, watch);
                }
                // [fixes] camera_rest (default 1): the standing cab camera keeps its transform bit for bit.
                if ((tscfix::config::Int("fixes", "camera_rest", 1, g_cfg.iniPath) != 0 || g_cfg.abCameraRest) &&
                    physicsfix::InstallCameraRest(h, tscfix::config::Int("fixes", "camera_rest_um", 300, g_cfg.iniPath),
                                                  tscfix::config::Int("fixes", "camera_rest_turn_ppm", 20, g_cfg.iniPath),
                                                  tscfix::config::Int("experiments", "camera_rest_log", 0, g_cfg.iniPath) != 0) &&
                    !g_cfg.abCameraRest)
                    physicsfix::SetCameraRestActive(true);
                if (tscfix::config::Int("experiments", "sched_watch", 0, g_cfg.iniPath) && schedwatch::Install(h) &&
                    tscfix::config::Int("experiments", "vehicle_watch", 0, g_cfg.iniPath))
                    schedwatch::InstallVehicles(h);
                if ((g_cfg.scriptTiming || g_cfg.abScriptTiming) && scripttiming::Install(h) && !g_cfg.abScriptTiming)
                    scripttiming::SetActive(true);
#ifdef TSCFIX_SCRIPTCALL
                if ((g_cfg.scriptCallCache || g_cfg.abScriptCallCache) && scriptcall::Install(h) && !g_cfg.abScriptCallCache)
                    scriptcall::SetActive(true);
#ifdef TSCFIX_SCRIPTCALL_CENSUS
                if (tscfix::config::Int("experiments", "script_call_census", 0, g_cfg.iniPath)) scriptcall::InstallCensus(h);
                if (tscfix::config::Int("experiments", "script_result_census", 0, g_cfg.iniPath)) scriptcall::InstallResultCensus(h);
#endif
#endif
#if 1
                {  // [experiments] dispatch_cache: 1 verify, 2 replace (also what the on/off test installs)
                    int mode = tscfix::config::Int("experiments", "dispatch_cache", 0, g_cfg.iniPath);
                    if (!mode && tscfix::config::Int("fixes", "entity_search_cache", 1, g_cfg.iniPath)) mode = 2;
                {
                    // (before entitysearch: that one hooks the same function, and the fingerprint reads its first bytes)
                    // [fixes] fast_function_lookup: 0 off, 1 on. [experiments] function_lookup_verify=1 compares only.
                    bool verify = tscfix::config::Int("experiments", "function_lookup_verify", 0, g_cfg.iniPath) != 0;
                    bool on = tscfix::config::Int("fixes", "fast_function_lookup", 0, g_cfg.iniPath) != 0;
                    if ((on || g_cfg.abFunctionLookup || verify) && funclookup::Install(h, verify) && !g_cfg.abFunctionLookup)
                        funclookup::SetActive(true);
                }
                    if (mode) entitysearch::Install(h, mode);
                }
                {
                    // [fixes] node_set_skip=1 skips; [experiments] node_set_skip_verify=1 only checks the verdicts.
                    // (after entitysearch and scriptcall: it uses the hooks of both)
                    int mode = tscfix::config::Int("fixes", "node_set_skip", 1, g_cfg.iniPath) || g_cfg.abNodeSetSkip ? 2 : 0;
                    if (!mode && tscfix::config::Int("experiments", "node_set_skip_verify", 0, g_cfg.iniPath)) mode = 1;
                    if (mode) nodeskip::Install(h, mode);
                    nodeskip::SetCensus(tscfix::config::Int("experiments", "node_census", 0, g_cfg.iniPath) != 0);
                }
                {
                    // [fixes] fast_control_lookup=1 uses remembered answers (default 1); [experiments] control_lookup_verify=1
                    // only compares them with the engine's lookup. It needs the script call hook ([fixes] script_call_cache).
                    int mode = tscfix::config::Int("fixes", "fast_control_lookup", 1, g_cfg.iniPath) || g_cfg.abControlLookup ? 2 : 0;
                    // The verify mode wins over the default, so a check run needs no other switch.
                    if (tscfix::config::Int("experiments", "control_lookup_verify", 0, g_cfg.iniPath)) mode = 1;
                    if (mode) controllookup::Install(h, mode);
                }
#endif
#ifdef TSCFIX_QICACHE
                if ((g_cfg.qiCache || g_cfg.abQiCache) &&
                    qicache::Install(h, tscfix::config::Int("experiments", "qi_cache_verify", 0, g_cfg.iniPath) != 0) &&
                    !g_cfg.abQiCache)
                    qicache::SetActive(true);
#endif
                if ((g_cfg.fastStrings || g_cfg.abFastStrings) && fastpaths::Install(h) && !g_cfg.abFastStrings)
                    fastpaths::SetActive(true);
                if (tscfix::config::Int("fixes", "safe_counts", 1, g_cfg.iniPath)) safecounts::Install(h);
                if (tscfix::config::Int("experiments", "refcount_census", 0, g_cfg.iniPath)) refcensus::Install(h);
                if (int probe = tscfix::config::Int("experiments", "lod_probe", 0, g_cfg.iniPath)) lodprobe::Install(h, probe);
                if (tscfix::config::Int("experiments", "dynheap_probe", 0, g_cfg.iniPath)) dynheap::Install(h);
                {
                    bool verify = tscfix::config::Int("experiments", "node_lookup_verify", 0, g_cfg.iniPath) != 0;
                    if ((g_cfg.fastNodeLookup || g_cfg.abNodeLookup || verify) && nodelookup::Install(h, verify) &&
                        !g_cfg.abNodeLookup)
                        nodelookup::SetActive(true);
                }
                {
                    // (one install for both: the instrument hooks a function that the clock's install reads)
                    bool clock = g_cfg.preciseLoadClock || g_cfg.abLoadClock;
                    if ((clock || g_cfg.fiberTime) && loadclock::Install(h, clock, g_cfg.fiberTime) && g_cfg.preciseLoadClock &&
                        !g_cfg.abLoadClock)
                        loadclock::SetActive(true);
                }
                if (g_cfg.shadowCull || g_cfg.abShadowCull) {
                    g_cfg.cull.cascadeCount = g_cfg.cascadeCount;
                    if (shadowcull::Install(h, g_cfg.cull) && !g_cfg.abShadowCull) shadowcull::SetActive(true);
                }
                if (g_cfg.patches && patches::Install(h, g_cfg.iniPath) && !g_cfg.abPatches) patches::SetActive(true);
                if (g_cfg.speedLimits) experiments::InstallSpeedLimits(h, g_cfg.speedLimitCaller);
                if ((g_cfg.shadowStagger || g_cfg.shadowTrace || g_cfg.shadowLod || g_cfg.sceneryProbe || g_cfg.shadowWorker || g_cfg.overlap ||
                     g_cfg.overlapState || g_cfg.binder) &&
                    experiments::InstallCascadeStagger(h, g_cfg.cascadeOffset, g_cfg.cascadeStride, g_cfg.cascadeCount)) {
                    if (g_cfg.shadowStagger && !g_cfg.abStagger) experiments::SetCascadeStagger(true);
                    if (g_cfg.shadowWorker && experiments::InstallShadowWorker() && !g_cfg.abShadowWorker)
                        experiments::SetShadowWorker(true);
                    if (g_cfg.mapSplit && g_cfg.overlap && experiments::InstallMapSplit(h, g_cfg.mapSplitMap) && !g_cfg.abMapSplit)
                        experiments::SetMapSplit(true);
                    if (g_cfg.shadowLod && experiments::InstallShadowLod(h, g_cfg.lodBiasOffset) && !g_cfg.abShadowLod)
                        experiments::SetShadowLod(true, g_cfg.shadowLod);
                }
                return 0;
            }
        }
        Sleep(5);  // short: Direct3D must be hooked before the device appears
    }
    Log("engine module not found; nothing hooked");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        LogOpen();
        g_mainThreadId = GetCurrentThreadId();
        Log("tscfix loaded into pid %lu on thread %lu", GetCurrentProcessId(), g_mainThreadId);
        LoadConfig();
        trace::Start(static_cast<uint32_t>(tscfix::config::Int("experiments", "trace_mask", 0, g_cfg.iniPath)));
        if (g_cfg.heapBudget && !InstallHeapBudget()) Log("config: engine_heap_budget: heap setup not found; the budget stays 2 GB");
        if (g_cfg.crashLog || g_cfg.crashRecord) {
            AddVectoredExceptionHandler(1, CrashHandler);
            if (!InstallErrorMessage()) Log("config: crash_record: the crash dialog keeps the game's own text");
            if (tscfix::config::Int("experiments", "crash_dialog_test", 0, g_cfg.iniPath)) LogSampleDialogs();
            g_memoryGuardGb = tscfix::config::Int("experiments", "memory_guard_gb", 0, g_cfg.iniPath);
            CloseHandle(CreateThread(nullptr, 0, &MemoryLogMain, nullptr, 0, nullptr));
        }
        if (g_cfg.multicore && !EnableMulticore()) Log("config: multicore: engine option lookup not found; not applied");
        if (g_cfg.richPresence) richpresence::Start();
        g_d3dPending = g_cfg.profileD3d || g_cfg.effectNoSaveState || g_cfg.stateFilter || g_cfg.renderThread ||
                       g_cfg.force9On12 || g_cfg.default9On12Args || g_cfg.swapEffect || g_cfg.crashLog;
        InstallD3dIfReady();
        framelog::Init();
        if (g_cfg.profile) profiler::Start(g_mainThreadId, g_cfg.profileIntervalUs);
        if (HANDLE t = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
