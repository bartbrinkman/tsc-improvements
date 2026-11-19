#include "overlap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <intrin.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "glitch.h"
#include "log.h"
#include "d3dlayer.h"
#include "offload.h"
#include "profiler.h"
#include "rtdevice.h"
#include "sites.h"
#include "status.h"

#if __has_include("tracer.h")  // the engine-specific parts
#include "tracer.h"
#include "trace.h"
#define TSCFIX_RESEARCH 1
#endif

namespace tscfix::overlap {
namespace {

// Scheduler phase runner (game version v79.0a): walks the registered modules of a phase
// and dispatches each through a one-line virtual-call thunk (`mov edx, ebp; mov rcx, rsi;
// call thunk`). The call site is found by that instruction pattern inside the function.
constexpr sites::Fingerprint kScheduler = {0x677951c87a13a94dull, 16};
constexpr size_t kSchedulerScan = 0x600;
constexpr uint8_t kCallPattern[] = {0x8B, 0xD5, 0x48, 0x8B, 0xCE, 0xE8};
constexpr uint8_t kThunk[] = {0x48, 0x8B, 0x01, 0xFF, 0x60, 0x08};  // mov rax,[rcx]; jmp [rax+8]

using ModuleFn = void (*)(void* module, int arg);

bool g_census = false;

struct ModuleStat {
    void** vtable;
    int arg;
    int order;        // position within its phase, last frame
    uint64_t calls;
    int64_t ticks;
};
ModuleStat g_stats[64];
int g_statCount = 0;
int g_position = 0;  // modules dispatched since the last Present
uint64_t g_frames = 0;

// Class name from MSVC x64 run-time type information (vtable[-1] -> complete object locator).
const char* ClassName(void** vtable) {
    __try {
        auto* col = static_cast<const int32_t*>(vtable[-1]);
        if (col[0] != 1) return "?";
        auto* image = reinterpret_cast<const uint8_t*>(col) - col[5];
        return reinterpret_cast<const char*>(image + col[3] + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

bool g_traceModules = false;  // research: trace each render module up to the scenery manager

// --- overlap ------------------------------------------------------------------------------------
// With overlap active, the shadow-map module of the render phase runs on a worker. The game
// thread waits only until the worker has finished the cascade setup (the shadow camera, which
// later modules read), then dispatches the following modules: those named in the allow-list
// run alongside the worker and record into the side buffer; the first one that is not on the
// list, or any device call that needs the queue, joins (waits for the worker, appends).
constexpr int kRenderPhase = 32;
offload::Worker g_worker;
// Fork: one render module (named in [experiments] overlap_fork) runs on a second worker while
// the game thread goes on with the next modules; its commands keep their place in the stream.
offload::Worker g_forkWorker;
std::string g_forkName;
// (glitch isolation) [experiments] glitch_skip_modules=<class name parts, comma separated>: these
// render modules are not drawn, so they can neither glitch nor hide another module's glitch
// (an allow-list removed frame setup and composition too: a black screen). Every render-phase
// module is logged once, in order, to choose from.
std::vector<std::string> g_skipModules;
bool ModuleSkipped(void** vt) {
    const char* name = ClassName(vt);
    static std::vector<std::string> seen;  // render thread / game thread, logging only
    if (seen.size() < 64 && std::find(seen.begin(), seen.end(), name) == seen.end()) {
        seen.push_back(name);
        Log("overlap: render module %zu: %s", seen.size(), name);
    }
    for (const std::string& m : g_skipModules)
        if (strstr(name, m.c_str())) return true;
    return false;
}
volatile bool g_forked = false;
// Early fork ([experiments] overlap_fork_early, default 1): the forked module starts when the
// overlap does, beside the modules dispatched before it, instead of at its own place in the
// dispatch order. Its commands still take that place in the stream (rt::ForkAhead: the game
// thread moves behind them when the dispatch reaches the module). Modules after it wait for it
// (they draw what it queued), not for the shadow pass.
bool g_forkEarly = true;
volatile bool g_forkOff = false;  // switched off for an A/B half (see SetForkActive)
volatile bool g_forkFailed = false;  // the forked module threw on its worker: no more forks this session
void* g_forkModule = nullptr;     // the forked module, learned at its dispatch (launched early from the next frame on)
bool g_forkAhead = false;         // this overlap's fork was launched ahead; the game thread has not moved behind it yet
bool g_forkSlotPassed = false;    // this overlap: the dispatch is past the forked module
// ([experiments] overlap_fork_after=<module name>, off by default) The forked module starts ahead
// only after the module named here has run in this overlap. Measured with scenery_leftovers: a
// copy taken before the terrain module holds older matrices, but none of them is marked valid,
// so the start point does not change what the scenery draws.
std::string g_forkAfter;
bool g_forkGate = false, g_lastWasGate = false;
bool g_forkRanThisFrame = false;  // launched early this frame: its own dispatch is skipped
uint32_t g_forkEarlyCount = 0;
int64_t g_forkBusyTicks = 0;      // fork worker, per report
ForkHooks g_forkHooks{};
struct ForkCall {
    void* module;
    int arg;
} g_forkCall;
uint32_t g_forkCount = 0;
LARGE_INTEGER g_forkWaitTicks{};

uintptr_t ForkMain(void*) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (g_forkHooks.enter) g_forkHooks.enter();
    void** vt = *static_cast<void***>(g_forkCall.module);
    TSCFIX_TRACE(trace::kOverlap, trace::kForkJobBegin, reinterpret_cast<uint64_t>(vt));
    // The engine throws C++ exceptions and has no catch frame on this thread: one that escapes
    // would end the process. It is caught here, the module stays on the game thread from the
    // next frame on, and the frame goes on (the crash handler has logged type and stack).
    try {
        reinterpret_cast<void (*)(void*, int)>(vt[1])(g_forkCall.module, g_forkCall.arg);
    } catch (...) {
        g_forkFailed = true;
        Log("overlap: the forked module threw an exception on its worker; it runs on the game thread from now on");
    }
    TSCFIX_TRACE(trace::kOverlap, trace::kForkJobEnd, reinterpret_cast<uint64_t>(vt));
    if (g_forkHooks.leave) g_forkHooks.leave();
    rt::SegmentDone();
    QueryPerformanceCounter(&t1);
    g_forkBusyTicks += t1.QuadPart - t0.QuadPart;
    return 0;
}
volatile bool g_overlapOn = false;
void (*g_beforeShadow)() = nullptr;  // see SetBeforeShadowHook
bool g_overlapping = false;
volatile bool g_setupDone = false;
std::vector<std::string> g_allowed;  // class-name substrings
std::vector<std::string> g_deferNames;  // allow-list entries starting with '>': run after the join
struct DeferredCall {
    void* module;
    int arg;
};
std::vector<DeferredCall> g_deferred;
void** g_shadowVt = nullptr;
void* g_shadowModule = nullptr;
struct VtVerdict { void** vt; bool allowed; };
VtVerdict g_verdicts[64];
int g_verdictCount = 0;
// statistics, per 300 frames
uint64_t g_overlapCount = 0;
int64_t g_setupWaitTicks = 0, g_joinWaitTicks = 0, g_overlapTicks = 0;
LARGE_INTEGER g_overlapStart{};
char g_lastJoinBy[64] = "";

bool IsShadowManager(void** vt) {
    if (!g_shadowVt && strstr(ClassName(vt), "cShadowMapManager@@")) g_shadowVt = vt;
    return vt == g_shadowVt;
}

bool Deferred(void** vt) {
    const char* name = ClassName(vt);
    for (const std::string& d : g_deferNames)
        if (strstr(name, d.c_str())) return true;
    return false;
}

// (glitch isolation switches) Modules that join the shadow pass first even if allowed beside it.
volatile bool g_joinScenery = false, g_joinConsist = false;
bool ForcedJoin(void** vt) {
    if (!g_joinScenery && !g_joinConsist) return false;
    const char* name = ClassName(vt);
    return (g_joinScenery && strstr(name, "cSceneryManager")) || (g_joinConsist && strstr(name, "cConsistManager"));
}

// The module named in [experiments] overlap_fork (verdict cached per vtable).
bool IsForkModule(void** vt) {
    static VtVerdict verdicts[64];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (verdicts[i].vt == vt) return verdicts[i].allowed;
    bool is = !g_forkName.empty() && strstr(ClassName(vt), g_forkName.c_str()) != nullptr;
    if (count < 64) verdicts[count++] = {vt, is};
    return is;
}

bool Allowed(void** vt) {
    for (int i = 0; i < g_verdictCount; ++i)
        if (g_verdicts[i].vt == vt) return g_verdicts[i].allowed;
    const char* name = ClassName(vt);
    bool ok = false;
    for (const std::string& a : g_allowed)
        if (!a.empty() && strstr(name, a.c_str())) ok = true;
    if (g_verdictCount < 64) g_verdicts[g_verdictCount++] = {vt, ok};
    Log("overlap: module %s %s", name, ok ? "runs alongside the shadow pass" : "joins first");
    return ok;
}

uintptr_t RunShadowModule(void* m) {
    void** vt = *static_cast<void***>(m);
    reinterpret_cast<ModuleFn>(vt[1])(m, kRenderPhase);
    return 0;
}

// A thread's call stack as return addresses, walked from a context. Called while that thread is
// suspended, so it takes no lock (no logging, no loader calls): the suspended thread may hold one
// (slow-load-1: a stack report hung the game with the log lock and the CRT stream lock taken).
int WalkStack(CONTEXT ctx, DWORD64* out, int max) {
    int n = 0;
    for (; n < max && ctx.Rip; ++n) {
        out[n] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    return n;
}

// Logs a call stack (module + offset per frame).
void LogFrames(const char* who, const DWORD64* frames, int n) {
    Log("overlap: %s stack:", who);
    for (int i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &m)) {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* base = strrchr(name, 0x5c);
            Log("overlap:   %s+0x%llx", base ? base + 1 : name,
                static_cast<unsigned long long>(frames[i] - reinterpret_cast<uintptr_t>(m)));
        } else {
            Log("overlap:   0x%llx", static_cast<unsigned long long>(frames[i]));
        }
    }
}

void LogStack(const char* who, const CONTEXT& ctx) {
    DWORD64 frames[32];
    LogFrames(who, frames, WalkStack(ctx, frames, 32));
}

// Another thread's stack: walked while it is suspended, logged after it runs again.
void LogThreadStack(HANDLE t, const char* who) {
    if (!t || GetThreadId(t) == GetCurrentThreadId()) return;
    if (SuspendThread(t) == static_cast<DWORD>(-1)) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    DWORD64 frames[32];
    int n = GetThreadContext(t, &ctx) ? WalkStack(ctx, frames, 32) : -1;
    ResumeThread(t);
    if (n >= 0) LogFrames(who, frames, n);
}

volatile bool g_joining = false;  // the game thread is waiting for the worker
volatile bool g_waitingFork = false;  // the game thread is waiting for the forked module's worker

// A join that waits for seconds is a deadlock: log both threads' stacks once.
void WaitForWorker(const char* why) {
    g_joining = true;
    struct Clear {
        ~Clear() { g_joining = false; }
    } clear;
    if (g_worker.WaitFor(3000)) return;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("overlap: join by %s has waited 3 s for the worker; stacks follow", why);
        CONTEXT self{};
        RtlCaptureContext(&self);
        LogStack("game thread", self);
        LogThreadStack(g_worker.Thread(), "worker");
    }
    g_worker.Wait();
}

// The game thread moves behind the early fork in the command stream (at the forked module's
// place in the dispatch order, or at a join that comes before it).
void ResumeBehindFork() {
    if (!g_forkAhead) return;
    g_forkAhead = false;
    if (g_forkHooks.closeCaller) g_forkHooks.closeCaller();
    rt::ResumeAfterForks();
}

}  // namespace
void LogStacks(const char* why, HANDLE other, const char* otherName);
namespace {
// Waits for the forked module (not for the shadow pass).
void WaitFork(bool atJoin) {
    if (!g_forked) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    ResumeBehindFork();
    g_waitingFork = true;
    struct Clear {
        ~Clear() { g_waitingFork = false; }
    } clear;
    // A wait of seconds is a deadlock: say so once, with both stacks (the bench stops the run on
    // this line), instead of hanging silently as t10 did.
    if (!g_forkWorker.WaitFor(3000)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogStacks("overlap: the game thread has waited 3 s for the worker of the forked module", g_forkWorker.Thread(),
                      "forked-module worker");
        }
        g_forkWorker.Wait();
    }
    g_forked = false;
    if (g_forkHooks.joined) g_forkHooks.joined();
    QueryPerformanceCounter(&t1);
    g_forkWaitTicks.QuadPart += t1.QuadPart - t0.QuadPart;
    TSCFIX_TRACE(trace::kOverlap, trace::kForkWait, static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), atJoin ? 1 : 0);
}

