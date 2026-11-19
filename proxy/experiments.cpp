#include "experiments.h"

#include <MinHook.h>
#include <d3d9.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine_sites.h"
#include "log.h"
#include "offload.h"
#include "overlap.h"
#include "rtdevice.h"
#include "overlapstate.h"
#include "sites.h"
#include "status.h"
#include "telemetry.h"
#include "trace.h"

#if __has_include("tracer.h")  // the engine-specific parts
#include "binder.h"
#include "pipeline.h"
#include "tracer.h"
#define TSCFIX_RESEARCH 1
#endif

namespace tscfix::experiments {
thread_local const int* t_shadowMapIndex = nullptr;
thread_local uint32_t t_shadowMaps = 0;
namespace {

// Per-frame shadow map update (game version v79.0a); takes only `this`.
constexpr sites::Fingerprint kShadowUpdate = {0x992f8a8a58917831ull, 16};

using UpdateFn = void (*)(void* self);
UpdateFn g_shadowUpdate = nullptr;
volatile bool g_shadowSkip = false;
int g_every = 2;
unsigned g_calls = 0;

void ShadowUpdate(void* self) {
    if (g_shadowSkip && (g_calls++ % g_every) != 0) return;  // keep last frame's shadow map
    g_shadowUpdate(self);
}

// --- probe: scenery render calls per frame ------------------------------------------------
// Scenery render entry (game version v79.0a): (this, int pass).
constexpr sites::Fingerprint kSceneryRender = {0x7bdec49d43c6a149ull, 16};
using SceneryFn = void (*)(void* self, int pass);
SceneryFn g_scenery = nullptr;
HMODULE g_engine = nullptr;

// Every scenery draw starts by asking the player profile and the scenario for its draw flags,
// each through a counted pointer it takes and drops again. The counts are plain increments: with
// the game thread and a shadow thread drawing scenery at once a count can be lost, and the
// scenario is then freed while still in use (run split-ab-stand: pure virtual call on the game
// thread and the shadow thread, 107 s in). The flags do not change within a frame, so a shadow
// thread uses what the game thread read before it launched the shadow pass and never touches
// the two objects.
constexpr sites::Fingerprint kSceneryFlags = {0xe5f694bdef1efad1ull, 24};
using SceneryFlagsFn = uint64_t (*)();
SceneryFlagsFn g_sceneryFlags = nullptr;
volatile uint64_t g_sceneryFlagsSeen = 0;
volatile bool g_sceneryFlagsKnown = false;
uint64_t SceneryFlags() {
    if (overlapstate::ShadowRole() && g_sceneryFlagsKnown) return g_sceneryFlagsSeen;
    return g_sceneryFlags();
}
// Game thread, before the shadow pass launches.
void ReadSceneryFlags() {
    if (!g_sceneryFlags) return;
    g_sceneryFlagsSeen = g_sceneryFlags();
    g_sceneryFlagsKnown = true;
}

struct ProbeKey { uintptr_t caller; int pass; };
struct ProbeStat { ProbeKey key; uint64_t calls; int64_t ticks; };
ProbeStat g_probe[32];
int g_probeCount = 0;
uint64_t g_probeFrames = 0;
bool g_probeOn = false;
int64_t g_shadowTicks = 0;  // whole shadow update, while probing

// --- staggered shadow cascades ----------------------------------------------------------------
// The shadow pass renders the scenery once per cascade. With staggering active, cascade 0
// is drawn every frame and the outer cascades take turns; a cascade that is not drawn
// keeps its previous shadow map, and its projection matrix is restored to the one that
// map was drawn with so the two stay consistent. The matrix array location inside the
// shadow object comes from local configuration (it describes game internals).
constexpr sites::Fingerprint kShadowCascades = {0x44bdc2ab74b030d0ull, 32};
using CascadesFn = uintptr_t (*)(void* self, void* arg);
CascadesFn g_cascades = nullptr;
void* g_shadowObj = nullptr;
thread_local bool g_inShadow = false;  // this thread runs the shadow update (game thread or worker)
int g_cascadeCall = 0;
volatile bool g_stagger = false;
int g_matrixOffset = 0, g_matrixStride = 0, g_maxCascades = 0;
uint64_t g_staggerFrame = 0;
uint8_t g_savedMatrix[8][64];
bool g_haveSaved[8] = {};
uint64_t g_cascadeDraws = 0, g_cascadeSkips = 0, g_cascadeFrames = 0;
bool g_splitMode = false;  // shadow_split: alternate near / outer cascades between frames

// One-frame trace of device calls made during the shadow update (frame 600 after install).
bool g_traceFrame = false;

// Each cascade's region of the shadow atlas is cleared right before its scenery draw.
// While staggering, that clear is held back until we know whether the cascade will be
// drawn: drawn -> clear then draw; skipped -> drop the clear so the old map survives.
// A held clear not followed by a cascade draw is executed at the next state change.
struct PendingClear {
    bool active = false;
    IDirect3DDevice9* device = nullptr;
    DWORD flags = 0;
    D3DCOLOR color = 0;
    float z = 0;
    DWORD stencil = 0;
} g_pendingClear;

void ExecutePendingClear() {
    if (!g_pendingClear.active) return;
    g_pendingClear.active = false;
    auto& p = g_pendingClear;
    p.device->Clear(0, nullptr, p.flags, p.color, p.z, p.stencil);
}

// --- coarser level of detail in the shadow pass ------------------------------------------------
// The engine's LOD selector adds a global integer bias (in its render-settings object) to the
// level it picks. While the shadow update runs, that bias is raised so shadow casters use
// coarser meshes; it is restored before the main view renders. The selector function is
// found by fingerprint and the settings object through its own RIP-relative load; the field
// offset comes from local configuration.
constexpr sites::Fingerprint kLodSelect = {0xebf959af1193fc7dull, 24};
void** g_lodSettingsVar = nullptr;  // address of the global pointer to the settings object
int g_lodBiasOffset = 0;
volatile int g_shadowLodBias = 0;    // 0 = off

// --- shadow update on a worker thread ------------------------------------------------------
// Step one towards overlapping the shadow pass with the main view: the whole update runs
// on a worker while the game thread waits, which shows whether the engine's shadow code
// (and the recorder) work off the game thread at all.
offload::Worker g_shadowWorker;
volatile bool g_shadowOnWorker = false;
struct CascadeCall { void* self; void* arg; };
uintptr_t RunCascades(void* p) {
    auto* c = static_cast<CascadeCall*>(p);
    return g_cascades(c->self, c->arg);
}

int g_traceCascade = -1;      // research: which shadow cascade to single-step trace (99 = overlap trace)
uint64_t g_traceAtFrame = 0;
bool g_traceMain = false;     // overlap trace: the next main-view scenery walk is traced too

thread_local bool t_binderSnapshotted = false;  // the launcher took the binder snapshot already

// --- shadow maps on two threads -----------------------------------------------------------------
// The engine's loop (kShadowCascades) draws the sun's shadow maps one after another, keeping the
// current map's index and matrices in the shadow manager and its camera in the manager's light
// viewport. Traced (research/tools/cascades.py): the widest map is about half the pass, and what
// two maps drawn at once would share is that current-map state plus things the shadow thread
// already has to itself. So the second thread runs the same loop on a copy of the manager, with
// a light viewport of its own, and the one engine function that reads the current map through
// the manager (an object against the map's bounds) is pointed at the copy on that thread.
// Each map's content is switched per map by the engine's own masks: a thread's copy has the
// other thread's maps masked out.
constexpr sites::Fingerprint kInCurrentMap = {0xe231e2a7acf023ebull, 16};       // cShadowMapManager: (self, point, radius)
constexpr sites::Fingerprint kLightViewportCtor = {0xd182543ecd8c531bull, 16};  // cLightViewPort::cLightViewPort
constexpr size_t kManagerSize = 0x8a8, kManagerMapCount = 0x33c, kManagerMapMasks = 0x360, kManagerViewport = 0x7b0,
                 kManagerMapIndex = 0x7d8, kLightInfoSize = 0x598, kViewportSize = 0x3440;
using InCurrentMapFn = uint8_t (*)(void* self, void* point, float radius);
using ViewportCtorFn = void* (*)(void* mem);
InCurrentMapFn g_inCurrentMap = nullptr;
ViewportCtorFn g_viewportCtor = nullptr;
volatile bool g_mapSplit = false;
volatile bool g_mapSplitFailed = false;  // the second thread threw: one thread from then on
int g_splitMapConfig = -1;               // the map the second thread draws (-1: the last but one)
void* g_realManager = nullptr;
alignas(16) uint8_t g_managerCopy[kManagerSize];
alignas(16) uint8_t g_lightInfoCopy[kLightInfoSize];
void* g_viewportCopy = nullptr;
int g_splitMap = 0;
thread_local void* t_manager = nullptr;  // second shadow thread: its copy of the manager
int64_t g_splitFirstTicks = 0, g_splitSecondTicks = 0, g_splitWaitTicks = 0;
unsigned g_splitPasses = 0, g_splitDeclined = 0;

uint8_t InCurrentMap(void* self, void* point, float radius) {
    return g_inCurrentMap(t_manager && self == g_realManager ? t_manager : self, point, radius);
}

// Game thread, before the shadow pass launches: the second thread's queues and viewport (the
// engine's allocator is the game thread's).
void PrepareMapSplit() {
    if (!g_mapSplit || g_mapSplitFailed) return;
#ifdef TSCFIX_RESEARCH
    pipeline::PrepareSplit();
#endif
    if (!g_viewportCopy && g_viewportCtor) {
        if (void* mem = _aligned_malloc(kViewportSize, 16)) {
            memset(mem, 0, kViewportSize);
            g_viewportCopy = g_viewportCtor(mem);
        }
    }
}

uintptr_t SplitJobMain(void*) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    overlapstate::SetShadowRole(true);
    g_inShadow = true;
#ifdef TSCFIX_RESEARCH
    pipeline::SplitEnter();
#endif
    t_manager = g_managerCopy;
    t_shadowMapIndex = reinterpret_cast<const int*>(g_managerCopy + kManagerMapIndex);
    t_shadowMaps = 1u << g_splitMap;
    TSCFIX_TRACE(trace::kOverlap, trace::kMapPassBegin, 1);
    try {
        g_cascades(g_managerCopy, g_lightInfoCopy);
    } catch (...) {
        g_mapSplitFailed = true;
        Log("cascades: the second shadow thread threw an exception; the shadow maps stay on one thread from now on");
    }
    TSCFIX_TRACE(trace::kOverlap, trace::kMapPassEnd, 1);
    t_shadowMapIndex = nullptr;
    t_manager = nullptr;
#ifdef TSCFIX_RESEARCH
    pipeline::SplitLeave();
#endif
    g_inShadow = false;
    overlapstate::SetShadowRole(false);
    rt::SegmentDone();
    QueryPerformanceCounter(&t1);
    g_splitSecondTicks += t1.QuadPart - t0.QuadPart;
    return 0;
}

// First shadow thread: hands one map to the second thread and draws the others.
uintptr_t CascadesOnTwoThreads(void* self, void* arg) {
    auto* manager = static_cast<uint8_t*>(self);
    void** info = static_cast<void**>(arg);
    int count = 0;
    memcpy(&count, manager + kManagerMapCount, 4);
    // The engine draws the maps from the last index down to 0 and lets each map inherit render states
    // from the one before (the slope-scaled depth bias is set once at the start and changed by some
    // draw modules). The second thread's commands are replayed after all of the first thread's, so
    // only the map that is drawn last (index 0) inherits the same states there as in the engine's
    // own order. With a map from the middle, that map and the next one had each other's depth bias
    // and about 245 shadow draws per frame differed from a serial frame (run croydon-flash-all).
    int map = g_splitMapConfig >= 0 ? g_splitMapConfig : 0;
    uint8_t* viewport = nullptr;
    memcpy(&viewport, manager + kManagerViewport, 8);
    // Only the sun's maps (light kind 0): its per-map content masks hand the maps out.
    bool can = info && info[0] && info[1] && viewport && g_viewportCopy && count >= 3 && count <= 8 && map >= 0 &&
               map < count && reinterpret_cast<int (*)(void*)>((*static_cast<void***>(info[0]))[1])(info[0]) == 0;
#ifdef TSCFIX_RESEARCH
    can = can && pipeline::SplitSnapshot();
#else
    can = false;
#endif
    if (!can || !rt::ForkAhead(overlap::SplitWorkerTid(), true)) {
        ++g_splitDeclined;
        return g_cascades(self, arg);
    }
    g_realManager = self;
    g_splitMap = map;
    memcpy(g_managerCopy, manager, kManagerSize);
    // (The second viewport is only constructed, as the engine's own is: the loop sets what it needs
    // for each map. Copying the engine's fields into it made the two share objects the viewport
    // owns, and every run died somewhere else: split4, split6 - split8.)
    memcpy(g_managerCopy + kManagerViewport, &g_viewportCopy, 8);
    memcpy(g_lightInfoCopy, arg, kLightInfoSize);
    uint8_t saved = manager[kManagerMapMasks + map * 4];
    manager[kManagerMapMasks + map * 4] = 0;
    for (int k = 0; k < count; ++k)
        if (k != map) g_managerCopy[kManagerMapMasks + k * 4] = 0;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    overlap::LaunchSplit(&SplitJobMain, nullptr);
    t_shadowMapIndex = reinterpret_cast<const int*>(manager + kManagerMapIndex);
    t_shadowMaps = ~(1u << map);
    struct Finish {  // also when the engine throws: the masks go back and the second thread is done
        uint8_t* mask;
        uint8_t saved;
        ~Finish() {
            t_shadowMapIndex = nullptr;
            *mask = saved;
            overlap::WaitSplit();
            rt::ResumeAfterForks();
        }
    };
    uintptr_t r;
    {
        Finish finish{manager + kManagerMapMasks + map * 4, saved};
        TSCFIX_TRACE(trace::kOverlap, trace::kMapPassBegin, 0);
        r = g_cascades(self, arg);
        TSCFIX_TRACE(trace::kOverlap, trace::kMapPassEnd, 0);
        QueryPerformanceCounter(&t1);
    }
    QueryPerformanceCounter(&t2);
    g_splitFirstTicks += t1.QuadPart - t0.QuadPart;
    g_splitWaitTicks += t2.QuadPart - t1.QuadPart;
    if (++g_splitPasses % 600 == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double ms = 1000.0 / f.QuadPart / 600.0;
        Log("cascades: two shadow threads, last 600 passes: first %.2f ms (maps but %d of %d), second %.2f ms (map %d), "
            "first waited %.2f ms for the second; %u passes on one thread",
            g_splitFirstTicks * ms, map, count, g_splitSecondTicks * ms, map, g_splitWaitTicks * ms, g_splitDeclined);
        g_splitFirstTicks = g_splitSecondTicks = g_splitWaitTicks = 0;
        g_splitDeclined = 0;
    }
    return r;
}

uintptr_t ShadowCascadesBody(void* self, void* arg) {
    g_inShadow = true;
    // Private per-thread state only where another thread draws at the same time (the overlap
    // worker, or the synchronous shadow worker); inline, the cascades run exactly as in the game.
    const bool privateState = t_binderSnapshotted || g_shadowOnWorker;
    if (privateState) overlapstate::SetShadowRole(true);
#ifdef TSCFIX_RESEARCH
    if (privateState) {
        pipeline::BeginWorkerPass();
        if (!t_binderSnapshotted) {
            binder::EnterShadow();
            if (binder::Substituting()) pipeline::PrepareWorker(binder::Copy());
        }
        binder::ActivateThread();
    }
#endif
    overlap::ShadowSetupDone();  // the shadow camera is set up by now
    g_shadowObj = self;
    g_cascadeCall = 0;
    ++g_staggerFrame;
    if (g_staggerFrame == 600) g_traceFrame = true;  // traced until the next Present
    if (g_traceFrame) Log("trace: shadow update begin");
    int* bias = nullptr;
    int savedBias = 0;
    if (g_shadowLodBias && g_lodSettingsVar && *g_lodSettingsVar) {
        bias = reinterpret_cast<int*>(static_cast<uint8_t*>(*g_lodSettingsVar) + g_lodBiasOffset);
        savedBias = *bias;
        *bias = savedBias + g_shadowLodBias;
    }
    LARGE_INTEGER t0{}, t1;
    if (g_probeOn) QueryPerformanceCounter(&t0);
    uintptr_t r;
#ifdef TSCFIX_RESEARCH
    // trace_cascade=98: only the render modules after the shadow update (to the end of the frame).
    if (g_traceCascade == 98 && g_staggerFrame == g_traceAtFrame) overlap::ArmModuleTrace();
    // trace_cascade=99: trace the whole shadow update, then the render modules after it.
    if (g_traceCascade == 99 && g_staggerFrame == g_traceAtFrame) {
        r = tracer::TraceCall2([](void* s, void* a) { return g_cascades(s, a); }, self, arg, "shadow");
        overlap::ArmModuleTrace();  // traced only if the dispatch redirect is installed
    } else
#endif
    if (g_shadowOnWorker) {
        CascadeCall c{self, arg};
        r = g_shadowWorker.Run(&RunCascades, &c);
    } else if (t_binderSnapshotted && g_mapSplit && !g_mapSplitFailed) {
        r = CascadesOnTwoThreads(self, arg);
    } else {
        r = g_cascades(self, arg);
    }
    if (g_probeOn) {
        QueryPerformanceCounter(&t1);
        g_shadowTicks += t1.QuadPart - t0.QuadPart;
    }
    if (bias) *bias = savedBias;
    if (g_traceFrame) Log("trace: shadow update end");
    ExecutePendingClear();
#ifdef TSCFIX_RESEARCH
    if (privateState) pipeline::EndWorkerPass(), binder::LeaveShadow();
#endif
    if (privateState) overlapstate::SetShadowRole(false);
    g_inShadow = false;
    return r;
}

// Overlap: the cascade drawing runs on the worker while the game thread goes on with the rest
// of the shadow module (restoring the original target) and the following render modules.
// Per-thread state is snapshotted on the game thread before the worker starts.
struct CascadeJob {
    void* self;
    void* arg;
};
CascadeJob g_cascadeJobs[4];
unsigned g_cascadeJobNext = 0;

// The shadow pass threw a C++ exception on the worker (the engine has no catch frame there): the
// shadow pass stays on the game thread from then on instead of ending the process.
volatile bool g_shadowWorkerFailed = false;

uintptr_t CascadeJobMain(void* p) {
    CascadeJob job = *static_cast<CascadeJob*>(p);
    t_binderSnapshotted = true;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    try {
        ShadowCascadesBody(job.self, job.arg);
    } catch (...) {
        g_shadowWorkerFailed = true;
        Log("cascades: the shadow pass threw an exception on its worker; it runs on the game thread from now on");
    }
    QueryPerformanceCounter(&t1);
    overlap::AddShadowTime(t1.QuadPart - t0.QuadPart);  // what the overlap hides
    t_binderSnapshotted = false;
    rt::SegmentDone();  // if it forked ring workers it moved into a side segment: that one is done
    return 0;
}

void BeforeCascadeLaunch(DWORD worker) {
    ReadSceneryFlags();
#ifdef TSCFIX_RESEARCH
    binder::EnterShadowFor(worker);
    pipeline::PrepareWorker(binder::Copy());
    PrepareMapSplit();
#else
    (void)worker;
#endif
}

// Callers whose argument lives on their own stack and whose shadow map is freed right after
// the call (a one-off shadow render, 0x148 bytes, located by fingerprint): the cascades must
// finish before it returns, so they run inline.
constexpr uintptr_t kInlineOnlyCallerSize = 0x148;
bool CalledInlineOnly(void* ret) {
    static HMODULE engine = GetModuleHandleA("GameManagerVC64.dll");
    static uintptr_t caller = engine ? engine::Resolved(engine)[engine::ShadowOneOffCaller] : 0;
    if (!caller) return false;
    auto r = reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(engine);
    return r >= caller && r < caller + kInlineOnlyCallerSize;
}

uintptr_t ShadowCascades(void* self, void* arg) {
    if (g_traceCascade < 0 && !g_shadowWorkerFailed && !CalledInlineOnly(_ReturnAddress())) {  // research tracing runs inline
        CascadeJob* job = &g_cascadeJobs[g_cascadeJobNext++ % 4];
        *job = {self, arg};
        if (overlap::LaunchCascades(&CascadeJobMain, job, &BeforeCascadeLaunch)) return 0;
    }
    // A shadow render that runs inline (a one-off render whose map is freed right after) uses
    // the engine's shared state as the stock engine does: nothing else may be drawing beside it.
    overlap::JoinIfOverlapping("inline shadow render");
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uintptr_t r = ShadowCascadesBody(self, arg);
    QueryPerformanceCounter(&t1);
    overlap::AddShadowTime(t1.QuadPart - t0.QuadPart);  // what the overlap would hide
    return r;
}

// Returns true when this cascade's draw should be skipped (matrix already restored).
bool SkipCascade() {
    int k = g_cascadeCall++;
    if (k >= g_maxCascades || k >= 8 || !g_matrixStride) return false;
    uint8_t* m = static_cast<uint8_t*>(g_shadowObj) + g_matrixOffset + k * g_matrixStride;
    int outer = g_maxCascades - 1;
    bool draw;
    if (!g_stagger || !g_haveSaved[k] || outer <= 0)
        draw = true;
    else if (g_splitMode)  // near cascade on even frames, all outer cascades on odd frames
        draw = (k == 0) == (g_staggerFrame % 2 == 0);
    else                   // near cascade every frame, outer cascades in turn
        draw = k == 0 || static_cast<int>(g_staggerFrame % outer) == k - 1;
    if (draw) {
        memcpy(g_savedMatrix[k], m, 64);
        g_haveSaved[k] = true;
        ++g_cascadeDraws;
        return false;
    }
    memcpy(m, g_savedMatrix[k], 64);
    ++g_cascadeSkips;
    return true;
}


void SceneryRender(void* self, int pass) {
    if (g_traceFrame) Log("trace: cascade %d scenery draw", g_cascadeCall);
#ifdef TSCFIX_RESEARCH
    if (g_traceMain && !g_inShadow && pass == 32) {
        g_traceMain = false;
        struct Call { void* self; int pass; } c{self, pass};
        tracer::TraceCall2([](void* p, void*) -> uintptr_t {
            auto* k = static_cast<Call*>(p);
            g_scenery(k->self, k->pass);
            return 0;
        }, &c, nullptr, "main");
        return;
    }
#endif
    if (g_inShadow) {
#ifdef TSCFIX_RESEARCH
        // trace_cascade=97: every shadow map's scenery draw of one shadow update, each as its own
        // access trace (casc0, casc1, ...): what two of them would share on two threads.
        if (g_traceCascade == 97 && g_staggerFrame == g_traceAtFrame) {
            char tag[16];
            sprintf_s(tag, "casc%d", g_cascadeCall++);
            struct Call { void* self; int pass; } c{self, pass};
            Log("research: tracing shadow map scenery draw as '%s'", tag);
            tracer::TraceCall2([](void* q, void*) -> uintptr_t {
                auto* k = static_cast<Call*>(q);
                g_scenery(k->self, k->pass);
                return 0;
            }, &c, nullptr, tag);
            return;
        }
        if (g_traceCascade >= 0 && g_staggerFrame == g_traceAtFrame && g_cascadeCall == g_traceCascade) {
            ++g_cascadeCall;
            Log("research: tracing shadow cascade %d", g_traceCascade);
            tracer::TraceArm();
            tracer::TraceCall(g_scenery, self, pass);
            return;
        }
#endif
        if (t_shadowMapIndex) {  // two shadow threads: no staggering (its bookkeeping is one thread's)
            g_scenery(self, pass);
            return;
        }
        if (SkipCascade()) {
            g_pendingClear.active = false;  // keep last frame's map in this region
            return;
        }
        ExecutePendingClear();
    }
    if (!g_probeOn) {
        g_scenery(self, pass);
        return;
    }
    uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_engine);
    if (g_inShadow) pass = 100 + g_cascadeCall - 1;  // shadow cascades: one row per cascade
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_scenery(self, pass);
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < g_probeCount; ++i)
        if (g_probe[i].key.caller == caller && g_probe[i].key.pass == pass) {
            ++g_probe[i].calls;
            g_probe[i].ticks += t1.QuadPart - t0.QuadPart;
            return;
        }
    if (g_probeCount < 32) g_probe[g_probeCount++] = {{caller, pass}, 1, t1.QuadPart - t0.QuadPart};
}