struct JoinStat {
    char why[48];
    uint32_t count;
    int64_t ticks;
};
JoinStat g_joinStats[16];
int g_joinStatCount = 0;

void Join(const char* why) {
    if (!g_overlapping) return;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    WaitFork(true);
    QueryPerformanceCounter(&t2);
    WaitForWorker(why);
    QueryPerformanceCounter(&t1);
    g_joinWaitTicks += t1.QuadPart - t2.QuadPart;
    g_overlapTicks += t1.QuadPart - g_overlapStart.QuadPart;
    rt::EndOverlap();
    g_overlapping = false;
    g_forkSlotPassed = false;
    g_forkGate = g_lastWasGate = false;
    strncpy_s(g_lastJoinBy, why, _TRUNCATE);
    if (trace::On(trace::kOverlap)) trace::Blob(trace::kJoinWait, static_cast<uint64_t>(t1.QuadPart - t2.QuadPart), why, strlen(why) + 1);
    // who ends the overlap, how often, and how long the game thread waited for the shadow pass there
    int i = 0;
    while (i < g_joinStatCount && strncmp(g_joinStats[i].why, why, sizeof g_joinStats[i].why - 1)) ++i;
    if (i == g_joinStatCount && g_joinStatCount < 16) strncpy_s(g_joinStats[g_joinStatCount++].why, why, _TRUNCATE);
    if (i < g_joinStatCount) ++g_joinStats[i].count, g_joinStats[i].ticks += t1.QuadPart - t2.QuadPart;
}

void JoinFromDevice() { Join("device call"); }

// [experiments] overlap_core: the worker gets the last physical core to itself (both its
// logical processors) and the game thread stays off it, so the two never share a core.
// overlap_core=2: the worker and the game thread share one physical core (its two logical
// processors), so the shadow walk and the main-view walk share its L2 cache as they do when
// both run on the game thread.
int g_dedicatedCore = 0;
bool g_coresAssigned = false;
DWORD g_ownerTid = 0;  // the thread that started the current overlap
ULONG_PTR g_offWorkerMask = 0;  // overlap_core=1: every core but the worker's
void AssignCores() {
    g_coresAssigned = true;
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buf.data());
    if (!GetLogicalProcessorInformation(info, &len)) return;
    ULONG_PTR all = 0, last = 0;
    for (DWORD i = 0; i < len / sizeof(*info); ++i)
        if (info[i].Relationship == RelationProcessorCore) {
            all |= info[i].ProcessorMask;
            last = info[i].ProcessorMask;
        }
    if (!last || last == all) return;
    if (g_dedicatedCore == 2) {
        SetThreadAffinityMask(g_worker.Thread(), last);
        SetThreadAffinityMask(GetCurrentThread(), last);
        Log("overlap: worker and game thread share core mask 0x%llx", static_cast<unsigned long long>(last));
        return;
    }
    // The forked module's worker gets the physical core before the shadow worker's to itself as
    // well: three threads walking the scene at once must not share a core's two hardware threads.
    ULONG_PTR fork = 0;
    if (g_forkWorker.Thread())
        for (DWORD i = 0; i < len / sizeof(*info); ++i)
            if (info[i].Relationship == RelationProcessorCore && info[i].ProcessorMask != last) fork = info[i].ProcessorMask;
    if (fork == (all & ~last)) fork = 0;  // two cores only: nothing to give away
    SetThreadAffinityMask(g_worker.Thread(), last);
    if (fork) SetThreadAffinityMask(g_forkWorker.Thread(), fork);
    SetThreadAffinityMask(GetCurrentThread(), all & ~last & ~fork);
    g_offWorkerMask = all & ~last & ~fork;
    Log("overlap: shadow worker on core mask 0x%llx, forked-module worker 0x%llx, game thread mask 0x%llx",
        static_cast<unsigned long long>(last), static_cast<unsigned long long>(fork),
        static_cast<unsigned long long>(all & ~last & ~fork));
}

// Returns true if the shadow module now runs on the worker.
bool StartShadow(void* module) {
    if (g_beforeShadow) g_beforeShadow();
    if (!rt::BeginOverlap(g_worker.ThreadId())) return false;
    g_ownerTid = GetCurrentThreadId();
    g_setupDone = false;
    g_shadowModule = module;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_worker.Launch(&RunShadowModule, module);
    // The cascade setup is short; spin until it is done (or the module returned without any).
    while (!g_setupDone && !g_worker.Finished()) _mm_pause();
    QueryPerformanceCounter(&t1);
    g_setupWaitTicks += t1.QuadPart - t0.QuadPart;
    g_overlapStart = t1;
    g_overlapping = true;
    ++g_overlapCount;
    return true;
}