// --- speed limits ---------------------------------------------------------------------------------
// Track speed-limit query (game version v79.0a): returns the current limit and writes the
// next limit and its distance. Called for every train; the call site that serves the
// player's train is taken from local configuration once identified by the probe below.
constexpr sites::Fingerprint kSpeedLimit = {0x8aa8d56dc0ed3ea6ull, 16};
using SpeedLimitFn = float (*)(void* follower, float a, bool b, float* next, float* dist);
SpeedLimitFn g_speedLimit = nullptr;

struct LimitStat {
    uintptr_t caller;
    uint64_t calls;
    void* keys[8];
    int keyCount;
    float cur, next, dist;
};
LimitStat g_limitStats[16];
int g_limitStatCount = 0;
uintptr_t g_playerCaller = 0;  // from config; 0 = probe only
Telemetry* g_telemetry = nullptr;

float SpeedLimit(void* follower, float a, bool b, float* next, float* dist) {
    float cur = g_speedLimit(follower, a, b, next, dist);
    uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_engine);
    float n = next ? *next : -1.0f, d = dist ? *dist : -1.0f;
    if (g_playerCaller && caller == g_playerCaller && g_telemetry) {
        g_telemetry->speedLimit = cur;
        g_telemetry->nextSpeedLimit = n;
        g_telemetry->nextSpeedLimitDistance = d;
        ++g_telemetry->speedLimitUpdates;
    }
    if (!g_probeOn) return cur;
    LimitStat* s = nullptr;
    for (int i = 0; i < g_limitStatCount; ++i)
        if (g_limitStats[i].caller == caller) s = &g_limitStats[i];
    if (!s && g_limitStatCount < 16) s = &g_limitStats[g_limitStatCount++], *s = {caller};
    if (!s) return cur;
    ++s->calls;
    bool seen = false;
    for (int i = 0; i < s->keyCount; ++i) seen |= s->keys[i] == follower;
    if (!seen && s->keyCount < 8) s->keys[s->keyCount++] = follower;
    s->cur = cur, s->next = n, s->dist = d;
    return cur;
}