// Optional guard around a dispatched module call (research: locks for modules that share data
// with a pipelined update).
ModuleGuardFn g_moduleGuard = nullptr;
struct ModuleScope {
    void** vt;
    int arg;
    bool on;
    ModuleScope(void** v, int a) : vt(v), arg(a), on(g_moduleGuard != nullptr) {
        if (on) g_moduleGuard(ClassName(vt), arg, true);
    }
    ~ModuleScope() {
        if (on) g_moduleGuard(ClassName(vt), arg, false);
    }
};
// (diagnosis) Module-level update freeze: while on, modules' non-render phases are skipped except
// the one switchable module the current glitch variant lets run ([experiments] glitch_update_modules).
volatile bool g_updFreeze = false;
std::vector<std::string> g_updNames;
volatile int g_updAllow = -1;
std::vector<std::pair<void**, int>> g_updSeen;
void NoteUpdateModule(void** vt, int arg) {
    if (g_updSeen.size() >= 120) return;
    for (auto& s : g_updSeen)
        if (s.first == vt && s.second == arg) return;
    g_updSeen.push_back({vt, arg});
    Log("overlap: update module %s phase %d", ClassName(vt), arg);
}
void (*g_moduleBeginHook)(const char*) = nullptr;  // (measurement, see SetModuleBeginHook)
void (*g_moduleEndHook)(const char*) = nullptr;
void DispatchModule(void* module, int arg);
// (telemetry) every module call, update and render phases alike, with its class name once.
void Dispatch(void* module, int arg) {
    if (!trace::On(trace::kFrame)) return DispatchModule(module, arg);
    void** vt = *static_cast<void***>(module);
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static void** named[128];
        static int count = 0;
        bool known = false;
        AcquireSRWLockShared(&lock);
        for (int i = 0; i < count; ++i) known |= named[i] == vt;
        ReleaseSRWLockShared(&lock);
        if (!known) {
            AcquireSRWLockExclusive(&lock);
            if (count < 128) named[count++] = vt;
            ReleaseSRWLockExclusive(&lock);
            const char* name = ClassName(vt);
            trace::Blob(trace::kModuleName, reinterpret_cast<uint64_t>(vt), name, strlen(name) + 1);
        }
    }
    trace::Emit(trace::kModuleBegin, reinterpret_cast<uint64_t>(vt), static_cast<uint64_t>(arg));
    DispatchModule(module, arg);
    trace::Emit(trace::kModuleEnd, reinterpret_cast<uint64_t>(vt), static_cast<uint64_t>(arg));
}
void DispatchModule(void* module, int arg) {
    void** vt = *static_cast<void***>(module);
    auto fn = reinterpret_cast<ModuleFn>(vt[1]);
    if (arg != kRenderPhase) {
        if (g_updFreeze) {
            int allow = g_updAllow;
            if (allow < 0 || !strstr(ClassName(vt), g_updNames[allow].c_str())) return;
        }
        NoteUpdateModule(vt, arg);
    }
    if (arg == kRenderPhase && ModuleSkipped(vt)) return;
    if (g_overlapping && GetCurrentThreadId() != g_ownerTid) return fn(module, arg);  // beside the overlap
    if (g_overlapping && g_worker.Finished()) Join("worker done");  // costs nothing now
    // Modules deferred during the overlap run (in order) before the next module after it.
    if (g_overlapping && arg == kRenderPhase && !g_deferNames.empty() && Deferred(vt)) {
        g_deferred.push_back({module, arg});
        return;
    }
    // The shadow manager itself runs here; its cascade drawing starts the overlap (LaunchCascades).
    if (g_overlapping && (!(arg == kRenderPhase && (Allowed(vt) || IsShadowManager(vt))) || ForcedJoin(vt)))
        Join(ClassName(vt));
    if (arg == kRenderPhase && !g_forkName.empty() && !g_forkAfter.empty()) {
        if (g_lastWasGate) g_forkGate = true;
        g_lastWasGate = g_overlapping && strstr(ClassName(vt), g_forkAfter.c_str()) != nullptr;
    }
    if (arg == kRenderPhase && !g_forkName.empty() && ((!g_forkFailed && !g_forkOff) || g_forkRanThisFrame)) {
        if (IsForkModule(vt)) {
            g_forkModule = module;
            if (g_forkRanThisFrame) {  // launched early: this is its place in the stream
                g_forkRanThisFrame = false;
                ResumeBehindFork();
                if (g_forkHooks.slot) g_forkHooks.slot();  // (after the game thread closed its own part)
                g_forkSlotPassed = g_overlapping;
                return;
            }
            if (g_overlapping && !g_forked) {
                if (g_forkHooks.closeCaller) g_forkHooks.closeCaller();
                if (rt::ForkSegment(g_forkWorker.ThreadId())) {
                    if (g_forkHooks.prepare) g_forkHooks.prepare(g_forkWorker.ThreadId());
                    g_forkCall = {module, arg};
                    g_forked = true;
                    g_forkSlotPassed = true;
                    ++g_forkCount;
                    g_forkWorker.Launch(&ForkMain, nullptr);
                    return;
                }
            }
        } else if (g_overlapping && g_forked && g_forkSlotPassed) {
            WaitFork(false);  // this module may draw what the forked one queued
        } else if (g_overlapping && !g_forked && g_forkEarly && g_forkModule && !g_forkRanThisFrame && !g_forkSlotPassed &&
                   (g_forkAfter.empty() || g_forkGate) && !IsShadowManager(vt)) {
            // The first module after the shadow launch: the forked module starts now, beside it.
            if (g_forkHooks.closeCaller) g_forkHooks.closeCaller();
            if (rt::ForkAhead(g_forkWorker.ThreadId())) {
                if (g_forkHooks.prepare) g_forkHooks.prepare(g_forkWorker.ThreadId());
                g_forkCall = {g_forkModule, kRenderPhase};
                g_forked = true;
                g_forkAhead = true;
                g_forkRanThisFrame = true;
                ++g_forkCount;
                ++g_forkEarlyCount;
                g_forkWorker.Launch(&ForkMain, nullptr);
            }
        }
    }
    if (!g_overlapping && !g_deferred.empty()) {
        std::vector<DeferredCall> calls;
        calls.swap(g_deferred);
        for (const DeferredCall& c : calls)
            reinterpret_cast<ModuleFn>((*static_cast<void***>(c.module))[1])(c.module, c.arg);
    }
#ifdef TSCFIX_RESEARCH
    if (g_traceModules && arg == 32) {
        // One frame: the first traced module coming round again ends it (the shadow update that
        // arms the trace runs before the frame's first render module).
        static void* first = nullptr;
        if (first == module) {
            g_traceModules = false;
            first = nullptr;
            Log("research: render module traces done");
            fn(module, arg);
            return;
        }
        if (!first) first = module;
        const char* name = ClassName(vt);
        // Every module to the end of the frame's render (the flag is cleared at Present): what the
        // modules after the scenery share with it matters as much as what those before it do.
        char tag[32];
        sprintf_s(tag, "mod%02d", g_position++);
        struct Call { ModuleFn fn; void* module; int arg; } c{fn, module, arg};
        Log("research: tracing render module %s as '%s'", name, tag);
        tracer::TraceCall2([](void* p, void*) -> uintptr_t {
            auto* k = static_cast<Call*>(p);
            k->fn(k->module, k->arg);
            return 0;
        }, &c, nullptr, tag);
        return;
    }
#endif
    ModuleScope scope(vt, arg);
    if (g_moduleBeginHook && arg == kRenderPhase) g_moduleBeginHook(ClassName(vt));
    if (!g_census) {
        fn(module, arg);
        if (g_moduleEndHook && arg == kRenderPhase) g_moduleEndHook(ClassName(vt));
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    fn(module, arg);
    QueryPerformanceCounter(&t1);
    int pos = g_position++;
    for (int i = 0; i < g_statCount; ++i)
        if (g_stats[i].vtable == vt && g_stats[i].arg == arg) {
            ++g_stats[i].calls;
            g_stats[i].ticks += t1.QuadPart - t0.QuadPart;
            g_stats[i].order = pos;
            return;
        }
    if (g_statCount < 64) g_stats[g_statCount++] = {vt, arg, pos, 1, t1.QuadPart - t0.QuadPart};
}

uint8_t* AllocNear(uint8_t* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = reinterpret_cast<uintptr_t>(target) & ~(gran - 1);
    for (uintptr_t delta = gran; delta < 0x7FF00000; delta += gran)
        for (uintptr_t candidate : {start - delta, start + delta})
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
    return nullptr;
}

}  // namespace