// Lowest line speed limit query used by the scripts' GetCurrentSpeedLimit (v79.0a):
// (follower, float, bool, property**, float*, float*, float*) -> current limit.
constexpr sites::Fingerprint kLineLimit = {0x19881ce9c699575dull, 24};
using LineLimitFn = float (*)(void* follower, float a, bool b, void** prop, float* o1, float* o2, float* o3);
LineLimitFn g_lineLimit = nullptr;

float LineLimit(void* follower, float a, bool b, void** prop, float* o1, float* o2, float* o3) {
    float cur = g_lineLimit(follower, a, b, prop, o1, o2, o3);
    uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(g_engine);
    float n = o2 ? *o2 : -1.0f, d = o3 ? *o3 : -1.0f;
    if (g_playerCaller && caller == g_playerCaller && g_telemetry) {
        g_telemetry->speedLimit = cur;
        g_telemetry->nextSpeedLimit = n;
        g_telemetry->nextSpeedLimitDistance = d;
        ++g_telemetry->speedLimitUpdates;
    }
    if (!g_probeOn) return cur;
    LimitStat* s = nullptr;
    for (int i = 0; i < g_limitStatCount; ++i)
        if (g_limitStats[i].caller == caller) s = &g_limitStats[i];
    if (!s && g_limitStatCount < 16) s = &g_limitStats[g_limitStatCount++], *s = {caller};
    if (!s) return cur;
    ++s->calls;
    void* key = follower ? *static_cast<void**>(follower) : nullptr;  // InterfacePtr passed by reference
    bool seen = false;
    for (int i = 0; i < s->keyCount; ++i) seen |= s->keys[i] == key;
    if (!seen && s->keyCount < 8) s->keys[s->keyCount++] = key;
    s->cur = cur, s->next = n, s->dist = d;
    return cur;
}

// HUD driver's guide (v79.0a), called for the player's train only:
//   next speed limit:   bool (site&, direction&, float& distance, int& limit)
//   next signal:        float (site&, direction&, InterfacePtr<signal>&) -> distance
constexpr sites::Fingerprint kHudNextLimit = {0xb72e3c97d7a2f44dull, 16};
constexpr sites::Fingerprint kHudSignal = {0x8f10fb60c03bb2b2ull, 24};
using HudNextLimitFn = bool (*)(void* site, void* dir, float* dist, int* limit);
using HudSignalFn = float (*)(void* site, void* dir, void* signalOut);
HudNextLimitFn g_hudNextLimit = nullptr;
HudSignalFn g_hudSignal = nullptr;

bool HudNextLimit(void* site, void* dir, float* dist, int* limit) {
    bool found = g_hudNextLimit(site, dir, dist, limit);
    if (g_telemetry) {
        g_telemetry->hudNextLimit = found && limit ? static_cast<float>(*limit) : -1.0f;
        g_telemetry->hudNextLimitDistance = found && dist ? *dist : -1.0f;
        ++g_telemetry->hudUpdates;
    }
    return found;
}

float HudSignal(void* site, void* dir, void* signalOut) {
    float d = g_hudSignal(site, dir, signalOut);
    if (g_telemetry) {
        g_telemetry->hudSignalDistance = d;
        ++g_telemetry->hudUpdates;
    }
    return d;
}