void ArmModuleTrace() { g_traceModules = true; }

bool GameThreadJoining() { return g_joining; }
bool GameThreadHolds(const CRITICAL_SECTION* lock) {
    return static_cast<DWORD>(reinterpret_cast<uintptr_t>(lock->OwningThread)) == g_ownerTid;
}

// The second shadow thread. The first shadow thread launches it and waits for it before its own
// job ends, so whoever waits for the first waits for both.
offload::Worker g_splitWorker;
bool StartSplitWorker() { return g_splitWorker.Thread() || g_splitWorker.Start(L"tscfix shadow maps 2"); }
DWORD SplitWorkerTid() { return g_splitWorker.Thread() ? g_splitWorker.ThreadId() : 0; }
void LaunchSplit(uintptr_t (*job)(void*), void* ctx) { g_splitWorker.Launch(job, ctx); }
void WaitSplit() { g_splitWorker.Wait(); }
bool GameThreadHoldsWhileWaitingForMe(const CRITICAL_SECTION* lock) {
    // (OwningThread holds the owner's thread id; read without the lock: the owner we care about
    // is blocked, so it does not change under us.)
    return GameThreadWaitingForMe() &&
           static_cast<DWORD>(reinterpret_cast<uintptr_t>(lock->OwningThread)) == g_ownerTid;
}
bool GameThreadWaitingForMe() {
    DWORD t = GetCurrentThreadId();
    return (g_joining && g_worker.Thread() && t == g_worker.ThreadId()) ||
           (g_joining && g_splitWorker.Thread() && t == g_splitWorker.ThreadId()) ||
           (g_waitingFork && g_forkWorker.Thread() && t == g_forkWorker.ThreadId());
}
bool Overlapping() { return g_overlapping; }

void WorkerThreadIds(DWORD out[2]) {
    out[0] = g_worker.Thread() ? g_worker.ThreadId() : 0;
    out[1] = g_forkWorker.Thread() ? g_forkWorker.ThreadId() : 0;
}

void LogStacks(const char* why, HANDLE other, const char* otherName) {
    Log("%s; stacks follow", why);
    CONTEXT self{};
    RtlCaptureContext(&self);
    LogStack("calling thread", self);
    auto logThread = LogThreadStack;
    logThread(other, otherName);
    logThread(g_worker.Thread(), "overlap worker");
    if (DWORD rtid = rt::RenderThreadId()) {
        if (HANDLE r = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, rtid)) {
            logThread(r, "render (replay) thread");
            CloseHandle(r);
        }
    }
    Log("overlap: overlapping %d, owner thread %lu, render pause requests %ld", g_overlapping ? 1 : 0, g_ownerTid,
        rt::PauseRequests());
}

bool KeepOffWorkerCore() {
    if (g_dedicatedCore != 1 || !g_offWorkerMask) return false;
    return SetThreadAffinityMask(GetCurrentThread(), g_offWorkerMask) != 0;
}

void SetForkHooks(const ForkHooks& hooks) { g_forkHooks = hooks; }
void SetModuleBeginHook(void (*hook)(const char* className)) { g_moduleBeginHook = hook; }
void SetModuleEndHook(void (*hook)(const char* className)) { g_moduleEndHook = hook; }
void SetBeforeShadowHook(void (*hook)()) { g_beforeShadow = hook; }
void SetModuleGuard(ModuleGuardFn guard) { g_moduleGuard = guard; }
void SetForkActive(bool on) { g_forkOff = !on; }
void WaitForFork() {
    if (g_overlapping && g_forked && GetCurrentThreadId() == g_ownerTid) WaitFork(false);
}
bool OnSplitWorker() { return g_splitWorker.Thread() && GetCurrentThreadId() == g_splitWorker.ThreadId(); }
bool OnForkWorker() { return g_forkWorker.ThreadId() && GetCurrentThreadId() == g_forkWorker.ThreadId(); }
// A module is configured to run on the fork worker (a second main-view thread exists).
bool ForkActive() { return !g_forkName.empty() && !g_forkFailed; }

void JoinIfOverlapping(const char* why) {
    // Only the thread that started the overlap ends it (a pipelined update runs beside it).
    if (g_overlapping && GetCurrentThreadId() == g_ownerTid) Join(why);
}

bool LaunchCascades(uintptr_t (*job)(void*), void* ctx, void (*beforeLaunch)(DWORD worker)) {
    if (!g_overlapOn) return false;
    if (g_dedicatedCore && !g_coresAssigned) AssignCores();
    if (g_overlapping) Join("next shadow light");
    if (g_beforeShadow) g_beforeShadow();
    if (!rt::BeginOverlap(g_worker.ThreadId())) return false;
    g_ownerTid = GetCurrentThreadId();
    if (beforeLaunch) beforeLaunch(g_worker.ThreadId());
    QueryPerformanceCounter(&g_overlapStart);
    g_overlapping = true;  // before the worker runs: overlap-only locks see it from its first call
    ++g_overlapCount;
    g_worker.Launch(job, ctx);
    return true;
}

void ShadowSetupDone() {
    if (g_overlapping || GetCurrentThreadId() == g_worker.ThreadId()) g_setupDone = true;
}

void SetDedicatedCore(int mode) { g_dedicatedCore = mode; }

bool InstallOverlap(const char* allowList) {
    glitch::RegisterSwitch("join:scenery", [](bool on) { g_joinScenery = on; });
    glitch::RegisterSwitch("join:consist", [](bool on) { g_joinConsist = on; });
    if (!g_worker.Start(L"tscfix shadow overlap")) return false;
    std::string list = allowList ? allowList : "";
    for (size_t pos = 0; pos <= list.size();) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty() && item[0] == '>') g_deferNames.push_back(item.substr(1));
        else if (!item.empty()) g_allowed.push_back(item);
        pos = comma + 1;
    }
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
    if (tscfix::config::Int("profile", "worker", 0, ini) == 1) profiler::Retarget(GetThreadId(g_worker.Thread()));
    char autoCfg[64] = "";
    tscfix::config::Str("experiments", "overlap_auto", "", autoCfg, sizeof autoCfg, ini);
    double onMs = 0, offMs = 0;
    if (sscanf_s(autoCfg, "%lf,%lf", &onMs, &offMs) == 2 && onMs > 0 && offMs > 0) SetAuto(onMs, offMs);
    char fork[128] = "";
    // (default: the scenery on its own thread; overlap_fork=0 switches it off)
    tscfix::config::Str("experiments", "overlap_fork", "cSceneryManager", fork, sizeof fork, ini);
    if (strcmp(fork, "0") == 0) fork[0] = 0;
    {
        char only[512];
        tscfix::config::Str("experiments", "glitch_skip_modules", "", only, sizeof only, ini);
        for (char* p = only; *p;) {
            char* c = strchr(p, ',');
            if (c) *c = 0;
            if (*p) g_skipModules.push_back(p);
            if (!c) break;
            p = c + 1;
        }
        if (!g_skipModules.empty()) Log("overlap: not drawing %zu render module name(s) (glitch isolation)", g_skipModules.size());
    }
    g_forkName = fork;
    g_forkEarly = tscfix::config::Int("experiments", "overlap_fork_early", 1, ini) != 0;
    char after[96];
    tscfix::config::Str("experiments", "overlap_fork_after", "0", after, sizeof after, ini);
    g_forkAfter = strcmp(after, "0") ? after : "";
    if (!g_forkName.empty() && g_forkWorker.Start(L"tscfix fork worker"))
        Log("overlap: modules named '%s' run on a second worker during the overlap%s", fork,
            g_forkEarly ? (g_forkAfter.empty() ? ", started when the overlap starts" : ", started after the module named in overlap_fork_after") : "");
    else
        g_forkName.clear();
    rt::SetOverlapJoinHook(&JoinFromDevice);
    rt::SetOverlapWorkerDoneHook([] { return g_overlapping && g_worker.Finished(); });
    Log("overlap: worker started; %zu module name(s) allowed alongside the shadow pass, %zu deferred until it ends",
        g_allowed.size(), g_deferNames.size());
    return true;
}

void SetActive(bool on) {
    g_overlapOn = on;
    d3d::SetLockPathsActive(on);
    status::Set("overlap", on ? "shadow overlap ON" : "shadow overlap OFF");
    // Game thread, frame boundary. With the overlap off the game thread may use every core
    // again (the one kept for the worker may be the fastest); AssignCores restricts it again at
    // the next overlap.
    if (!on && g_coresAssigned) {
        DWORD_PTR process = 0, system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) SetThreadAffinityMask(GetCurrentThread(), process);
        g_coresAssigned = false;
    }
}

// Only whether the shadow pass goes to the worker (lock paths, core assignment and status stay as
// they are): for per-frame switching (the glitch oracle).
void SetOverlapOnly(bool on) { g_overlapOn = on; }