bool HookScenery(HMODULE engine) {
    if (g_scenery) return true;
    g_engine = engine;
    void* target = sites::Find(engine, kSceneryRender);
    if (!target) {
        Log("experiments: scenery render not found; disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&SceneryRender), reinterpret_cast<void**>(&g_scenery)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    if (!ok) Log("experiments: scenery hook failed");
    if (void* flags = sites::Find(engine, kSceneryFlags)) {
        if (MH_CreateHook(flags, reinterpret_cast<void*>(&SceneryFlags), reinterpret_cast<void**>(&g_sceneryFlags)) != MH_OK ||
            MH_EnableHook(flags) != MH_OK)
            g_sceneryFlags = nullptr;
    }
    if (!g_sceneryFlags) Log("experiments: scenery flags not found; shadow threads read them from the scenario themselves");
    return ok;
}

}  // namespace

bool InstallSceneryProbe(HMODULE engine) {
    g_probeOn = HookScenery(engine);
    Log("experiments: scenery probe %s", g_probeOn ? "installed" : "failed");
    return g_probeOn;
}

bool InstallCascadeStagger(HMODULE engine, int matrixOffset, int matrixStride, int cascades) {
    if (g_cascades) return true;
    if (matrixOffset <= 0 || matrixStride < 64 || cascades < 2) {
        Log("experiments: cascade stagger needs local configuration; disabled");
        return false;
    }
    g_matrixOffset = matrixOffset;
    g_matrixStride = matrixStride;
    g_maxCascades = cascades;
    void* target = sites::Find(engine, kShadowCascades);
    if (!target || !HookScenery(engine)) {
        Log("experiments: shadow cascade loop not found; disabled");
        return false;
    }
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&ShadowCascades), reinterpret_cast<void**>(&g_cascades)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    Log("experiments: cascade stagger %s (%d cascades)", ok ? "installed" : "failed", cascades);
    return ok;
}