void SetUpdateFreeze(bool on) { g_updFreeze = on; }
// Skip-one mode ([experiments] glitch_upd_mode=skip): an upd:<name> switch skips only that class's
// per-object updates (everything else updates), for bisecting which update collides with the render.
bool g_updSkipOne = false;
void SetUpdateSkipOne(bool on) { g_updSkipOne = on; }
// Per vtable: the index of the first upd: name its class contains (-1: none), resolved once (the
// class name lookup is far too slow for every per-object update). Game thread only.
int UpdateClassIndex(void** vt) {
    static std::unordered_map<void**, int> cache;
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    NoteUpdateModule(vt, 0);
    int idx = -1;
    const char* name = ClassName(vt);
    for (size_t i = 0; i < g_updNames.size() && idx < 0; ++i)
        if (strstr(name, g_updNames[i].c_str())) idx = static_cast<int>(i);
    cache[vt] = idx;
    return idx;
}
bool UpdateSkipped(void** vt) {
    if (!g_updSkipOne && !g_updFreeze) {
        UpdateClassIndex(vt);  // (logs each class once)
        return false;
    }
    int idx = UpdateClassIndex(vt);
    if (g_updSkipOne) {
        int skip = g_updAllow;
        return skip >= 0 && idx == skip;
    }
    int allow = g_updAllow;
    return allow < 0 || idx != allow;
}
void RegisterUpdateSwitches(const char* list) {
    std::string l = list ? list : "";
    for (size_t pos = 0; pos < l.size();) {
        size_t comma = l.find(',', pos);
        if (comma == std::string::npos) comma = l.size();
        std::string item = l.substr(pos, comma - pos);
        pos = comma + 1;
        if (item.empty() || g_updNames.size() >= 32) continue;
        g_updNames.push_back(item);
    }
    // Switch functions are plain function pointers: one per slot.
    static void (*const slots[32])(bool) = {
#define S(i) [](bool on) { g_updAllow = on ? i : -1; }
        S(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
        S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31)
#undef S
    };
    for (size_t i = 0; i < g_updNames.size(); ++i) {
        static std::vector<std::string> names;
        names.push_back("upd:" + g_updNames[i]);
        glitch::RegisterSwitch(names.back().c_str(), slots[i]);
    }
}

bool Install(HMODULE engine, bool census) {
    g_census = census;
    auto* fn = static_cast<uint8_t*>(sites::Find(engine, kScheduler));
    if (!fn) {
        Log("overlap: scheduler not found (game version changed?); disabled");
        return false;
    }
    uint8_t* site = nullptr;
    for (size_t i = 0; i + sizeof kCallPattern + 4 <= kSchedulerScan; ++i)
        if (!memcmp(fn + i, kCallPattern, sizeof kCallPattern)) {
            if (site) return Log("overlap: dispatch call not unique; disabled"), false;
            site = fn + i + 5;  // the call instruction
        }
    if (!site) return Log("overlap: dispatch call not found; disabled"), false;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    uint8_t* thunk = site + 5 + rel;
    if (memcmp(thunk, kThunk, sizeof kThunk)) return Log("overlap: dispatch target unexpected; disabled"), false;

    // Stub within rel32 reach: mov rax, imm64; jmp rax.
    uint8_t* stub = AllocNear(site, 64);
    if (!stub) return Log("overlap: no memory near the engine; disabled"), false;
    uint8_t code[12] = {0x48, 0xB8};
    auto target = reinterpret_cast<uint64_t>(&Dispatch);
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    memcpy(stub, code, sizeof code);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof code);

    int32_t newRel = static_cast<int32_t>(stub - (site + 5));
    DWORD old = 0;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site + 1, &newRel, 4);  // installed at engine load, before the scheduler first runs
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("overlap: scheduler dispatch redirected%s", census ? " (module census)" : "");
    return true;
}