bool InstallShadowLod(HMODULE engine, int biasOffset) {
    if (biasOffset <= 0) {
        Log("experiments: shadow LOD needs local configuration; disabled");
        return false;
    }
    if (!g_cascades) {
        Log("experiments: shadow LOD needs the shadow cascade hook; disabled");
        return false;
    }
    auto* fn = static_cast<uint8_t*>(sites::Find(engine, kLodSelect));
    if (!fn) {
        Log("experiments: LOD selector not found; disabled");
        return false;
    }
    // First "mov rsi, [rip+disp32]" (48 8B 35) in the prologue loads the settings object.
    for (int i = 0; i < 48; ++i) {
        if (fn[i] == 0x48 && fn[i + 1] == 0x8B && fn[i + 2] == 0x35) {
            int32_t disp;
            memcpy(&disp, fn + i + 3, 4);
            g_lodSettingsVar = reinterpret_cast<void**>(fn + i + 7 + disp);
            break;
        }
    }
    g_lodBiasOffset = biasOffset;
    Log("experiments: shadow LOD %s", g_lodSettingsVar ? "ready" : "failed (settings load not found)");
    return g_lodSettingsVar != nullptr;
}

void SetShadowLod(bool active, int bias) {
    g_shadowLodBias = active ? bias : 0;
    char text[64];
    sprintf_s(text, "shadow LOD +%d %s", bias, active ? "ON" : "OFF");
    status::Set("shadowlod", text);
}

bool InstallMapSplit(HMODULE engine, int map) {
    if (!g_cascades) return false;
    void* inMap = sites::Find(engine, kInCurrentMap);
    g_viewportCtor = static_cast<ViewportCtorFn>(sites::Find(engine, kLightViewportCtor));
    bool ok = inMap && g_viewportCtor && overlap::StartSplitWorker() &&
              MH_CreateHook(inMap, reinterpret_cast<void*>(&InCurrentMap), reinterpret_cast<void**>(&g_inCurrentMap)) == MH_OK &&
              MH_EnableHook(inMap) == MH_OK;
    g_splitMapConfig = map;
    Log("experiments: shadow maps on two threads %s", ok ? "installed" : "not available (game version changed?)");
    return ok;
}

void SetMapSplit(bool on) {
#ifdef TSCFIX_RESEARCH
    pipeline::SetSplit(on);
#endif
    g_mapSplit = on;
}

void SetShadowSplit(bool active) {
    g_splitMode = true;
    g_stagger = active;
    status::Set("shadow", active ? "shadow split near/outer ON" : "shadow split near/outer OFF");
}

bool InstallShadowWorker() {
    if (!g_cascades) {
        Log("experiments: shadow worker needs the shadow cascade hook; disabled");
        return false;
    }
    bool ok = g_shadowWorker.Start(L"tscfix shadow");
    Log("experiments: shadow worker %s", ok ? "started" : "failed");
    return ok;
}

void SetShadowWorker(bool active) {
    g_shadowOnWorker = active;
    status::Set("shadowworker", active ? "shadow pass on worker ON" : "shadow pass on worker OFF");
}