namespace {

// [experiments] overlap_auto=<on ms>,<off ms>: the overlap pays only when the shadow pass is
// heavy. The smoothed frame time switches it on above <on ms> and off below <off ms>.
double g_autoOnMs = 0, g_autoOffMs = 0, g_frameEma = 0, g_shadowEma = 0;
volatile int64_t g_shadowTicks = 0;  // this frame's cascade drawing
uint32_t g_dwell = 0;                // frames since the last switch
bool (*g_engineHooks)(bool on) = nullptr;
LARGE_INTEGER g_lastFrame{};
uint32_t g_framesOn = 0, g_framesOff = 0, g_switches = 0;

// Probing: every kProbeEvery frames the other mode runs for kProbeSettle + kProbeMeasure
// frames; whichever had the lower average frame time over kProbeMeasure frames is kept. The
// scene decides, not a model of it (on light scenes the worker is slower than inline).
constexpr uint32_t kProbeEvery = 600, kProbeSettle = 30, kProbeMeasure = 60;
uint32_t g_phaseFrames = 0;  // frames in the current phase
bool g_probing = false;
double g_homeMs = 0, g_probeMs = 0;  // sums over the measured frames
uint32_t g_probes = 0, g_kept = 0;

bool Switch(bool on) {
    if (on == g_overlapOn) return true;
    if (g_engineHooks && !g_engineHooks(on)) return false;
    SetActive(on);
    ++g_switches;
    return true;
}

void AutoSwitch() {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double ms = 0;
    if (g_lastFrame.QuadPart) ms = (now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / f.QuadPart;
    g_lastFrame = now;
    if (ms <= 0 || ms > 1000) return;  // loading, pauses
    g_frameEma = g_frameEma ? g_frameEma * 0.95 + ms * 0.05 : ms;
    g_shadowEma = g_shadowEma * 0.95 + InterlockedExchange64(&g_shadowTicks, 0) * 1000.0 / f.QuadPart * 0.05;
    ++(g_overlapOn ? g_framesOn : g_framesOff);
    ++g_phaseFrames;
    if (!g_probing) {
        // the last kProbeMeasure frames of the home mode
        if (g_phaseFrames > kProbeEvery - kProbeMeasure) g_homeMs += ms;
        if (g_phaseFrames >= kProbeEvery && Switch(!g_overlapOn)) {
            g_probing = true;
            g_phaseFrames = 0;
            g_probeMs = 0;
        }
        return;
    }
    if (g_phaseFrames > kProbeSettle) g_probeMs += ms;
    if (g_phaseFrames < kProbeSettle + kProbeMeasure) return;
    ++g_probes;
    bool keep = g_probeMs < g_homeMs;  // the probed mode was faster
    if (keep || Switch(!g_overlapOn)) {
        if (keep) ++g_kept;
        g_probing = false;
        g_phaseFrames = 0;
        g_homeMs = 0;
    }
}
}  // namespace

void SetEngineHooksSwitch(bool (*fn)(bool on)) { g_engineHooks = fn; }
void AddShadowTime(int64_t ticks) { InterlockedExchangeAdd64(&g_shadowTicks, ticks); }

void SetAuto(double onMs, double offMs) {
    g_autoOnMs = onMs;
    g_autoOffMs = offMs;
    Log("overlap: automatic (probes the other mode every %u frames, keeps the faster)", kProbeEvery);
    (void)onMs, (void)offMs;
}

// --- slow-frame watchdog ----------------------------------------------------------------------
// A frame that takes far longer than its neighbours while overlaps are running is nearly always
// threads waiting for each other. A watchdog thread notices a frame still open after
// kSlowFrameMs and logs where the game thread, the workers and the replay thread are at that
// moment (a few times per session), so a sick run explains itself in its own log.
constexpr DWORD kSlowFrameMs = 250;
volatile LONGLONG g_frameOpenedTick = 0;  // GetTickCount64 at the last Present; 0 until overlaps run
volatile LONG g_frameSerial = 0;
HANDLE g_gameThread = nullptr;
DWORD WINAPI WatchdogMain(void*) {
    LONG reported = -1;
    int reports = 0;
    ULONGLONG lastReport = 0;
    for (;;) {
        Sleep(50);
        LONGLONG opened = g_frameOpenedTick;
        LONG serial = g_frameSerial;
        if (!opened || !g_overlapOn || serial == reported || reports >= 8) continue;
        ULONGLONG now = GetTickCount64();
        if (now - static_cast<ULONGLONG>(opened) < kSlowFrameMs || now - lastReport < 2000) continue;
        reported = serial;
        lastReport = now;
        ++reports;
        char why[160];
        sprintf_s(why, "overlap: slow frame %ld: open for %llu ms (overlapping %d, forked %d, joining %d, waiting for fork %d)",
                  serial, now - static_cast<ULONGLONG>(opened), g_overlapping ? 1 : 0, g_forked ? 1 : 0, g_joining ? 1 : 0,
                  g_waitingFork ? 1 : 0);
        LogStacks(why, g_gameThread, "game thread");
        LogThreadStack(g_forkWorker.Thread(), "forked-module worker");
    }
}

void OnFrame() {
    Join("present");  // normally already joined by the device's Present
    if (g_overlapCount || g_gameThread) {  // in the game (overlaps have happened): the watchdog watches from here on
        if (!g_gameThread) {
            g_gameThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
            CloseHandle(CreateThread(nullptr, 0, &WatchdogMain, nullptr, 0, nullptr));
        }
        InterlockedExchange64(&g_frameOpenedTick, static_cast<LONGLONG>(GetTickCount64()));
        InterlockedIncrement(&g_frameSerial);
    }
    g_position = 0;
    g_forkRanThisFrame = false;
    g_traceModules = false;
    if (g_autoOnMs > 0) AutoSwitch();
    static uint64_t frames = 0;
    if (++frames % 300 == 0 && (g_overlapCount || g_overlapOn)) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double ms = 1000.0 / f.QuadPart / 300.0;
        Log("overlap: last 300 frames: %llu overlaps, setup wait %.2f ms/frame, overlapped %.2f ms/frame, "
            "join wait %.2f ms/frame (last join by %s)", static_cast<unsigned long long>(g_overlapCount),
            g_setupWaitTicks * ms, g_overlapTicks * ms, g_joinWaitTicks * ms, g_lastJoinBy);
        for (int i = 0; i < g_joinStatCount; ++i)
            if (g_joinStats[i].count)
                Log("overlap:   joined by %s: %.2f/frame, waited %.2f ms/frame for the shadow pass", g_joinStats[i].why,
                    g_joinStats[i].count / 300.0, g_joinStats[i].ticks * ms);
        for (int i = 0; i < g_joinStatCount; ++i) g_joinStats[i].count = 0, g_joinStats[i].ticks = 0;
        if (g_forkCount)
            Log("overlap:   forked module: %.2f/frame (%.2f early), worker busy %.2f ms/frame, game thread waited %.2f ms/frame for it",
                g_forkCount / 300.0, g_forkEarlyCount / 300.0, g_forkBusyTicks * ms, g_forkWaitTicks.QuadPart * ms);
        g_forkCount = g_forkEarlyCount = 0;
        g_forkBusyTicks = 0;
        g_forkWaitTicks.QuadPart = 0;
        if (g_autoOnMs > 0) {
            Log("overlap: automatic: shadow pass %.1f ms, frame %.1f ms (smoothed), on %u / off %u frames, %u switch(es), "
                "%u probe(s), %u kept", g_shadowEma, g_frameEma, g_framesOn, g_framesOff, g_switches, g_probes, g_kept);
            g_framesOn = g_framesOff = g_switches = g_probes = g_kept = 0;
        }
        g_overlapCount = 0;
        g_setupWaitTicks = g_joinWaitTicks = g_overlapTicks = 0;
    }
    if (!g_census || ++g_frames % 300 != 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (int order = 0; order < 64; ++order)
        for (int i = 0; i < g_statCount; ++i) {
            const ModuleStat& s = g_stats[i];
            if (s.order != order) continue;
            Log("census: #%2d arg=%d %-48s calls/frame=%.2f ms/frame=%.2f", s.order, s.arg, ClassName(s.vtable),
                s.calls / 300.0, s.ticks * 1000.0 / f.QuadPart / 300.0);
        }
    for (int i = 0; i < g_statCount; ++i) g_stats[i].calls = 0, g_stats[i].ticks = 0;
}

}  // namespace tscfix::overlap