void SetCascadeStagger(bool active) {
    g_splitMode = false;
    g_stagger = active;
    status::Set("shadow", active ? "shadow cascades staggered ON" : "shadow cascades staggered OFF");
}

bool InstallSpeedLimits(HMODULE engine, uintptr_t playerCaller) {
    g_engine = engine;
    g_playerCaller = playerCaller;
    g_telemetry = TelemetryBlock();
    void* target = sites::Find(engine, kSpeedLimit);
    void* line = sites::Find(engine, kLineLimit);
    if (!target || !line) {
        Log("experiments: speed limit queries not found; disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&SpeedLimit), reinterpret_cast<void**>(&g_speedLimit)) == MH_OK &&
              MH_EnableHook(target) == MH_OK &&
              MH_CreateHook(line, reinterpret_cast<void*>(&LineLimit), reinterpret_cast<void**>(&g_lineLimit)) == MH_OK &&
              MH_EnableHook(line) == MH_OK;
    void* hudLimit = sites::Find(engine, kHudNextLimit);
    void* hudSignal = sites::Find(engine, kHudSignal);
    bool hud = hudLimit && hudSignal &&
               MH_CreateHook(hudLimit, reinterpret_cast<void*>(&HudNextLimit), reinterpret_cast<void**>(&g_hudNextLimit)) == MH_OK &&
               MH_EnableHook(hudLimit) == MH_OK &&
               MH_CreateHook(hudSignal, reinterpret_cast<void*>(&HudSignal), reinterpret_cast<void**>(&g_hudSignal)) == MH_OK &&
               MH_EnableHook(hudSignal) == MH_OK;
    Log("experiments: HUD driver's guide hooks %s", hud ? "installed" : "failed");
    Log("experiments: speed limits %s (player call site %s)", ok ? "hooked" : "failed",
        playerCaller ? "configured" : "not configured, probe only");
    return ok;
}

bool Tracing() { return g_traceFrame; }

void SetResearchTrace(int cascade, int frame) {
    g_traceCascade = cascade;
    g_traceAtFrame = frame > 0 ? frame : 600;
}

bool HoldShadowClear(IDirect3DDevice9* device, DWORD rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if (!g_stagger || !g_inShadow || rects != 0) return false;
    ExecutePendingClear();  // two clears in a row: the first one belonged to no cascade
    g_pendingClear = {true, device, flags, color, z, stencil};
    return true;
}

void FlushShadowClear() { ExecutePendingClear(); }

void OnFrame() {
    if (g_traceFrame) {
        Log("trace: Present");
        g_traceFrame = false;
    }
    if (g_cascades && ++g_cascadeFrames % 300 == 0) {
        Log("cascades: last 300 frames: shadow updates %llu, cascade draws/frame %.2f, skips/frame %.2f (stagger %s)",
            static_cast<unsigned long long>(g_staggerFrame), g_cascadeDraws / 300.0, g_cascadeSkips / 300.0,
            g_stagger ? "on" : "off");
        g_cascadeDraws = g_cascadeSkips = 0;
    }
    if (g_telemetry) ++g_telemetry->frame;
    if (!g_probeOn || ++g_probeFrames % 300 != 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < g_limitStatCount; ++i) {
        const LimitStat& s = g_limitStats[i];
        Log("probe: speed limit caller=+0x%llx calls/frame=%.2f trains=%d cur=%.1f next=%.1f dist=%.0f",
            static_cast<unsigned long long>(s.caller), s.calls / 300.0, s.keyCount, s.cur, s.next, s.dist);
    }
    g_limitStatCount = 0;
    for (int i = 0; i < g_probeCount; ++i) {
        const ProbeStat& s = g_probe[i];
        Log("probe: scenery render caller=+0x%llx pass=%d calls/frame=%.2f ms/call=%.2f ms/frame=%.2f",
            static_cast<unsigned long long>(s.key.caller), s.key.pass, s.calls / 300.0,
            s.calls ? s.ticks * 1000.0 / f.QuadPart / s.calls : 0.0, s.ticks * 1000.0 / f.QuadPart / 300.0);
    }
    g_probeCount = 0;
    if (g_cascades)
        Log("probe: shadow update ms/frame=%.2f", g_shadowTicks * 1000.0 / f.QuadPart / 300.0);
    g_shadowTicks = 0;
}

bool InstallShadowSkip(HMODULE engine, int every) {
    g_every = every > 1 ? every : 2;
    void* target = sites::Find(engine, kShadowUpdate);
    if (!target) {
        Log("experiments: shadow map update not found (game version changed?); disabled");
        return false;
    }
    MH_Initialize();
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ShadowUpdate), reinterpret_cast<void**>(&g_shadowUpdate)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("experiments: shadow hook failed");
        return false;
    }
    Log("experiments: shadow map update hooked (every %d frames when active)", g_every);
    return true;
}

void SetShadowSkip(bool active) {
    g_shadowSkip = active;
    char text[64];
    sprintf_s(text, "shadow map every %d frames %s", g_every, active ? "ON" : "OFF");
    status::Set("shadow", text);
}

}  // namespace tscfix::experiments
