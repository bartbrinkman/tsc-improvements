#include "glitch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <string>

#include "log.h"
#include "overlap.h"
#include "rtdevice.h"

namespace tscfix::glitch {
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

constexpr int kW = 384, kH = 216;
constexpr int kThreshold = 24;   // luminance step (of 255) that counts as a change
constexpr int kMinPixels = 10;   // pixels in one compact patch for an event
constexpr int kMaxSaved = 24;     // event frames written to disk

bool g_enabled = false;
std::map<const void*, std::string> g_rtNames;  // "WxH format" per render target seen
DWORD g_vpX = 0;  // x of the viewport set last (cascade within an atlas)

// Draw log: per frame, every indexed draw with the state bound for it.
struct Draw {
    const void* state[kStateKinds];
    const void* rt;  // render target 0 when drawn
    DWORD vpX;       // viewport x (the cascade within a shadow atlas)
    INT base;
    UINT start, count;
    uintptr_t caller;
    DWORD tid;
    uintptr_t texCaller;  // who recorded the tex0 binding this draw used
    DWORD texTid;
    uintptr_t fxCaller;  // who recorded the latest effect texture parameter before this draw
    DWORD fxTid;
    uint64_t fxMaterial, fxSlot, fxSlotObject;  // watched caller: material (r14), slot (rdi), slot entry
    uint32_t foreign;  // draws recorded by other threads replayed since that effect texture was set
    uint64_t texN;     // textures of stages 1 to 7, mixed into one value
    uint64_t rs, ss, misc;  // render states, sampler states, other bindings (depth surface, streams 1-3), each mixed into one value
    uint32_t cg[2][16];  // shader constants c0-c63 in groups of four registers, vertex and pixel, one mixed value per group
    DWORD slopeTid = 0;      // who set the slope-scaled depth bias last (0: not through SetRenderState)
    uint32_t slopeCaller = 0;
    uint32_t used[51];  // the values a draw depends on: 4 stages x 8 sampler states, 14 render states, tex0 LOD, instancing (stream 0 and 1 frequency, stream 1 buffer and offset)
};
uint32_t g_foreign = 0;
uintptr_t g_engineBase = 0;
uintptr_t g_fxCaller = 0;
DWORD g_fxTid = 0;
uint64_t g_fxMaterial = 0, g_fxSlot = 0, g_fxSlotObject = 0;
uintptr_t g_watchRva = 0;

// Writers of the watched slot entry's texture (+0x18): a hardware write breakpoint on every
// thread, armed once from a helper thread; hits tallied per (engine rva after the write, thread).
struct WatchHit {
    volatile LONG64 key;  // rva << 32 | thread id
    volatile LONG count;
};
WatchHit g_hits[64];
volatile LONG g_watchArmed = 0;
uintptr_t g_watchAddr = 0;
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    uintptr_t rip = e->ContextRecord->Rip;
    uintptr_t rva = rip >= g_engineBase && rip < g_engineBase + 0x2000000 ? rip - g_engineBase : 0xffffffff;
    LONG64 key = static_cast<LONG64>(rva) << 32 | GetCurrentThreadId();
    for (WatchHit& h : g_hits) {
        LONG64 k = h.key;
        if (!k) k = InterlockedCompareExchange64(&h.key, key, 0);
        if (!k || k == key) {
            InterlockedIncrement(&h.count);
            break;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
DWORD WINAPI ArmWatch(void*) {
    AddVectoredExceptionHandler(1, &WatchHandler);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = g_watchAddr;
                c.Dr7 = (c.Dr7 & ~0xf0003ull) | 1 | (1ull << 16) | (3ull << 18);  // L0, write, 8 bytes
                if (SetThreadContext(t, &c)) ++armed;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("glitch: watching writes to %p on %d thread(s)", reinterpret_cast<void*>(g_watchAddr), armed);
    return 0;
}
void LogWatch() {
    char line[900];
    int n = 0;
    for (const WatchHit& h : g_hits)
        if (h.key && n < 840)
            n = Append(line, sizeof line, n, " rva %llx thread %llu x%ld;",
                           static_cast<unsigned long long>(h.key >> 32), static_cast<unsigned long long>(h.key & 0xffffffff),
                           h.count);
    Log("glitch: watched entry writers:%s", n ? line : " none");
}
// The watched caller's texture slot entry, read at replay (material +0x40 + slot * 8).
uint64_t ReadSlot(uint64_t material, uint64_t slot) {
    __try {
        return *reinterpret_cast<const uint64_t*>(material + 0x40 + (slot & 0xffffffff) * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uintptr_t g_texCaller = 0;
DWORD g_texTid = 0;
uintptr_t g_caller = 0;
DWORD g_callerTid = 0;
std::vector<Draw> g_draws[3];  // ring like g_frames: oldest .. newest
std::vector<Draw> g_current;   // the frame being drawn
const void* g_state[kStateKinds] = {};
int g_drawDiffsLogged = 0;

bool SameGeometry(const Draw& a, const Draw& b) {
    return a.state[kVB] == b.state[kVB] && a.state[kIB] == b.state[kIB] && a.start == b.start && a.count == b.count &&
           a.base == b.base;
}
// Constant registers carry per-frame values (time, sun): they are compared only where the
// neighbours agree.
bool SameState(const Draw& a, const Draw& b) {
    for (int k = 0; k < kStateKinds; ++k)
        if (k != kVSConst && k != kPSConst && a.state[k] != b.state[k]) return false;
    return true;
}
// Every draw of d's geometry in a frame: stream position, technique, recording thread.
void Occurrences(const std::vector<Draw>& list, const Draw& d, char* out, size_t size) {
    size_t n = 0;
    int shown = 0;
    out[0] = 0;
    for (size_t i = 0; i < list.size() && shown < 5; ++i)
        if (SameGeometry(list[i], d)) {
            const char* t = list[i].state[kTechnique] ? static_cast<const char*>(list[i].state[kTechnique]) : "-";
            n = static_cast<size_t>(Append(out, size, static_cast<int>(n), " %zu:%s:%lu", i, t, list[i].tid));
            ++shown;
        }
}
bool ConstOnlyHere(const Draw& a, const Draw& m, const Draw& c, int k) {
    return a.state[k] == c.state[k] && m.state[k] != a.state[k];
}
const Draw* FindGeometry(const std::vector<Draw>& list, const Draw& d) {
    for (const Draw& x : list)
        if (SameGeometry(x, d)) return &x;
    return nullptr;
}
// The flagged (middle) frame against its neighbours: draws of geometry drawn identically in both
// neighbours that the middle frame drew with other state, or did not draw.
void DiffDraws(uint64_t frame) {
    if (g_drawDiffsLogged >= 300) return;
    const auto &a = g_draws[0], &m = g_draws[1], &c = g_draws[2];
    int changed = 0, missing = 0, logged = 0, shadowChanged = 0;
    // Missing draws by who drew them in the frame before (engine rva, thread): what was dropped.
    struct Tally { uintptr_t rva; DWORD tid; int n; };
    Tally tally[16] = {};
    for (const Draw& d : a) {
        const Draw* dc = FindGeometry(c, d);
        if (!dc || !SameState(d, *dc)) continue;  // not stable around the flagged frame
        const Draw* dm = FindGeometry(m, d);
        if (!dm) {
            ++missing;
            for (Tally& t : tally)
                if (!t.n || (t.rva == d.caller && t.tid == d.tid)) {
                    t.rva = d.caller, t.tid = d.tid, ++t.n;
                    break;
                }
            continue;
        }
        if (SameState(d, *dm) && !ConstOnlyHere(d, *dm, *dc, kVSConst) && !ConstOnlyHere(d, *dm, *dc, kPSConst)) continue;
        ++changed;
        if (d.state[kTechnique] != dm->state[kTechnique] && logged < 12) {
            char oa[200], om[200], oc[200];
            Occurrences(a, d, oa, sizeof oa), Occurrences(m, d, om, sizeof om), Occurrences(c, d, oc, sizeof oc);
            Log("glitch: frame %llu draw (vb %p ib %p start %u count %u) order before:%s | flagged:%s | after:%s",
                static_cast<unsigned long long>(frame), d.state[kVB], d.state[kIB], d.start, d.count, oa, om, oc);
        }
        if (d.state[kShadow] != dm->state[kShadow] && shadowChanged++ < 6)
            Log("glitch: frame %llu draw (vb %p ib %p start %u count %u ps %p) shadow stages %llx->%llx; recorded by "
                "engine rva %llx on thread %lu (before: rva %llx thread %lu)",
                static_cast<unsigned long long>(frame), d.state[kVB], d.state[kIB], d.start, d.count, dm->state[kPS],
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(d.state[kShadow])),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(dm->state[kShadow])),
                static_cast<unsigned long long>(dm->caller), dm->tid, static_cast<unsigned long long>(d.caller), d.tid);
        if (logged++ < 12) {
            static const char* names[kStateKinds] = {"vs", "ps", "ib", "vb", "tex0", "shadow stages", "vs constants",
                                                      "ps constants", "technique"};
            char what[320] = "";
            for (int k = 0; k < kStateKinds; ++k)
                if (d.state[k] != dm->state[k] && ((k != kVSConst && k != kPSConst) || ConstOnlyHere(d, *dm, *dc, k))) {
                    size_t len = strlen(what);
                    if (k == kTechnique)
                        Append(what, sizeof what, static_cast<int>(len), " technique %s->%s",
                                  d.state[k] ? static_cast<const char*>(d.state[k]) : "-",
                                  dm->state[k] ? static_cast<const char*>(dm->state[k]) : "-");
                    else
                        Append(what, sizeof what, static_cast<int>(len), " %s %p->%p", names[k], d.state[k], dm->state[k]);
                }
            Log("glitch: frame %llu draw (vb %p ib %p start %u count %u) differs:%s; recorded by engine rva %llx on "
                "thread %lu, effect texture by %llx on thread %lu material %llx slot %llu entry %llx foreign %u (before: "
                "rva %llx thread %lu, effect texture by %llx thread %lu material %llx slot %llu entry %llx foreign %u)",
                static_cast<unsigned long long>(frame), d.state[kVB], d.state[kIB], d.start, d.count, what,
                static_cast<unsigned long long>(dm->caller), dm->tid, static_cast<unsigned long long>(dm->fxCaller),
                dm->fxTid, dm->fxMaterial, dm->fxSlot, dm->fxSlotObject, dm->foreign,
                static_cast<unsigned long long>(d.caller), d.tid, static_cast<unsigned long long>(d.fxCaller), d.fxTid,
                d.fxMaterial, d.fxSlot, d.fxSlotObject, d.foreign);
        }
    }
    Log("glitch: frame %llu draws: %zu / %zu / %zu (before / flagged / after); stable draws changed %d (shadow stages %d), missing %d",
        static_cast<unsigned long long>(frame), a.size(), m.size(), c.size(), changed, shadowChanged, missing);
    {  // shadow casters (depth techniques) drawn in both neighbours but not in the flagged frame
        auto isDepth = [](const Draw& x) {
            return x.state[kTechnique] && strstr(static_cast<const char*>(x.state[kTechnique]), "DEPTH");
        };
        auto sameTech = [](const Draw& x, const Draw& y) {
            return x.state[kTechnique] && y.state[kTechnique] &&
                   !strcmp(static_cast<const char*>(x.state[kTechnique]), static_cast<const char*>(y.state[kTechnique]));
        };
        auto has = [&](const std::vector<Draw>& list, const Draw& d) {
            for (const Draw& x : list)
                if (SameGeometry(x, d) && sameTech(x, d)) return true;
            return false;
        };
        std::map<std::tuple<DWORD, std::string, uintptr_t>, int> byThread;
        int dropped = 0, depthDraws = 0;
        for (const Draw& d : a) {
            if (!isDepth(d)) continue;
            ++depthDraws;
            if (has(c, d) && !has(m, d)) {
                ++dropped, ++byThread[{d.tid, static_cast<const char*>(d.state[kTechnique]), d.caller}];
                if (dropped <= 3)
                    Log("glitch: frame %llu dropped caster: %s vb %p ib %p start %u count %u",
                        static_cast<unsigned long long>(frame), static_cast<const char*>(d.state[kTechnique]), d.state[kVB],
                        d.state[kIB], d.start, d.count);
            }
        }
        int added = 0;  // depth draws only in the flagged frame (the same casters with other buffers?)
        std::map<std::string, int> addedBy;
        for (const Draw& d : m)
            if (isDepth(d) && !has(a, d) && !has(c, d)) {
                ++added;
                char k[96];
                sprintf_s(k, "%s vb %p ib %p", static_cast<const char*>(d.state[kTechnique]), d.state[kVB], d.state[kIB]);
                ++addedBy[k];
            }
        if (added) {
            char al[500];
            int an = 0;
            for (auto& [k, v] : addedBy)
                if (an < 440) an = Append(al, sizeof al, an, " %s x%d;", k.c_str(), v);
            Log("glitch: frame %llu depth draws only in it: %d;%s", static_cast<unsigned long long>(frame), added, al);
        }
        char line[400];
        int n = 0;
        for (auto& [key, k] : byThread)
            if (n < 360)
                n = Append(line, sizeof line, n, " thread %lu %s rva %llx x%d;", std::get<0>(key),
                               std::get<1>(key).c_str(), static_cast<unsigned long long>(std::get<2>(key)), k);
        Log("glitch: frame %llu shadow casters dropped: %d of %d depth draws%s%s", static_cast<unsigned long long>(frame),
            dropped, depthDraws, n ? "; drawn before by" : "", n ? line : "");
    }
    if (missing) {
        char line[600];
        int n = 0;
        for (const Tally& t : tally)
            if (t.n && n < 560)
                n = Append(line, sizeof line, n, " rva %llx thread %lu x%d;", static_cast<unsigned long long>(t.rva),
                               t.tid, t.n);
        Log("glitch: frame %llu missing draws were recorded (before) by:%s", static_cast<unsigned long long>(frame), line);
    }
    ++g_drawDiffsLogged;
}
IDirect3DSurface9* g_small = nullptr;   // default pool render target, kW x kH
IDirect3DSurface9* g_sys = nullptr;     // system memory copy
D3DFORMAT g_format = D3DFMT_UNKNOWN;
std::vector<uint8_t> g_frames[3];       // ring: oldest .. newest
int g_have = 0;
uint64_t g_frame = 0, g_events = 0, g_flagged = 0, g_windowEvents = 0;
int g_saved = 0;

bool Ensure(IDirect3DDevice9* d, D3DFORMAT fmt) {
    if (g_small && g_format == fmt) return true;
    if (g_small) g_small->Release(), g_small = nullptr;
    if (g_sys) g_sys->Release(), g_sys = nullptr;
    g_format = fmt;
    if (FAILED(d->CreateRenderTarget(kW, kH, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_small, nullptr))) return false;
    if (FAILED(d->CreateOffscreenPlainSurface(kW, kH, fmt, D3DPOOL_SYSTEMMEM, &g_sys, nullptr))) return false;
    return true;
}

void Save(const std::vector<uint8_t>& f, uint64_t frame, char tag) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) *slash = 0;
    char name[MAX_PATH];
    sprintf_s(name, "%s\\tscfix-glitch-%06llu-%c.pgm", path, static_cast<unsigned long long>(frame), tag);
    FILE* out = nullptr;
    if (fopen_s(&out, name, "wb") || !out) return;
    fprintf(out, "P5\n%d %d\n255\n", kW, kH);
    fwrite(f.data(), 1, f.size(), out);
    fclose(out);
}

// Where events happen, on a coarse 24 x 12 grid: a flickering object fires again and again in
// the same cell, motion noise does not.
uint32_t g_cells[12][24] = {};

// The middle frame of the ring against its neighbours. A pixel is flagged when it moved away
// from both neighbours in the same direction while they agree; flagged pixels are grouped into
// 8-connected patches, and only a compact, dense patch counts (scattered pixels along moving
// edges and repeating patterns at a low frame rate do not).

}  // namespace
extern std::vector<std::string> g_variantNames;
extern int g_variant;
namespace {

// --- oracle: threaded frames against the serial frames around them ---------------------------
bool g_oracle = false;
bool g_step = false;                     // oracle step mode (see OracleOnFrame)
bool g_stepCalibrate = false;            // step mode, both twins rendered stock (the method's noise floor)
int g_stepThreaded = 1;                  // step mode: threaded renders per stock render of one state
volatile int g_stepPos = 0;              // position of the frame being drawn in its step cycle (0 = stock)
int g_curStepPos = 0;
volatile bool g_skipNextUpdate = false;
volatile uint8_t g_nextMode = 0, g_nextStepPos = 0;  // set by the stream marker: mode of the frame being drawn next
volatile uint32_t g_nextMarkerSeq = 0;               // the recorded frame the marker belongs to
uint32_t g_curMarkerSeq = 0, g_prevPresentedSeq = 0;
uint64_t g_seqBreaks = 0;
uint8_t g_curMode = 0;  // mode of the frame being drawn: 0 serial, 1 threaded
struct DrawKey {
    const void* rt;
    DWORD vpX;
    uintptr_t caller;
    const void* technique;
    const void* vb;
    const void* ib;
    INT base;
    UINT start, count;
    bool operator==(const DrawKey& o) const {
        return rt == o.rt && vpX == o.vpX && caller == o.caller && technique == o.technique && vb == o.vb && ib == o.ib &&
               base == o.base && start == o.start && count == o.count;
    }
};
struct DrawKeyHash {
    size_t operator()(const DrawKey& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(reinterpret_cast<uintptr_t>(k.rt)), mix(k.vpX), mix(k.caller), mix(reinterpret_cast<uintptr_t>(k.technique));
        mix(reinterpret_cast<uintptr_t>(k.vb)), mix(reinterpret_cast<uintptr_t>(k.ib));
        mix(static_cast<uint32_t>(k.base)), mix(k.start), mix(k.count);
        return static_cast<size_t>(h);
    }
};
struct KeyInfo {
    int n = 0;
    const Draw* first = nullptr;
};
using FrameMap = std::unordered_map<DrawKey, KeyInfo, DrawKeyHash>;
DrawKey KeyOf(const Draw& d) {
    return {d.rt, d.vpX, d.caller, d.state[kTechnique], d.state[kVB], d.state[kIB], d.base, d.start, d.count};
}
void Index(const std::vector<Draw>& list, FrameMap& m) {
    m.clear();
    m.reserve(list.size() * 2);
    for (const Draw& d : list) {
        KeyInfo& k = m[KeyOf(d)];
        if (!k.n++) k.first = &d;
    }
}
bool SameDrawState(const Draw& a, const Draw& b) {
    static const int kinds[] = {kVS, kPS, kTex0, kShadow};
    for (int k : kinds)
        if (a.state[k] != b.state[k]) return false;
    return true;
}
struct Defect {
    uint64_t missing = 0, extra = 0, state = 0, frames = 0;
    DWORD tid = 0;
    std::map<DWORD, uint64_t> threadedBy;  // threaded frames: recording thread of each copy drawn
};
std::unordered_map<DrawKey, Defect, DrawKeyHash> g_defects;
uint64_t g_triples = 0, g_triplesWithDefect = 0, g_missingTotal = 0, g_extraTotal = 0, g_stateTotal = 0;
uint64_t g_reference = 0;  // draws both serial frames agreed on, summed
struct VariantOracle {
    uint64_t triples = 0, missing = 0, extra = 0, state = 0;
};
std::vector<VariantOracle> g_variantOracle;  // per runtime-switch variant (glitch_variants)
int g_oracleVariant[5] = {-1, -1, -1, -1, -1};  // variant each ring frame was drawn under

std::string ThreadName(DWORD tid) {
    std::string name = std::to_string(tid);
    if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc && *desc) {
            char narrow[64];
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof narrow, nullptr, nullptr);
            name += std::string("(") + narrow + ")";
        }
        if (desc) LocalFree(desc);
        CloseHandle(t);
    }
    return name;
}
// Pixel pick ([experiments] glitch_pick_x / glitch_pick_y in window pixels, glitch_pick_frame=N,
// glitch_pick_every=frames, glitch_pick_times=count): in a pick frame every indexed draw to a
// target shaped like the window runs under a one-pixel scissor inside an occlusion query, and
// each draw that reaches the pixel is logged with its state and the textures bound (size,
// format, first bytes of the smallest mip level). Answers "which draws make this pixel, with
// which textures" for a defect that stays on screen. glitch_pick_consts=N also logs the float
// constants c0 to cN-1 of both shader stages for each of those draws.
// glitch_pick_key=VK: each press of that key starts glitch_pick_times picks (instead of glitch_pick_frame).
int g_pickX = -2, g_pickY = 0, g_pickEvery = 240, g_pickTimes = 0, g_pickConsts = 0, g_pickKey = 0, g_pickPerPress = 0;
uint64_t g_pickFrame = 0;
UINT g_backW = 0;
IDirect3DQuery9* g_pickQuery = nullptr;
DWORD g_pickSavedEnable = 0;
RECT g_pickSavedRect{};
int g_pickHits = 0;
void PickInit() {
    if (g_pickX == -2) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_pickX = GetPrivateProfileIntA("experiments", "glitch_pick_x", -1, ini);
        g_pickY = GetPrivateProfileIntA("experiments", "glitch_pick_y", 0, ini);
        g_pickFrame = GetPrivateProfileIntA("experiments", "glitch_pick_frame", 300, ini);
        g_pickEvery = GetPrivateProfileIntA("experiments", "glitch_pick_every", 240, ini);
        g_pickTimes = GetPrivateProfileIntA("experiments", "glitch_pick_times", 3, ini);
        g_pickConsts = GetPrivateProfileIntA("experiments", "glitch_pick_consts", 0, ini);
        if (g_pickConsts > 64) g_pickConsts = 64;
        g_pickKey = GetPrivateProfileIntA("experiments", "glitch_pick_key", 0, ini);
        if (g_pickKey) g_pickFrame = ~0ull, g_pickPerPress = g_pickTimes, g_pickTimes = 0;
    }
}
bool PickFrame() {
    PickInit();
    return g_pickX >= 0 && g_pickTimes > 0 && g_backW && g_frame == g_pickFrame;
}
int TexInfo(IDirect3DDevice9* d, DWORD stage, char* out, size_t size, int n) {
    IDirect3DBaseTexture9* b = nullptr;
    if (FAILED(d->GetTexture(stage, &b)) || !b) return n;
    if (b->GetType() != D3DRTYPE_TEXTURE) {
        n = Append(out, size, n, " s%lu %p (type %d);", stage, static_cast<void*>(b), static_cast<int>(b->GetType()));
    } else {
        auto* t = static_cast<IDirect3DTexture9*>(b);
        D3DSURFACE_DESC de{};
        t->GetLevelDesc(0, &de);
        DWORD levels = t->GetLevelCount();
        uint8_t px[16] = {};
        D3DLOCKED_RECT lr{};
        bool read = false;
        if (de.Pool != D3DPOOL_DEFAULT && SUCCEEDED(t->LockRect(levels - 1, &lr, nullptr, D3DLOCK_READONLY)) && lr.pBits) {
            memcpy(px, lr.pBits, 8);
            t->UnlockRect(levels - 1);
            read = true;
        }
        n = Append(out, size, n, " s%lu %p %ux%u fmt %d pool %d levels %lu", stage, static_cast<void*>(b), de.Width, de.Height,
                   static_cast<int>(de.Format), static_cast<int>(de.Pool), levels);
        if (read) n = Append(out, size, n, " last %02x%02x%02x%02x%02x%02x%02x%02x", px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
        n = Append(out, size, n, ";");
    }
    b->Release();
    return n;
}

void OracleReport() {
    std::vector<std::pair<DrawKey, Defect>> top(g_defects.begin(), g_defects.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        return a.second.missing + a.second.extra + a.second.state > b.second.missing + b.second.extra + b.second.state;
    });
    Log("oracle: %llu serial/threaded/serial triples (same frame parity), %llu with defects; per triple: %.1f draws missing, %.1f extra, "
        "%.1f with other state (of %.0f the serial frames agree on)",
        static_cast<unsigned long long>(g_triples), static_cast<unsigned long long>(g_triplesWithDefect),
        g_triples ? double(g_missingTotal) / g_triples : 0.0, g_triples ? double(g_extraTotal) / g_triples : 0.0,
        g_triples ? double(g_stateTotal) / g_triples : 0.0, g_triples ? double(g_reference) / g_triples : 0.0);
    for (size_t i = 0; i < top.size() && i < 20; ++i) {
        const DrawKey& k = top[i].first;
        const Defect& d = top[i].second;
        auto rt = g_rtNames.find(k.rt);
        char by[400] = "";
        int bn = 0;
        for (auto& [tid, c] : d.threadedBy)
            if (bn < 340)
                bn = Append(by, sizeof by, bn, " %s x%llu", ThreadName(tid).c_str(), static_cast<unsigned long long>(c));
        Log("oracle:     threaded copies by thread:%s", by);
        Log("oracle:   target %s viewport x %lu, engine rva %llx, %s, vb %p ib %p start %u count %u: missing %llu, extra %llu, state %llu "
            "(in %llu triples; serial frames drew it on thread %s)",
            rt != g_rtNames.end() ? rt->second.c_str() : "?", k.vpX, static_cast<unsigned long long>(k.caller),
            k.technique ? static_cast<const char*>(k.technique) : "-", k.vb, k.ib, k.start, k.count,
            static_cast<unsigned long long>(d.missing), static_cast<unsigned long long>(d.extra),
            static_cast<unsigned long long>(d.state), static_cast<unsigned long long>(d.frames), ThreadName(d.tid).c_str());
    }
}

// The engine alternates work between frames (shadow cascades: 4 one frame, 2 the next), so a
// threaded frame is compared with the serial frames two before and two after it (same parity):
// modes run S S T T, and a frame ring of five is kept.
std::vector<Draw> g_oracleRing[5];
uint8_t g_oracleModes[5] = {};
int g_oracleFilled = 0;
// Step mode: a threaded frame against its stock twin (same world state), draw by draw. The key
// includes texture and shaders, so a draw with other state shows as one missing plus one extra.
std::vector<Draw> g_twinStock;
bool g_haveTwin = false;
int g_twinLogged = 0;
bool g_bigDiff = false;
bool g_constsLogged = false;  // this frame's first main-view pre-pass constants are logged ([experiments] oracle_log_consts)
// Fail early ([experiments] oracle_stop_px): the first threaded frame with a region at least this big
// (past start-up) is saved and its twins logged, then the game ends.
int g_stopPx = -1;
bool g_stopPending = false;
char g_stopWhy[200] = "";
int StopPx() {
    if (g_stopPx < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_stopPx = GetPrivateProfileIntA("experiments", "oracle_stop_px", 0, ini);
    }
    return g_stopPx;
}
void StopIfPending() {
    if (!g_stopPending) return;
    Log("oracle: STOP at the first significant defect: %s", g_stopWhy);
    TerminateProcess(GetCurrentProcess(), 0);
}  // the threaded frame just compared differs in > 100k pixels: its twins are always logged
// Which twins are logged: [experiments] oracle_twin_from / oracle_twin_to (frames), else the first 1500.
bool TwinWindow() {
    static int from = -1, to = 0;
    if (from < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        from =GetPrivateProfileIntA("experiments", "oracle_twin_from", 0, ini);
        to = GetPrivateProfileIntA("experiments", "oracle_twin_to", 0, ini);
    }
    if (from > 0) return g_frame >= static_cast<uint64_t>(from) && g_frame <= static_cast<uint64_t>(to);
    return g_twinLogged++ < 1500;
}
struct TwinKey {
    DrawKey k;
    const void* tex0;
    const void* vs;
    const void* ps;
    bool operator==(const TwinKey& o) const { return k == o.k && tex0 == o.tex0 && vs == o.vs && ps == o.ps; }
};
struct TwinKeyHash {
    size_t operator()(const TwinKey& t) const {
        return DrawKeyHash()(t.k) ^ (reinterpret_cast<uintptr_t>(t.tex0) * 31) ^ (reinterpret_cast<uintptr_t>(t.ps) * 17);
    }
};
void TwinDiff(const std::vector<Draw>& stock, const std::vector<Draw>& threaded, int pos) {
    std::unordered_map<TwinKey, std::pair<int, const Draw*>, TwinKeyHash> a, b;
    for (const Draw& d : stock) {
        auto& e = a[{KeyOf(d), d.state[kTex0], d.state[kVS], d.state[kPS]}];
        if (!e.first++) e.second = &d;
    }
    for (const Draw& d : threaded) {
        auto& e = b[{KeyOf(d), d.state[kTex0], d.state[kVS], d.state[kPS]}];
        if (!e.first++) e.second = &d;
    }
    int missing = 0, extra = 0;
    // Tally by type: (missing/extra, target, viewport x, technique, triangle count).
    std::map<std::tuple<int, std::string, DWORD, std::string, UINT>, int> types;
    auto add = [&](const Draw* d, int diff, int kind) {
        types[{kind, g_rtNames.count(d->rt) ? g_rtNames[d->rt] : "?", d->vpX,
               d->state[kTechnique] ? static_cast<const char*>(d->state[kTechnique]) : "-", d->count}] += diff;
    };
    for (auto& [key, e] : a) {
        auto it = b.find(key);
        int nb = it == b.end() ? 0 : it->second.first;
        if (nb < e.first) missing += e.first - nb, add(e.second, e.first - nb, 0);
    }
    for (auto& [key, e] : b) {
        auto it = a.find(key);
        int na = it == a.end() ? 0 : it->second.first;
        if (e.first > na) extra += e.first - na, add(e.second, e.first - na, 1);
    }
    // Small draws (<= 8 triangles: lamps, sprites) on the main view, matched by instance: call site,
    // size and vertex shader constants (the world transform; the state is the same in step mode).
    // Per instance: did it get another texture (same instance, other tex0), or was it drawn only in
    // one of the frames (another instance / node drawn)?
    auto mainView = [&](const Draw& d) {
        auto it = g_rtNames.find(d.rt);
        return d.count <= 8 && d.caller && it != g_rtNames.end() && it->second.rfind("6144x3456", 0) == 0;
    };
    using Inst = std::tuple<uintptr_t, UINT, const void*>;
    std::map<Inst, std::pair<std::multiset<const void*>, std::multiset<const void*>>> inst;  // tex0: stock, threaded
    std::map<Inst, std::set<DWORD>> instThreads;
    for (const Draw& d : stock)
        if (mainView(d)) inst[{d.caller, d.count, d.state[kVSConst]}].first.insert(d.state[kTex0]);
    for (const Draw& d : threaded)
        if (mainView(d)) {
            Inst k{d.caller, d.count, d.state[kVSConst]};
            inst[k].second.insert(d.state[kTex0]), instThreads[k].insert(d.tid);
        }
    int texSwapped = 0, onlyStock = 0, onlyThreaded = 0, countDiff = 0;
    char detail[1400];
    int dn = 0, shownDetail = 0;
    for (auto& [k, v] : inst) {
        if (v.first == v.second) continue;
        const char* what;
        if (v.second.empty()) ++onlyStock, what = "only stock";
        else if (v.first.empty()) ++onlyThreaded, what = "only threaded";
        else if (v.first.size() == v.second.size()) ++texSwapped, what = "other texture";
        else ++countDiff, what = "other count";
        if (shownDetail++ >= 6) continue;
        char threads[64];
        int tn = 0;
        for (DWORD t : instThreads[k]) tn = Append(threads, sizeof threads, tn, "%s%lu", tn ? "/" : "", t);
        dn = Append(detail, sizeof detail, dn, " {%s: rva %llx count %u vsc %p stock tex %p x%zu threaded tex %p x%zu threads %s}",
                    what, static_cast<unsigned long long>(std::get<0>(k)), std::get<1>(k), std::get<2>(k),
                    v.first.empty() ? nullptr : *v.first.begin(), v.first.size(),
                    v.second.empty() ? nullptr : *v.second.begin(), v.second.size(), tn ? threads : "-");
    }
    dn = Append(detail, sizeof detail, dn, " | instances %zu: other texture %d, only stock %d, only threaded %d, other count %d",
                inst.size(), texSwapped, onlyStock, onlyThreaded, countDiff);
    // Same geometry, other texture: draws matched by buffers / start / count only, whose texture
    // differs between the twins; for the threaded one, who recorded the draw, its texture binding
    // and the latest effect texture parameter.
    {
        std::map<DrawKey, std::pair<std::multiset<const void*>, std::vector<const Draw*>>, bool (*)(const DrawKey&, const DrawKey&)>
            geo([](const DrawKey& x, const DrawKey& y) { return memcmp(&x, &y, sizeof x) < 0; });
        std::map<DrawKey, std::multiset<const void*>, bool (*)(const DrawKey&, const DrawKey&)> geoStock(
            [](const DrawKey& x, const DrawKey& y) { return memcmp(&x, &y, sizeof x) < 0; });
        for (const Draw& d : stock) geoStock[KeyOf(d)].insert(d.state[kTex0]);
        for (const Draw& d : threaded) {
            auto& g = geo[KeyOf(d)];
            g.first.insert(d.state[kTex0]);
            g.second.push_back(&d);
        }
        int shownTex = 0;
        for (auto& [k, g] : geo) {
            auto it = geoStock.find(k);
            if (it == geoStock.end() || it->second == g.first || shownTex >= 4) continue;
            for (const Draw* d : g.second) {
                if (it->second.count(d->state[kTex0])) continue;  // this one's texture is in the stock twin too
                ++shownTex;
                dn = Append(detail, sizeof detail, dn,
                            " | other tex: rva %llx %s count %u tex0 %p (stock %p) draw thread %lu, tex bound by %lu "
                            "(rva %llx), fx tex by %lu (rva %llx)",
                            static_cast<unsigned long long>(d->caller),
                            d->state[kTechnique] ? static_cast<const char*>(d->state[kTechnique]) : "-", d->count,
                            d->state[kTex0], *it->second.begin(), d->tid, d->texTid,
                            static_cast<unsigned long long>(d->texCaller), d->fxTid,
                            static_cast<unsigned long long>(d->fxCaller));
                break;
            }
        }
    }
    // Large draws (>= 100 triangles) in only one of the twins: who drew them.
    int shownLarge = 0;
    for (int pass = 0; pass < 2 && shownLarge < 4; ++pass)
        for (auto& [key, e] : pass ? b : a) {
            auto& other = pass ? a : b;
            auto it = other.find(key);
            int no = it == other.end() ? 0 : it->second.first;
            const Draw* d = e.second;
            if (e.first <= no || d->count < 100 || shownLarge >= 4) continue;
            ++shownLarge;
            dn = Append(detail, sizeof detail, dn, " | %s %d: rva %llx %s count %u vb %p ib %p start %u tex0 %p thread %lu",
                        pass ? "extra" : "missing", e.first - no, static_cast<unsigned long long>(d->caller),
                        d->state[kTechnique] ? static_cast<const char*>(d->state[kTechnique]) : "-", d->count, d->state[kVB],
                        d->state[kIB], d->start, d->state[kTex0], d->tid);
        }
    std::vector<std::pair<int, std::tuple<int, std::string, DWORD, std::string, UINT>>> top;
    for (auto& [k, v] : types) top.push_back({v, k});
    std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    char line[1600];
    int n = 0;
    for (size_t t = 0; t < top.size() && t < 10; ++t)
        n = Append(line, sizeof line, n, " [%s %d: %s vp %lu %s count %u];", std::get<0>(top[t].second) ? "extra" : "missing",
                   top[t].first, std::get<1>(top[t].second).c_str(), std::get<2>(top[t].second),
                   std::get<3>(top[t].second).c_str(), std::get<4>(top[t].second));
    // Twins only once stepping runs (past start-up) and the states really match (a few defects,
    // not thousands of differences from frames of different states).
    bool big = g_bigDiff || g_stopPending;
    g_bigDiff = false;
    if (big || ((missing || extra) && g_frame > 400 && missing + extra < 1000 && TwinWindow()))
        Log("oracle: frame %llu twin draws%s (step position %d, variant %s): %d missing, %d extra of %zu (threaded %zu):%s | small main-view:%s",
            static_cast<unsigned long long>(g_frame), big ? " BIG" : "", pos,
            g_variant >= 0 && g_variant < static_cast<int>(g_variantNames.size()) ? g_variantNames[g_variant].c_str() : "-",
            missing, extra, stock.size(), threaded.size(), n ? line : "", dn ? detail : " none");
    StopIfPending();
}

}  // namespace
int DescribeUsed(const uint32_t* a, const uint32_t* b, char* out, size_t size);
namespace {
// A shadow-map draw that a threaded frame has more or less often than the serial frames around it
// (which agree). Each one is logged: a wrong shadow map for one frame darkens or lights objects.
void ShadowDefect(const DrawKey& key, int serial, int threaded, DWORD serialThread) {
    static int logged = 0;
    auto rt = g_rtNames.find(key.rt);
    if (rt == g_rtNames.end() || g_frame < 120 || logged++ >= 60000) return;
    bool shadow = strstr(rt->second.c_str(), "fmt 114") != nullptr;
    char by[200] = "";
    int n = 0;
    for (const Draw& x : g_oracleRing[2])
        if (KeyOf(x) == key && n < 160) n = Append(by, sizeof by, n, " %lu", x.tid);
    Log("oracle: %s at threaded frame %llu: target %s viewport x %lu, engine rva %llx, %s, vb %p count %u: serial frames %d (thread %lu), "
        "threaded frame %d (threads:%s)",
        shadow ? "shadow map draw" : "draw count", static_cast<unsigned long long>(g_frame - 2), rt->second.c_str(), key.vpX, static_cast<unsigned long long>(key.caller),
        key.technique ? static_cast<const char*>(key.technique) : "-", key.vb, key.count, serial, serialThread, threaded, by);
}

// A draw that a threaded frame makes with another shader, first texture or shadow-stage set than
// the serial frames around it (which agree). Logged in a compact form for a later count per draw:
// a difference in every frame is a constant of the threaded mode, a rare one is a flash.
void StateDefect(const DrawKey& key, const Draw& serial, const Draw& threaded) {
    static int logged = 0;
    if (g_frame < 120 || logged++ >= 120000) return;
    char what[200] = "";
    int n = 0;
    static const int kinds[] = {kVS, kPS, kTex0, kShadow};
    static const char* names[] = {"vs", "ps", "tex0", "shadow"};
    for (int i = 0; i < 4; ++i)
        if (serial.state[kinds[i]] != threaded.state[kinds[i]])
            n = Append(what, sizeof what, n, " %s %p->%p", names[i], serial.state[kinds[i]], threaded.state[kinds[i]]);
    auto rt = g_rtNames.find(key.rt);
    Log("oracle: state at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u:%s; threads %lu->%lu",
        static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
        static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-", key.vb, key.start,
        key.count, what, serial.tid, threaded.tid);
}

// A draw with the same shaders and textures as in the serial frames around it (which agree with
// each other, constants included), but with other shader constants in the threaded frame.
void ConstDefect(const DrawKey& key, const Draw& serial, const Draw& threaded, bool vs, bool ps) {
    static int logged = 0;
    if (g_frame < 120 || logged++ >= 120000) return;
    auto rt = g_rtNames.find(key.rt);
    Log("oracle: consts at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u:%s%s; threads %lu->%lu",
        static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
        static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-", key.vb, key.start,
        key.count, vs ? " vs" : "", ps ? " ps" : "", serial.tid, threaded.tid);
}

void OracleFrameDone() {
    // The frame just finished was drawn in g_curMode; the next one in the mode its marker set.
    uint8_t mode = g_curMode;
    int finishedPos = g_curStepPos;
    // Each presented frame must carry the next recorded frame's marker: a repeat, gap or jump means
    // the replay presented commands of another recorded frame (or none).
    uint32_t seq = g_curMarkerSeq;
    if (g_oracle && g_prevPresentedSeq && seq != g_prevPresentedSeq + 1 && ++g_seqBreaks <= 50)
        Log("oracle: presented frame %llu carried recorded frame %u after %u (mode %d, step position %d)",
            static_cast<unsigned long long>(g_frame), seq, g_prevPresentedSeq, mode, finishedPos);
    g_prevPresentedSeq = seq;
    g_curMode = g_nextMode;
    g_curStepPos = g_nextStepPos;
    g_curMarkerSeq = g_nextMarkerSeq;
    g_constsLogged = false;
    if (!g_oracle) return;
    if (g_step) {
        if (mode == 0) g_twinStock = g_draws[2], g_haveTwin = true;
        else if (g_haveTwin) TwinDiff(g_twinStock, g_draws[2], finishedPos);
        return;
    }
    for (int i = 0; i < 4; ++i)
        g_oracleRing[i].swap(g_oracleRing[i + 1]), g_oracleModes[i] = g_oracleModes[i + 1],
            g_oracleVariant[i] = g_oracleVariant[i + 1];
    g_oracleRing[4] = g_draws[2];
    g_oracleModes[4] = mode;
    g_oracleVariant[4] = g_variant;
    if (++g_oracleFilled < 5) return;
    if (!(g_oracleModes[0] == 0 && g_oracleModes[2] == 1 && g_oracleModes[4] == 0)) return;
    static FrameMap A, B, C;
    Index(g_oracleRing[0], A), Index(g_oracleRing[2], B), Index(g_oracleRing[4], C);
    ++g_triples;
    uint64_t missing = 0, extra = 0, state = 0;
    for (const auto& [key, a] : A) {
        auto c = C.find(key);
        if (c == C.end() || c->second.n != a.n) continue;  // the serial frames disagree: not a reference
        g_reference += a.n;
        auto b = B.find(key);
        int nb = b == B.end() ? 0 : b->second.n;
        if (nb != a.n)  // who drew it in the threaded frame
            for (const Draw& x : g_oracleRing[2])
                if (KeyOf(x) == key) ++g_defects[key].threadedBy[x.tid];
        bool stateDiffers = a.n == 1 && nb == 1 && SameDrawState(*a.first, *c->second.first) &&
                            !SameDrawState(*a.first, *b->second.first);
        if (a.n == 1 && nb == 1 && !stateDiffers) {
            bool vs = ConstOnlyHere(*a.first, *b->second.first, *c->second.first, kVSConst);
            bool ps = ConstOnlyHere(*a.first, *b->second.first, *c->second.first, kPSConst);
            if (vs || ps) ConstDefect(key, *a.first, *b->second.first, vs, ps);
            if (a.first->texN == c->second.first->texN && b->second.first->texN != a.first->texN) {
                static int logged = 0;
                auto rt = g_rtNames.find(key.rt);
                if (g_frame >= 120 && logged++ < 120000)
                    Log("oracle: textures 1-7 at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u; threads %lu->%lu",
                        static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
                        static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-",
                        key.vb, key.start, key.count, a.first->tid, b->second.first->tid);
            }
            {
                const Draw &da = *a.first, &db = *b->second.first, &dc = *c->second.first;
                bool rs = da.rs == dc.rs && db.rs != da.rs, ss = da.ss == dc.ss && db.ss != da.ss,
                     misc = da.misc == dc.misc && db.misc != da.misc;
                static int logged = 0;
                auto rt = g_rtNames.find(key.rt);
                if (!memcmp(da.used, dc.used, sizeof da.used) && memcmp(da.used, db.used, sizeof da.used)) {
                    static int usedLogged = 0;
                    char what[400] = "";
                    DescribeUsed(da.used, db.used, what, sizeof what);
                    if (g_frame >= 120 && usedLogged++ < 300000)
                        Log("oracle: used state at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u:%s; threads %lu->%lu",
                            static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
                            static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-",
                            key.vb, key.start, key.count, what, da.tid, db.tid);
                }
                {  // constant groups that the serial frames agree on and the threaded frame has otherwise
                    char groups[200] = "";
                    int gn = 0;
                    for (int k = 0; k < 2; ++k)
                        for (int g = 0; g < 16; ++g)
                            if (da.cg[k][g] == dc.cg[k][g] && db.cg[k][g] != da.cg[k][g] && gn < 180)
                                gn = Append(groups, sizeof groups, gn, " %s%d-%d", k ? "ps" : "vs", g * 4, g * 4 + 3);
                    static int constLogged = 0;
                    if (gn && g_frame >= 120 && constLogged++ < 400000)
                        Log("oracle: constants at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u:%s; threads %lu->%lu",
                            static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
                            static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-",
                            key.vb, key.start, key.count, groups, da.tid, db.tid);
                }
                if (false && (rs || ss || misc) && g_frame >= 120 && logged++ < 200000)
                    Log("oracle: device state at threaded frame %llu: target %s vp %lu rva %llx %s vb %p start %u count %u:%s%s%s; threads %lu->%lu",
                        static_cast<unsigned long long>(g_frame - 2), rt != g_rtNames.end() ? rt->second.c_str() : "?", key.vpX,
                        static_cast<unsigned long long>(key.caller), key.technique ? static_cast<const char*>(key.technique) : "-",
                        key.vb, key.start, key.count, rs ? " render-states" : "", ss ? " sampler-states" : "", misc ? " bindings" : "",
                        da.tid, db.tid);
            }
        }
        if (nb == a.n && !stateDiffers) continue;
        Defect& d = g_defects[key];
        ++d.frames;
        d.tid = a.first->tid;
        if (nb < a.n) d.missing += a.n - nb, missing += a.n - nb;
        if (nb > a.n) d.extra += nb - a.n, extra += nb - a.n;
        if (stateDiffers) ++d.state, ++state;
        if (nb != a.n) ShadowDefect(key, a.n, nb, a.first->tid);
        if (stateDiffers) StateDefect(key, *a.first, *b->second.first);
    }
    for (const auto& [key, b] : B)  // drawn only in the threaded frame
        if (!A.count(key) && !C.count(key)) {
            Defect& d = g_defects[key];
            ++d.frames, d.extra += b.n, extra += b.n;
            d.tid = b.first->tid;
            ++d.threadedBy[b.first->tid];
            ShadowDefect(key, 0, b.n, 0);
        }
    g_missingTotal += missing, g_extraTotal += extra, g_stateTotal += state;
    // Per variant, when all five frames ran under the same one.
    int v = g_oracleVariant[0];
    bool one = v >= 0;
    for (int i = 1; i < 5; ++i) one = one && g_oracleVariant[i] == v;
    if (one) {
        if (g_variantOracle.size() < g_variantNames.size()) g_variantOracle.resize(g_variantNames.size());
        VariantOracle& vo = g_variantOracle[v];
        ++vo.triples, vo.missing += missing, vo.extra += extra, vo.state += state;
        if (g_triples % 100 == 0) {
            char line[900];
            int n = 0;
            for (size_t i = 0; i < g_variantOracle.size() && n < 840; ++i) {
                const VariantOracle& t = g_variantOracle[i];
                if (!t.triples) continue;
                n = Append(line, sizeof line, n, " %s: %.1f missing, %.1f extra, %.1f state per frame (%llu);",
                               g_variantNames[i].c_str(), double(t.missing) / t.triples, double(t.extra) / t.triples,
                               double(t.state) / t.triples, static_cast<unsigned long long>(t.triples));
            }
            Log("oracle: by variant:%s", line);
        }
    }
    if (missing || extra || state) ++g_triplesWithDefect;
    if (g_triples % 100 == 0) OracleReport();
}

// --- oracle: full-resolution threaded vs stock frames -----------------------------------------
// The presented image of every frame is read back. A threaded frame is compared with the stock
// frame two before it (same parity: some work alternates between frames), a stock frame with the
// stock frame two before (the noise floor; 0 when the scene is frozen). Differing pixels (any
// channel off by more than kPixelTolerance) are grouped into 16x16 tiles and those into regions;
// the first saved defects are written as stock / threaded / marked BMPs next to the game.
constexpr int kPixelTolerance = 24, kTile = 16, kMaxDefectsSaved = 8;
// Smallest region saved as images: [experiments] oracle_save_min_px (default 500).
int SavedRegionPixels() {
    static int v = -1;
    if (v < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        v = GetPrivateProfileIntA("experiments", "oracle_save_min_px", 500, ini);
    }
    return v;
}
IDirect3DSurface9* g_fullSys = nullptr;
UINT g_fullW = 0, g_fullH = 0;
std::vector<uint32_t> g_fullFrames[2];  // last stock frame per parity
bool g_fullHave[2] = {};
int g_defectsSaved = 0;
uint64_t g_fullCompared[2] = {}, g_fullDiffering[2] = {}, g_fullPixels[2] = {};  // [0] stock-stock, [1] threaded-stock

void WriteBmp(const char* path, const uint32_t* px, UINT w, UINT h, const uint32_t* mark = nullptr) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return;
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16), imgSize = row * h;
    int32_t iw = static_cast<int32_t>(w), ih = -static_cast<int32_t>(h);  // top-down
    memcpy(hdr + 10, &off, 4), memcpy(hdr + 14, &dib, 4), memcpy(hdr + 18, &iw, 4), memcpy(hdr + 22, &ih, 4);
    memcpy(hdr + 26, &planesBpp, 4), memcpy(hdr + 34, &imgSize, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            uint32_t p = px[y * w + x];
            if (mark && mark[y * w + x]) p = 0x00ff00ffu;  // magenta where the frames differ
            line[x * 3] = p & 0xff, line[x * 3 + 1] = (p >> 8) & 0xff, line[x * 3 + 2] = (p >> 16) & 0xff;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

void CompareFull(const std::vector<uint32_t>& ref, const std::vector<uint32_t>& cur, bool threaded) {
    const UINT w = g_fullW, h = g_fullH, tw = (w + kTile - 1) / kTile, th = (h + kTile - 1) / kTile;
    std::vector<uint16_t> tiles(tw * th, 0);
    uint64_t differing = 0;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            uint32_t a = ref[y * w + x], b = cur[y * w + x];
            if (a == b) continue;
            int d = 0;
            for (int c = 0; c < 24; c += 8) d = (std::max)(d, abs(static_cast<int>((a >> c) & 0xff) - static_cast<int>((b >> c) & 0xff)));
            if (d > kPixelTolerance) ++differing, ++tiles[(y / kTile) * tw + x / kTile];
        }
    int k = threaded ? 1 : 0;
    if (threaded && differing > 100000) g_bigDiff = true;

    ++g_fullCompared[k];
    g_fullPixels[k] += differing;
    if (!differing) return;
    ++g_fullDiffering[k];
    // Regions: 4-connected tiles with any differing pixel.
    std::vector<int> label(tw * th, -1);
    struct Box { UINT x0, y0, x1, y1; uint64_t n; };
    std::vector<Box> boxes;
    std::vector<int> stack;
    for (UINT i = 0; i < tw * th; ++i) {
        if (!tiles[i] || label[i] >= 0) continue;
        Box b{i % tw, i / tw, i % tw, i / tw, 0};
        label[i] = static_cast<int>(boxes.size());
        stack.assign(1, static_cast<int>(i));
        while (!stack.empty()) {
            int j = stack.back();
            stack.pop_back();
            UINT x = j % tw, y = j / tw;
            b.n += tiles[j];
            b.x0 = (std::min)(b.x0, x), b.x1 = (std::max)(b.x1, x), b.y0 = (std::min)(b.y0, y), b.y1 = (std::max)(b.y1, y);
            const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (auto& o : nb) {
                int nx = static_cast<int>(x) + o[0], ny = static_cast<int>(y) + o[1];
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(tw) || ny >= static_cast<int>(th)) continue;
                int q = ny * static_cast<int>(tw) + nx;
                if (tiles[q] && label[q] < 0) label[q] = label[i], stack.push_back(q);
            }
        }
        boxes.push_back(b);
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.n > b.n; });
    if (threaded && StopPx() > 0 && g_frame > 450 && !g_stopPending && !boxes.empty() &&
        boxes[0].n >= static_cast<uint64_t>(StopPx())) {
        g_stopPending = true;  // ends the game once this frame's twins are logged
        sprintf_s(g_stopWhy, "frame %llu, step position %d, region %u,%u-%u,%u (%llu px)", static_cast<unsigned long long>(g_frame),
                  g_curStepPos, boxes[0].x0 * kTile, boxes[0].y0 * kTile, (boxes[0].x1 + 1) * kTile - 1,
                  (boxes[0].y1 + 1) * kTile - 1, static_cast<unsigned long long>(boxes[0].n));
    }
    char line[700];
    int n = 0;
    for (size_t i = 0; i < boxes.size() && i < 8; ++i)
        n = Append(line, sizeof line, n, " %u,%u-%u,%u (%llu px);", boxes[i].x0 * kTile, boxes[i].y0 * kTile,
                   (boxes[i].x1 + 1) * kTile - 1, (boxes[i].y1 + 1) * kTile - 1, static_cast<unsigned long long>(boxes[i].n));
    Log("oracle: frame %llu %s differs from stock (step position %d, variant %s): %llu pixels in %zu region(s):%s",
        static_cast<unsigned long long>(g_frame), threaded ? "THREADED" : "stock", g_curStepPos,
        g_variant >= 0 && g_variant < static_cast<int>(g_variantNames.size()) ? g_variantNames[g_variant].c_str() : "-",
        static_cast<unsigned long long>(differing), boxes.size(), line);
    // Save only significant defects: a region of at least 50 px above the HUD band (the bottom
    // ~10% of the image), once past the first 300 frames (start-up).
    bool significant = false;
    for (const Box& bx : boxes) significant |= bx.n >= static_cast<uint64_t>(SavedRegionPixels());  // (the whole picture: the cab dashboard sits in the bottom tenth)
    if (threaded && significant && g_frame > 300 && g_defectsSaved < kMaxDefectsSaved) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* s = strrchr(path, '\\')) *s = 0;
        std::vector<uint32_t> mark(w * h, 0);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                uint32_t a = ref[y * w + x], b = cur[y * w + x];
                int d = 0;
                for (int c = 0; c < 24; c += 8) d = (std::max)(d, abs(static_cast<int>((a >> c) & 0xff) - static_cast<int>((b >> c) & 0xff)));
                mark[y * w + x] = d > kPixelTolerance;
            }
        char name[MAX_PATH + 64];
        sprintf_s(name, "%s\\tscfix-oracle-%02d-stock.bmp", path, g_defectsSaved);
        WriteBmp(name, ref.data(), w, h);
        sprintf_s(name, "%s\\tscfix-oracle-%02d-threaded.bmp", path, g_defectsSaved);
        WriteBmp(name, cur.data(), w, h);
        sprintf_s(name, "%s\\tscfix-oracle-%02d-marked.bmp", path, g_defectsSaved);
        WriteBmp(name, cur.data(), w, h, mark.data());
        Log("oracle: saved defect %02d: frame %llu, step position %d, variant %s, largest region %u,%u-%u,%u (%llu px)",
            g_defectsSaved, static_cast<unsigned long long>(g_frame), g_curStepPos,
            g_variant >= 0 && g_variant < static_cast<int>(g_variantNames.size()) ? g_variantNames[g_variant].c_str() : "-",
            boxes[0].x0 * kTile, boxes[0].y0 * kTile,
            (boxes[0].x1 + 1) * kTile - 1, (boxes[0].y1 + 1) * kTile - 1, static_cast<unsigned long long>(boxes[0].n));
        ++g_defectsSaved;
    }
}

// Present (render thread), mode = the mode the presented frame was drawn in.
void OracleFullFrame(IDirect3DDevice9* d, IDirect3DSurface9* back, uint8_t mode) {
    D3DSURFACE_DESC desc;
    if (FAILED(back->GetDesc(&desc))) return;
    if (!g_fullSys || desc.Width != g_fullW || desc.Height != g_fullH) {
        if (g_fullSys) g_fullSys->Release(), g_fullSys = nullptr;
        if (FAILED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_fullSys, nullptr)))
            return;
        g_fullW = desc.Width, g_fullH = desc.Height;
        g_fullHave[0] = g_fullHave[1] = false;
    }
    if (FAILED(d->GetRenderTargetData(back, g_fullSys))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_fullSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    std::vector<uint32_t> cur(static_cast<size_t>(g_fullW) * g_fullH);
    for (UINT y = 0; y < g_fullH; ++y) memcpy(&cur[y * g_fullW], static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, g_fullW * 4);
    g_fullSys->UnlockRect();
    int parity = g_step ? 0 : static_cast<int>(g_frame & 1);  // step: the stock twin is the previous frame
    if (g_fullHave[parity] && (!g_step || mode == 1)) CompareFull(g_fullFrames[parity], cur, mode == 1);
    if (mode == 0) g_fullFrames[parity].swap(cur), g_fullHave[parity] = true;
    else if (g_step && g_curStepPos >= g_stepThreaded) g_fullHave[parity] = false;  // the cycle's last twin
    if (g_frame % 300 == 0)
        Log("oracle: full frames: stock vs stock %llu of %llu differ (%.0f px avg), threaded vs stock %llu of %llu differ "
            "(%.0f px avg)", static_cast<unsigned long long>(g_fullDiffering[0]), static_cast<unsigned long long>(g_fullCompared[0]),
            g_fullCompared[0] ? double(g_fullPixels[0]) / g_fullCompared[0] : 0.0,
            static_cast<unsigned long long>(g_fullDiffering[1]), static_cast<unsigned long long>(g_fullCompared[1]),
            g_fullCompared[1] ? double(g_fullPixels[1]) / g_fullCompared[1] : 0.0);
}
}  // namespace
void TallyEvent(uint64_t frame, int pixels);
void StepVariants(uint64_t frame);
namespace {

void Check() {
    const uint8_t *a = g_frames[0].data(), *m = g_frames[1].data(), *c = g_frames[2].data();
    static std::vector<uint8_t> mask(kW * kH);
    static std::vector<int> stack;
    int flaggedTotal = 0;
    for (int i = 0; i < kW * kH; ++i) {
        int da = m[i] - a[i], dc = m[i] - c[i], ac = a[i] - c[i];
        if (ac < 0) ac = -ac;
        bool up = da > kThreshold && dc > kThreshold, down = da < -kThreshold && dc < -kThreshold;
        mask[i] = (up || down) && ac < kThreshold / 2;
        flaggedTotal += mask[i];
    }
    g_flagged += flaggedTotal;
    struct Patch { int n, x0, y0, x1, y1; };
    std::vector<Patch> patches;
    for (int start = 0; start < kW * kH; ++start) {
        if (!mask[start]) continue;
        int n = 0, x0 = kW, y0 = kH, x1 = -1, y1 = -1;
        stack.clear();
        stack.push_back(start);
        mask[start] = 0;
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            int x = i % kW, y = i / kW;
            ++n;
            x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0, x1 = x > x1 ? x : x1, y1 = y > y1 ? y : y1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= kW || yy >= kH) continue;
                    int j = yy * kW + xx;
                    if (mask[j]) mask[j] = 0, stack.push_back(j);
                }
        }
        int area = (x1 - x0 + 1) * (y1 - y0 + 1);
        // Any size or shape (a whole building can flicker). Run with the network renderer skipped
        // (glitch_skip_modules): its overhead wires were the only thing flagging without a glitch.
        (void)area;
        if (n >= kMinPixels) patches.push_back({n, x0, y0, x1, y1});
    }
    if (patches.empty()) return;
    std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.n > b.n; });
    int total = 0;
    for (const Patch& q : patches) {
        total += q.n;
        ++g_cells[(q.y0 + q.y1) / 2 * 12 / kH][(q.x0 + q.x1) / 2 * 24 / kW];
    }
    ++g_events, ++g_windowEvents;
    {
        int sum = 0;
        for (const Patch& q : patches) sum += q.n;
        TallyEvent(g_frame - 1, sum);
    }
    char boxes[160] = "";
    for (size_t k = 0; k < patches.size() && k < 3; ++k) {
        size_t len = strlen(boxes);
        Append(boxes, sizeof boxes, static_cast<int>(len), " %d,%d-%d,%d(%d)", patches[k].x0, patches[k].y0, patches[k].x1,
                  patches[k].y1, patches[k].n);
    }
    Log("glitch: frame %llu: %zu patch(es), %d pixels changed for one frame (of %dx%d); largest:%s",
        static_cast<unsigned long long>(g_frame - 1), patches.size(), total, kW, kH, boxes);
    if (total >= 100) DiffDraws(g_frame - 1);
    if (g_saved < kMaxSaved) {
        ++g_saved;
        Save(g_frames[0], g_frame - 1, 'a');
        Save(g_frames[1], g_frame - 1, 'b');
        Save(g_frames[2], g_frame - 1, 'c');
    }
}

// --- shadow maps ------------------------------------------------------------------------------
constexpr int kCap = 128;  // cells per side of a square region (an atlas keeps its aspect)
struct Capture {
    UINT size, cellsX;
    D3DFORMAT format;
    std::vector<float> cells;
};
std::vector<Capture> g_capCurrent, g_capRing[3];  // per frame, in the order the targets were left
IDirect3DSurface9* g_rt0 = nullptr;  // bound now (no reference: compared and used while bound)
struct CapSurfaces {
    IDirect3DSurface9* reduced = nullptr;
    IDirect3DSurface9* sys = nullptr;
    bool failed = false;
};
std::map<uint64_t, CapSurfaces> g_capSurfaces;  // by format and width in cells
int g_descsLogged = 0;
std::vector<IDirect3DSurface9*> g_descSeen;
void LogDesc(const char* what, IDirect3DSurface9* s) {
    if (!s || g_descsLogged >= 40) return;
    for (auto* x : g_descSeen)
        if (x == s) return;
    g_descSeen.push_back(s);
    D3DSURFACE_DESC d;
    if (FAILED(s->GetDesc(&d))) return;
    ++g_descsLogged;
    Log("glitch: %s %p: %ux%u format %d usage 0x%lx", what, static_cast<void*>(s), d.Width, d.Height,
        static_cast<int>(d.Format), d.Usage);
}
float Half(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v = exp == 0 ? man / 1024.0f / 16384.0f : (1.0f + man / 1024.0f) * float(1u << exp) / 32768.0f;
    return sign ? -v : v;
}
void CaptureTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    D3DSURFACE_DESC d;
    // Shadow maps: float targets at least 512 high whose width is a whole number of squares
    // (a cascade atlas), or square ones.
    if (FAILED(s->GetDesc(&d)) || d.Height < 512 || d.Width % d.Height || d.Width / d.Height > 8 ||
        !(d.Usage & D3DUSAGE_RENDERTARGET))
        return;
    const int cx = kCap * static_cast<int>(d.Width / d.Height);
    CapSurfaces& c = g_capSurfaces[static_cast<uint64_t>(d.Format) << 32 | cx];
    if (c.failed) return;
    if (!c.reduced) {
        if (FAILED(dev->CreateRenderTarget(cx, kCap, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &c.reduced, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(cx, kCap, d.Format, D3DPOOL_SYSTEMMEM, &c.sys, nullptr))) {
            c.failed = true;
            Log("glitch: cannot capture format %d", static_cast<int>(d.Format));
            return;
        }
    }
    if (FAILED(dev->StretchRect(s, nullptr, c.reduced, nullptr, D3DTEXF_POINT)) ||
        FAILED(dev->GetRenderTargetData(c.reduced, c.sys))) {
        c.failed = true;
        Log("glitch: copying format %d failed", static_cast<int>(d.Format));
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(c.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    Capture cap{d.Width, static_cast<UINT>(cx), d.Format, std::vector<float>(cx * kCap)};
    for (int y = 0; y < kCap; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
        for (int x = 0; x < cx; ++x) {
            float v = 0;
            uint16_t h = 0;
            switch (d.Format) {
                case D3DFMT_R32F: memcpy(&v, row + 4 * x, 4); break;
                case D3DFMT_G32R32F: memcpy(&v, row + 8 * x, 4); break;
                case D3DFMT_R16F: memcpy(&h, row + 2 * x, 2), v = Half(h); break;
                case D3DFMT_G16R16F: memcpy(&h, row + 4 * x, 2), v = Half(h); break;
                case D3DFMT_A16B16G16R16F: memcpy(&h, row + 8 * x, 2), v = Half(h); break;
                default: v = (row[4 * x] + row[4 * x + 1] + row[4 * x + 2]) / 765.0f;
            }
            cap.cells[y * cx + x] = v;
        }
    }
    c.sys->UnlockRect();
    g_capCurrent.push_back(std::move(cap));
}
// The frame before the newest against its neighbours, per target position in the frame.
void CheckCaptures(uint64_t frame) {
    const auto &A = g_capRing[0], &B = g_capRing[1], &C = g_capRing[2];
    size_t n = (std::min)({A.size(), B.size(), C.size()});
    for (size_t k = 0; k < n; ++k) {
        if (A[k].size != B[k].size || B[k].size != C[k].size) continue;
        const int cx = static_cast<int>(B[k].cellsX), cells = cx * kCap;
        int one = 0, minX = cx, minY = kCap, maxX = -1, maxY = -1;
        for (int i = 0; i < cells; ++i) {
            float a = A[k].cells[i], b = B[k].cells[i], c = C[k].cells[i];
            float eps = 1e-3f * (std::max)({fabsf(a), fabsf(c), 1e-3f});
            if (fabsf(a - c) <= eps && fabsf(b - a) > eps && fabsf(b - c) > eps) {
                ++one;
                int x = i % cx, y = i / cx;
                minX = (std::min)(minX, x), maxX = (std::max)(maxX, x);
                minY = (std::min)(minY, y), maxY = (std::max)(maxY, y);
            }
        }
        if (one)
            Log("glitch: frame %llu shadow target %zu (%u, format %d): %d of %d cells differ for one frame "
                "(cells %d,%d-%d,%d)",
                static_cast<unsigned long long>(frame), k, B[k].size, static_cast<int>(B[k].format), one, cells,
                minX, minY, maxX, maxY);
    }
    if (A.size() != B.size() || B.size() != C.size())
        Log("glitch: frame %llu shadow targets: %zu / %zu / %zu (before / flagged / after)",
            static_cast<unsigned long long>(frame), A.size(), B.size(), C.size());
}
}  // namespace

void NoteRenderTarget(IDirect3DDevice9* device, IDirect3DSurface9* next) {
    LogDesc("render target", next);
    if (next && !g_rtNames.count(next)) {
        D3DSURFACE_DESC d;
        char name[48] = "?";
        if (SUCCEEDED(next->GetDesc(&d))) sprintf_s(name, "%ux%u fmt %d", d.Width, d.Height, static_cast<int>(d.Format));
        g_rtNames[next] = name;
    }
    if (g_rt0 && g_rt0 != next) CaptureTarget(device, g_rt0);
    g_rt0 = next;
    g_vpX = 0;  // SetRenderTarget resets the viewport to the whole target
}
void NoteBinding(int slot, uint64_t v);
void NoteDepthStencil(IDirect3DSurface9* next) { LogDesc("depth surface", next), NoteBinding(0, reinterpret_cast<uintptr_t>(next)); }
void NoteViewport(const D3DVIEWPORT9* vp) { g_vpX = vp ? vp->X : 0; }

namespace {
}  // namespace

struct SwitchEntry {
    std::string name;
    Switch fn;
};
std::vector<SwitchEntry> g_switches;
std::vector<int> g_variantSwitch;  // per variant: index into g_switches, -1 for the baseline
std::vector<std::string> g_variantNames;
int g_slice = 0, g_variant = -1;
uint8_t g_variantOfFrame[256];  // variant that drew frame n (n % 256); 255 = not counted
struct VariantTally {
    uint64_t frames = 0, events = 0, pixels = 0, large = 0;
};
std::vector<VariantTally> g_tally;
constexpr int kSettleFrames = 10;  // frames after a switch that may still be the old variant's

void RegisterSwitch(const char* name, Switch fn) { g_switches.push_back({name, fn}); }
Switch FindSwitch(const char* name) {
    for (const SwitchEntry& e : g_switches)
        if (e.name == name) return e.fn;
    return nullptr;
}
void ConfigureVariants(const char* names, int sliceFrames) {
    g_slice = sliceFrames > 0 ? sliceFrames : 150;
    g_variantNames = {"baseline"};
    g_variantSwitch = {-1};
    std::string list = names ? names : "";
    for (size_t pos = 0; pos < list.size();) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string item = list.substr(pos, comma - pos);
        pos = comma + 1;
        if (item.empty()) continue;
        int found = -1;
        for (size_t i = 0; i < g_switches.size(); ++i)
            if (g_switches[i].name == item) found = static_cast<int>(i);
        if (found < 0) {
            Log("glitch: no switch named '%s'", item.c_str());
            continue;
        }
        g_variantNames.push_back(item);
        g_variantSwitch.push_back(found);
    }
    g_tally.assign(g_variantNames.size(), VariantTally{});
    memset(g_variantOfFrame, 255, sizeof g_variantOfFrame);
    if (g_variantNames.size() > 1) Log("glitch: %zu variants, %d frames each in turn", g_variantNames.size(), g_slice);
}
// Render thread, per present: switch variants at slice boundaries.
void StepVariants(uint64_t frame) {
    if (g_variantNames.size() < 2) return;
    int v = static_cast<int>((frame / g_slice) % g_variantNames.size());
    if (v != g_variant) {
        // With the oracle the switch is applied per frame on the recording thread (threaded frames only).
        if (!g_oracle) {
            if (g_variant >= 0 && g_variantSwitch[g_variant] >= 0) g_switches[g_variantSwitch[g_variant]].fn(false);
            if (g_variantSwitch[v] >= 0) g_switches[g_variantSwitch[v]].fn(true);
        }
        g_variant = v;
    }
    bool counted = frame % g_slice >= kSettleFrames;
    g_variantOfFrame[frame % 256] = counted ? static_cast<uint8_t>(v) : 255;
    if (counted) ++g_tally[v].frames;
    if (frame % 600 == 599) {
        char line[900];
        int n = 0;
        for (size_t i = 0; i < g_tally.size() && n < 840; ++i) {
            const VariantTally& t = g_tally[i];
            n = Append(line, sizeof line, n, " %s: %llu events (%llu >= 100 px) in %llu frames, %.1f per 1000;",
                           g_variantNames[i].c_str(), static_cast<unsigned long long>(t.events),
                           static_cast<unsigned long long>(t.large), static_cast<unsigned long long>(t.frames),
                           t.frames ? 1000.0 * t.events / t.frames : 0.0);
        }
        Log("glitch: variants:%s", line);
    }
}
void TallyEvent(uint64_t frame, int pixels) {
    if (g_tally.empty()) return;
    uint8_t v = g_variantOfFrame[frame % 256];
    if (v == 255 || v >= g_tally.size()) return;
    ++g_tally[v].events;
    g_tally[v].pixels += pixels;
    if (pixels >= 100) ++g_tally[v].large;
}

bool OracleSkipUpdate() {
    if (!g_skipNextUpdate) return false;
    g_skipNextUpdate = false;
    return true;
}
void ConfigureOracleStep(bool on, bool calibrate, int threaded) {
    g_step = on, g_stepCalibrate = calibrate, g_stepThreaded = threaded > 0 ? threaded : 1;
}
void ConfigureOracle(bool on) {
    g_oracle = on;
    if (on) Log("oracle: alternating serial and threaded frames");
}
volatile int g_recordMode = -1;
int OracleRecordMode() { return g_recordMode; }
// Step mode ([experiments] oracle_step=1): renders come in twins of one world state, stock then
// threaded. At the boundary that launches the stock render the update that follows is skipped,
// so the next render shows the same state; the threaded render then runs beside the next update,
// as in normal play. Each threaded frame is compared, pixel for pixel, with its stock twin.
void OracleOnFrame() {
    if (!g_oracle || !g_enabled) return;
    static uint64_t n = 0;
    ++n;
    // the next frame: 1 threaded, 0 serial (S S T T; step mode S T)
    // step: cycle of one stock and g_stepThreaded threaded renders of one state (updates skipped
    // within the cycle; the last threaded render runs beside the next update)
    int pos = g_step ? static_cast<int>(n % (1 + g_stepThreaded)) : 0;
    uint8_t mode = static_cast<uint8_t>(g_step ? (pos != 0) : ((n >> 1) & 1));
    if (g_step) g_skipNextUpdate = pos < g_stepThreaded, g_stepPos = pos;
    g_recordMode = mode;
    overlap::SetOverlapOnly(mode != 0 && !g_stepCalibrate);
    // The serial frames are the stock reference: a variant's switch is on in threaded frames only.
    static int applied = -1;
    // ("upd:" switches let an engine update run: they hold for every frame of their slice.)
    int sw = g_variant >= 0 && g_variant < static_cast<int>(g_variantSwitch.size()) ? g_variantSwitch[g_variant] : -1;
    bool everyFrame = sw >= 0 && g_switches[sw].name.rfind("upd:", 0) == 0;
    int want = mode || everyFrame ? sw : -1;
    if (want != applied) {
        if (applied >= 0) g_switches[applied].fn(false);
        if (want >= 0) g_switches[want].fn(true);
        applied = want;
    }
    if (rt::Recording()) {
        uint8_t blob[6] = {mode, static_cast<uint8_t>(g_stepPos)};
        uint32_t seq = static_cast<uint32_t>(n);
        memcpy(blob + 2, &seq, 4);
        rt::Enqueue([](const uint8_t* b) {
            g_nextMode = b[0], g_nextStepPos = b[1];
            uint32_t q;
            memcpy(&q, b + 2, 4);
            g_nextMarkerSeq = q;
        }, blob, 6);
    }
}

void Configure(bool enabled, uintptr_t watchRva) {
    g_enabled = enabled;
    g_watchRva = watchRva;
}

void NoteState(StateKind kind, const void* object) { g_state[kind] = object; }
std::map<DWORD, uint64_t> g_lastSeq;
uint64_t g_lostDraws = 0, g_lostEvents = 0;
void NoteRecorder(uintptr_t caller, DWORD tid, uint64_t seq) {
    if (!g_engineBase) g_engineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("GameManagerVC64.dll"));
    uint64_t& last = g_lastSeq[tid];
    if (last && seq != last + 1) {
        ++g_lostEvents;
        if (seq > last) g_lostDraws += seq - last - 1;
        if (g_lostEvents <= 60)
            Log("glitch: frame %llu: draws recorded by thread %lu replay out of order: #%llu after #%llu (%s)",
                static_cast<unsigned long long>(g_frame), tid, static_cast<unsigned long long>(seq),
                static_cast<unsigned long long>(last), seq > last ? "lost in between" : "earlier draw after a later one");
    }
    last = seq;
    g_caller = caller >= g_engineBase && caller < g_engineBase + 0x2000000 ? caller - g_engineBase : 0;
    g_callerTid = tid;
}
void NoteTexSetter(uintptr_t caller, DWORD tid) {
    if (!g_engineBase) g_engineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("GameManagerVC64.dll"));
    // Engine rva, or the raw address (high) when the binding came from elsewhere (an effect).
    g_texCaller = caller >= g_engineBase && caller < g_engineBase + 0x2000000 ? caller - g_engineBase : caller;
    g_texTid = tid;
}
void NoteFxTexture(uintptr_t caller, DWORD tid, uint64_t r14, uint64_t rdi) {
    if (!g_engineBase) g_engineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("GameManagerVC64.dll"));
    g_fxCaller = caller >= g_engineBase && caller < g_engineBase + 0x2000000 ? caller - g_engineBase : caller;
    g_fxTid = tid;
    g_foreign = 0;
    bool watched = g_watchRva && g_fxCaller == g_watchRva;
    g_fxMaterial = watched ? r14 : 0;
    g_fxSlot = watched ? rdi & 0xffffffff : 0;
    g_fxSlotObject = watched ? ReadSlot(r14, rdi) : 0;
    // Once in the simulation, watch the first entry seen.
    if (watched && g_fxSlotObject && g_frame > 300 && InterlockedCompareExchange(&g_watchArmed, 1, 0) == 0) {
        g_watchAddr = r14 + 0x40 + (rdi & 0xffffffff) * 8;  // the material's slot (entry pointer)
        if (HANDLE t = CreateThread(nullptr, 0, &ArmWatch, nullptr, 0, nullptr)) CloseHandle(t);
    }
}
// Shadow maps as textures: float render-target textures, at least 512 high, whose width is a
// whole number of squares (the cascade atlas). Answers are kept per texture.
std::map<const void*, bool> g_isShadowMap;
uintptr_t g_shadowStages = 0;
bool IsShadowMap(IDirect3DBaseTexture9* t) {
    auto it = g_isShadowMap.find(t);
    if (it != g_isShadowMap.end()) return it->second;
    bool yes = false;
    D3DSURFACE_DESC d;
    if (t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)))
        yes = (d.Usage & D3DUSAGE_RENDERTARGET) && d.Format == D3DFMT_R32F && d.Height >= 512 && d.Width % d.Height == 0;
    g_isShadowMap[t] = yes;
    return yes;
}
// Constant registers: their values, and a hash of all of them (XOR of per-register hashes, so a
// write updates it in O(registers written)).
constexpr UINT kRegisters = 256;
float g_regs[2][kRegisters][4] = {};
uint64_t g_regHash[2] = {};
uint32_t g_groupHash[2][16] = {};  // registers 0-63 in groups of four
uint64_t RegHash(UINT r, const float* v) {
    uint64_t h = 0x9e3779b97f4a7c15ull * (r + 1);
    uint32_t bits[4];
    memcpy(bits, v, 16);
    for (uint32_t b : bits) {
        h ^= b;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 31;
    }
    return h;
}
void NoteConstants(bool pixel, UINT start, const float* data, UINT count) {
    if (!data) return;
    int k = pixel ? 1 : 0;
    for (UINT i = 0; i < count && start + i < kRegisters; ++i) {
        UINT r = start + i;
        uint64_t before = RegHash(r, g_regs[k][r]);
        memcpy(g_regs[k][r], data + 4 * i, 16);
        uint64_t after = RegHash(r, g_regs[k][r]);
        g_regHash[k] ^= before ^ after;
        if (r < 64) g_groupHash[k][r / 4] ^= static_cast<uint32_t>(before ^ (before >> 32)) ^ static_cast<uint32_t>(after ^ (after >> 32));
    }
    NoteState(pixel ? kPSConst : kVSConst, reinterpret_cast<const void*>(g_regHash[k]));
}

// Rolling values over device state the draw log did not cover: each slot's old value is taken out
// of the mix and its new value put in.
uint64_t g_rsMix = 0, g_ssMix = 0, g_miscMix = 0;
DWORD g_rsVal[256] = {};
DWORD g_ssVal[16][16] = {};
uint64_t g_miscVal[16] = {};
uint64_t SlotMix(uint64_t slot, uint64_t v) {
    uint64_t h = (v + 0x9e3779b97f4a7c15ull * (slot + 1)) * 0xbf58476d1ce4e5b9ull;
    return h ^ (h >> 29);
}
// What a draw depends on, value by value (the mixes above cover every slot, also unused ones).
const DWORD kUsedSampler[8] = {1, 2, 5, 6, 7, 8, 9, 10};  // address u, v; mag, min, mip filter; mip bias; max mip level; max anisotropy
const char* const kUsedSamplerName[8] = {"addressU", "addressV", "mag", "min", "mip", "mipBias", "maxMip", "aniso"};
const DWORD kUsedRs[14] = {7, 14, 15, 19, 20, 22, 23, 24, 25, 27, 52, 168, 175, 195};
const char* const kUsedRsName[14] = {"zEnable", "zWrite", "alphaTest", "srcBlend", "destBlend", "cull", "zFunc", "alphaRef", "alphaFunc",
                                     "alphaBlend", "stencil", "colorWrite", "slopeBias", "depthBias"};
void FillUsed(uint32_t* used) {
    int n = 0;
    for (int stage = 0; stage < 4; ++stage)
        for (DWORD type : kUsedSampler) used[n++] = g_ssVal[stage][type];
    for (DWORD rs : kUsedRs) used[n++] = g_rsVal[rs];
    auto* tex = static_cast<IDirect3DBaseTexture9*>(const_cast<void*>(g_state[kTex0]));
    used[n++] = tex ? tex->GetLOD() : 0;
    used[n++] = static_cast<uint32_t>(g_miscVal[7]);                                 // stream 0 frequency: the instance count
    used[n++] = static_cast<uint32_t>(g_miscVal[8]);                                 // stream 1 frequency
    used[n++] = static_cast<uint32_t>(g_miscVal[1] ^ (g_miscVal[1] >> 32));          // stream 1 buffer
    used[n] = static_cast<uint32_t>(g_miscVal[4] ^ (g_miscVal[4] >> 32));            // stream 1 offset and stride
}
int DescribeUsed(const uint32_t* a, const uint32_t* b, char* out, size_t size) {
    int n = 0, i = 0;
    for (int stage = 0; stage < 4; ++stage)
        for (int k = 0; k < 8; ++k, ++i)
            if (a[i] != b[i] && n < static_cast<int>(size) - 60) n = Append(out, size, n, " s%d.%s %x->%x", stage, kUsedSamplerName[k], a[i], b[i]);
    for (int k = 0; k < 14; ++k, ++i)
        if (a[i] != b[i] && n < static_cast<int>(size) - 60) n = Append(out, size, n, " %s %x->%x", kUsedRsName[k], a[i], b[i]);
    if (a[i] != b[i] && n < static_cast<int>(size) - 60) n = Append(out, size, n, " tex0lod %u->%u", a[i], b[i]);
    static const char* const kInst[4] = {"instances", "stream1freq", "stream1buffer", "stream1offset"};
    for (int k = 0; k < 2; ++k)  // (the stream 1 buffer and offset are left over from other draws: noise)
        if (a[i + 1 + k] != b[i + 1 + k] && n < static_cast<int>(size) - 60)
            n = Append(out, size, n, " %s %x->%x", kInst[k], a[i + 1 + k], b[i + 1 + k]);
    return n;
}
// (state change detector) Who set the slope-scaled depth bias last, and the device's own value at each draw.
struct SlopeSet {
    DWORD value;
    uint32_t drawsBefore;
    const void* rt;
};
std::vector<SlopeSet> g_slopeSeq, g_slopeSeqPrev;  // the sets of this frame and of the frame before, in stream order
DWORD g_slopeSetterTid = 0, g_slopeSetValue = 0;
uintptr_t g_slopeSetterCaller = 0;
uint64_t g_slopeSets = 0, g_slopeTrackerWrong = 0;
void NoteSlopeSetter(DWORD v, void* caller) {
    g_slopeSetterTid = GetCurrentThreadId(), g_slopeSetValue = v, ++g_slopeSets;
    if (g_slopeSeq.size() < 200) g_slopeSeq.push_back({v, static_cast<uint32_t>(g_current.size()), g_rt0});
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const char*>(caller), &m);
    g_slopeSetterCaller = reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(m);
}
void NoteRealSlope(DWORD real) {
    if (real != g_rsVal[175]) {
        ++g_slopeTrackerWrong;
        g_rsMix ^= SlotMix(175, g_rsVal[175]) ^ SlotMix(175, real);
        g_rsVal[175] = real;  // the draw is recorded with the device's value
        g_slopeSetterTid = 0;  // (not set through SetRenderState: a state block or another path)
    }
}
void NoteRenderState(DWORD state, DWORD v) {
    if (state >= 256) return;
    g_rsMix ^= SlotMix(state, g_rsVal[state]) ^ SlotMix(state, v);
    g_rsVal[state] = v;
}
void NoteSamplerState(DWORD stage, DWORD type, DWORD v) {
    if (stage >= 16 || type >= 16) return;
    g_ssMix ^= SlotMix(stage * 16 + type, g_ssVal[stage][type]) ^ SlotMix(stage * 16 + type, v);
    g_ssVal[stage][type] = v;
}
void NoteBinding(int slot, uint64_t v) {  // 0 depth surface; 1-3 stream vb; 4-6 stream offset and stride; 7-10 stream frequency
    if (slot < 0 || slot >= 16) return;
    g_miscMix ^= SlotMix(slot, g_miscVal[slot]) ^ SlotMix(slot, v);
    g_miscVal[slot] = v;
}
const void* g_stageTex[8] = {};
uint64_t g_texN = 0;
uint64_t StageMix(DWORD stage, const void* t) { return (reinterpret_cast<uintptr_t>(t) + stage) * (0x9e3779b97f4a7c15ull + 2 * stage); }
void NoteTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    if (stage == 0) NoteState(kTex0, t);
    if (stage >= 1 && stage < 8) {
        g_texN ^= StageMix(stage, g_stageTex[stage]);
        g_stageTex[stage] = t;
        g_texN ^= StageMix(stage, t);
    }
    if (stage >= 16) return;
    if (t && IsShadowMap(t)) g_shadowStages |= uintptr_t{1} << stage;
    else g_shadowStages &= ~(uintptr_t{1} << stage);
    NoteState(kShadow, reinterpret_cast<const void*>(g_shadowStages));
}

void NoteDraw(INT baseVertex, UINT startIndex, UINT primCount) {
    Draw d;
    memcpy(d.state, g_state, sizeof d.state);
    d.base = baseVertex, d.start = startIndex, d.count = primCount;
    d.caller = g_caller, d.tid = g_callerTid;
    d.rt = g_rt0;
    d.texN = g_texN;
    d.rs = g_rsMix, d.ss = g_ssMix, d.misc = g_miscMix;
    FillUsed(d.used);
    d.slopeTid = g_slopeSetterTid, d.slopeCaller = static_cast<uint32_t>(g_slopeSetterCaller);
    memcpy(d.cg, g_groupHash, sizeof d.cg);
    d.vpX = g_vpX;
    d.texCaller = g_texCaller, d.texTid = g_texTid;
    d.fxCaller = g_fxCaller, d.fxTid = g_fxTid;
    d.fxMaterial = g_fxMaterial, d.fxSlot = g_fxSlot, d.fxSlotObject = g_fxSlotObject;
    if (d.tid != g_fxTid) ++g_foreign;
    d.foreign = g_foreign;
    g_caller = 0, g_callerTid = 0;
    g_current.push_back(d);
    // (diagnosis) the constants the first main-view pre-pass draw of each frame sees: hashes of
    // vertex and pixel registers 0-31 in groups of 8, per frame and oracle mode.
    static int logConsts = -1;
    if (logConsts < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        logConsts = GetPrivateProfileIntA("experiments", "oracle_log_consts", 0, ini);
    }
    if (logConsts && g_oracle && !g_constsLogged && d.state[kTechnique] &&
        !strcmp(static_cast<const char*>(d.state[kTechnique]), "DeferredPrePass")) {
        auto it = g_rtNames.find(d.rt);
        if (it != g_rtNames.end() && it->second.rfind("6144x3456", 0) == 0) {
            g_constsLogged = true;
            uint32_t h[8];
            for (int k = 0; k < 2; ++k)
                for (int g = 0; g < 4; ++g) {
                    uint32_t x = 2166136261u;
                    const auto* p = reinterpret_cast<const uint8_t*>(g_regs[k][g * 8]);
                    for (int i = 0; i < 8 * 16; ++i) x = (x ^ p[i]) * 16777619u;
                    h[k * 4 + g] = x;
                }
            Log("oracle: consts frame %llu mode %d pos %d vs %08x %08x %08x %08x ps %08x %08x %08x %08x",
                static_cast<unsigned long long>(g_frame), g_curMode, g_curStepPos, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        }
    }
}
bool PickBegin(IDirect3DDevice9* d) {
    if (!g_enabled || !PickFrame()) return false;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    rt->Release();
    if (desc.Width * 9 != desc.Height * 16 || desc.Width < g_backW) return false;
    if (!g_pickQuery && FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_pickQuery))) return false;
    LONG s = static_cast<LONG>(desc.Width / g_backW);
    if (FAILED(d->GetRenderState(D3DRS_SCISSORTESTENABLE, &g_pickSavedEnable))) g_pickSavedEnable = FALSE;
    if (FAILED(d->GetScissorRect(&g_pickSavedRect))) g_pickSavedRect = {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    RECT r{g_pickX * s, g_pickY * s, g_pickX * s + s, g_pickY * s + s};
    d->SetScissorRect(&r);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    g_pickQuery->Issue(D3DISSUE_BEGIN);
    return true;
}
void PickEnd(IDirect3DDevice9* d) {
    g_pickQuery->Issue(D3DISSUE_END);
    DWORD px = 0;
    for (int i = 0; i < 100000 && g_pickQuery->GetData(&px, sizeof px, D3DGETDATA_FLUSH) == S_FALSE; ++i) {}
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, g_pickSavedEnable);
    d->SetScissorRect(&g_pickSavedRect);
    if (!px || g_current.empty()) return;
    const Draw& dr = g_current.back();
    char line[1400];
    auto it = g_rtNames.find(dr.rt);
    int n = Append(line, sizeof line, 0, "pick: frame %llu draw %zu px %lu target %s technique %s tris %u caller rva %llx thread %s vs %p ps %p vsc %p psc %p |",
                   static_cast<unsigned long long>(g_frame), g_current.size() - 1, px, it != g_rtNames.end() ? it->second.c_str() : "?",
                   dr.state[kTechnique] ? static_cast<const char*>(dr.state[kTechnique]) : "-", dr.count,
                   static_cast<unsigned long long>(dr.caller ? dr.caller - g_engineBase : 0), ThreadName(dr.tid).c_str(), dr.state[kVS],
                   dr.state[kPS], dr.state[kVSConst], dr.state[kPSConst]);
    n = Append(line, sizeof line, n, " ib %p vb %p base %d start %u |", dr.state[kIB], dr.state[kVB], dr.base, dr.start);
    for (DWORD stage = 0; stage < 6; ++stage) n = TexInfo(d, stage, line, sizeof line, n);
    Log("%s", line);
    for (int k = 0; k < 2 && g_pickConsts > 0; ++k) {
        static char consts[64 * 4 * 16 + 64];
        int m = Append(consts, sizeof consts, 0, "pick: frame %llu draw %zu %s", static_cast<unsigned long long>(g_frame), g_current.size() - 1, k ? "ps" : "vs");
        for (int r = 0; r < g_pickConsts; ++r)
            m = Append(consts, sizeof consts, m, " c%d %.5g %.5g %.5g %.5g", r, g_regs[k][r][0], g_regs[k][r][1], g_regs[k][r][2], g_regs[k][r][3]);
        Log("%s", consts);
    }
    ++g_pickHits;
}
bool Enabled() { return g_enabled; }
bool PickWanted() {
    PickInit();
    return g_pickX >= 0;
}

// [experiments] glitch_census=1 (with glitch_detect=1): per frame, the draws and triangles per
// render target, engine call site, technique and recording thread, compared with the frame two
// before (the engine alternates its shadow work between frames). A difference is logged. The
// picture is not captured in this mode, so the frame timing stays close to a normal run. While
// the train stands, a stable scene gives the same counts in every frame.
bool CensusOnly() {
    static int on = -1;
    if (on < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        on = GetPrivateProfileIntA("experiments", "glitch_census", 0, ini);
    }
    return on != 0;
}
void DrawCensus() {
    using Key = std::tuple<const void*, uintptr_t, const void*, DWORD>;
    struct Sum { uint32_t draws = 0; uint64_t prims = 0; };
    static std::map<Key, Sum> prev[2];
    static int logged = 0;
    std::map<Key, Sum> cur;
    for (const Draw& x : g_current) {
        Sum& s = cur[{x.rt, x.caller, x.state[kTechnique], x.tid}];
        ++s.draws, s.prims += x.count;
    }
    std::map<Key, Sum>& old = prev[g_frame & 1];
    if (g_frame >= 240) {
        auto report = [&](const Key& k, const Sum& a, const Sum& b) {
            if (logged++ >= 20000) return;
            auto rt = g_rtNames.find(std::get<0>(k));
            const void* tech = std::get<2>(k);
            Log("census: frame %llu: target %s rva %llx %s thread %lu: draws %u -> %u, triangles %llu -> %llu",
                static_cast<unsigned long long>(g_frame), rt != g_rtNames.end() ? rt->second.c_str() : "?",
                static_cast<unsigned long long>(std::get<1>(k)), tech ? static_cast<const char*>(tech) : "-", std::get<3>(k), a.draws,
                b.draws, static_cast<unsigned long long>(a.prims), static_cast<unsigned long long>(b.prims));
        };
        for (const auto& [k, b] : cur) {
            auto it = old.find(k);
            Sum a = it == old.end() ? Sum{} : it->second;
            if (a.draws != b.draws || a.prims != b.prims) report(k, a, b);
        }
        for (const auto& [k, a] : old)
            if (!cur.count(k)) report(k, a, Sum{});
    }
    old.swap(cur);
    if (g_frame % 600 == 599) Log("census: frame %llu, %zu groups of draws in the last frame", static_cast<unsigned long long>(g_frame), old.size());
}

// Blink detector (part of the census mode): follows every draw by identity (render target, engine
// call site, technique, buffers, range). A draw that was there in every frame and is gone for one
// to three frames, or one that was not there and shows for one to three frames, is an event. Each
// event is logged at once with the draw's identity, the recording thread and how often it was
// drawn per frame. The engine alternates its shadow work between frames, so shadow-map draws are
// followed per frame parity.
struct BlinkKey {
    const void *rt, *tech, *vb, *ib;
    uintptr_t caller;
    UINT start, count;
    uint8_t parity;
    DWORD vpX;  // the viewport's left edge: which shadow map of the atlas (0 for other targets)
    bool operator==(const BlinkKey& o) const {
        return vpX == o.vpX && rt == o.rt && tech == o.tech && vb == o.vb && ib == o.ib && caller == o.caller && start == o.start && count == o.count &&
               parity == o.parity;
    }
};
struct BlinkKeyHash {
    size_t operator()(const BlinkKey& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(reinterpret_cast<uintptr_t>(k.rt)), mix(reinterpret_cast<uintptr_t>(k.tech)), mix(reinterpret_cast<uintptr_t>(k.vb));
        mix(reinterpret_cast<uintptr_t>(k.ib)), mix(k.caller), mix(k.start), mix(k.count), mix(k.parity), mix(k.vpX);
        return static_cast<size_t>(h);
    }
};
struct BlinkTrack {
    uint64_t first = 0, last = 0;  // step of the first and of the latest frame it was drawn in (of this streak)
    uint64_t gapFrom = 0;          // step it went missing at (0 = not missing)
    uint32_t streak = 0;           // steps in a row it was drawn
    uint32_t before = 0;           // the streak it had before the current one began
    uint16_t perFrame = 0;         // draws in the latest frame
    DWORD tid = 0;
    bool seen = false;
    uint32_t rs[14] = {};          // the render states it ran with in its latest frame (see kUsedRs)
};
// (census mode) The pictures around a main-picture draw that showed for one frame only: the frame
// before it, the frame with it, the frame after it, saved to %TEMP%	scfix-blink.
bool g_blinkShot = false;
char g_blinkShotWhat[300];
IDirect3DSurface9* g_blinkRing[2] = {};  // copies of the back buffers of the two frames before this one
int g_blinkShots = 0;
void BlinkPictures(IDirect3DDevice9* d, IDirect3DSurface9* back) {
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    using SaveFn = HRESULT(WINAPI*)(const char*, DWORD, IDirect3DSurface9*, const void*, const RECT*);
    static SaveFn save = nullptr;
    if (!save)
        if (HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll")) save = reinterpret_cast<SaveFn>(GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA"));
    if (g_blinkShot && save && g_blinkRing[0] && g_blinkRing[1] && g_blinkShots < 16 && g_frame > 400) {
        char dir[MAX_PATH], path[MAX_PATH];
        GetTempPathA(MAX_PATH, dir);
        strcat_s(dir, "tscfix-blink");
        CreateDirectoryA(dir, nullptr);
        // ring[g_frame & 1] holds frame N-2 (before the draw), the other holds frame N-1 (with the draw); `back` is frame N (after it).
        IDirect3DSurface9* three[3] = {g_blinkRing[g_frame & 1], g_blinkRing[(g_frame + 1) & 1], back};
        static const char* names[3] = {"a-before", "b-with", "c-after"};
        for (int i = 0; i < 3; ++i) {
            sprintf_s(path, "%s\ev%02d-%s.png", dir, g_blinkShots, names[i]);
            save(path, 3 /* PNG */, three[i], nullptr, nullptr);
        }
        Log("blink: pictures ev%02d (frames %llu to %llu): %s", g_blinkShots, static_cast<unsigned long long>(g_frame - 2),
            static_cast<unsigned long long>(g_frame), g_blinkShotWhat);
        ++g_blinkShots;
    }
    g_blinkShot = false;
    IDirect3DSurface9*& slot = g_blinkRing[g_frame & 1];
    if (!slot) d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &slot, nullptr);
    if (slot) d->StretchRect(back, nullptr, slot, nullptr, D3DTEXF_NONE);
}

// Render states that change from one frame to the next for the same draw: counted per state.
uint64_t g_stateJitter[14] = {}, g_stateJitterDraws = 0;
void BlinkDetect() {
    static std::unordered_map<BlinkKey, BlinkTrack, BlinkKeyHash> tracks;
    static uint64_t events = 0, gone = 0, shown = 0;
    static uint64_t byKind[3][2] = {};  // [shadow map, large main picture, other target][gone, shown]
    auto kindOf = [](const BlinkKey& k) {
        if (k.parity != 2) return 0;
        auto rt = g_rtNames.find(k.rt);
        unsigned w = rt != g_rtNames.end() ? static_cast<unsigned>(atoi(rt->second.c_str())) : 0;
        return w > g_backW ? 1 : 2;  // (the supersampled picture is larger than the back buffer; window-size targets hold the interface and post passes)
    };
    static int logged = 0;
    uint8_t frameParity = static_cast<uint8_t>(g_frame & 1);
    uint64_t step = g_frame / 2 + 1;  // counts frames of one parity
    auto isMap = [](const Draw& x) {
        auto rt = g_rtNames.find(x.rt);
        return rt != g_rtNames.end() && strstr(rt->second.c_str(), "fmt 114") != nullptr;
    };
    auto describe = [](const BlinkKey& k, char* out, size_t size) {
        auto rt = g_rtNames.find(k.rt);
        sprintf_s(out, size, "target %s map-x %lu rva %llx %s vb %p start %u count %u", rt != g_rtNames.end() ? rt->second.c_str() : "?", k.vpX,
                  static_cast<unsigned long long>(k.caller), k.tech ? static_cast<const char*>(k.tech) : "-", k.vb, k.start, k.count);
    };
    for (auto& [k, tr] : tracks) tr.seen = false;
    for (const Draw& x : g_current) {
        // Main-picture draws are followed in every frame (parity 2), shadow-map draws per parity.
        bool map = isMap(x);
        BlinkKey k{x.rt, x.state[kTechnique], x.state[kVB], x.state[kIB], x.caller, x.start, x.count, map ? frameParity : uint8_t{2}, map ? x.vpX : 0};
        BlinkTrack& tr = tracks[k];
        uint64_t now = k.parity == 2 ? g_frame + 1 : step;
        if (tr.seen) {
            ++tr.perFrame;
            continue;
        }
        tr.seen = true;
        tr.tid = x.tid;
        tr.perFrame = 1;
        // The same draw in the frame before, with other depth, blend or cull states: a state left over
        // from another part of the frame reaches it in some frames only.
        const uint32_t* rs = x.used + 32;
        if (tr.last == now - 1 && tr.streak >= 30 && g_frame >= 300 && memcmp(tr.rs, rs, sizeof tr.rs) != 0) {
            ++g_stateJitterDraws;
            char what[300], diff[360];
            int n = 0;
            diff[0] = 0;
            for (int i = 0; i < 14; ++i)
                if (tr.rs[i] != rs[i]) {
                    ++g_stateJitter[i];
                    if (n < 300) n += sprintf_s(diff + n, sizeof diff - n, " %s %x->%x", kUsedRsName[i], tr.rs[i], rs[i]);
                }
            static int stateLogged = 0;
            if (stateLogged++ < 600) {
                describe(k, what, sizeof what);
                Log("state: frame %llu: same draw, other state than the frame before:%s: %s; thread %lu; slope bias last set by thread %lu from module offset %x",
                    static_cast<unsigned long long>(g_frame), diff, what, tr.tid, x.slopeTid, x.slopeCaller);
            }
        }
        memcpy(tr.rs, rs, sizeof tr.rs);
        if (tr.gapFrom) {  // back after a gap
            uint64_t gap = now - tr.gapFrom;
            if (gap <= 3 && tr.streak >= 30 && g_frame >= 300) {
                ++events, ++gone;
                int kind = kindOf(k);
                ++byKind[kind][0];
                if (kind != 2 && logged++ < 4000) {
                    char what[300];
                    describe(k, what, sizeof what);
                    Log("blink: frame %llu: GONE for %llu frame(s) after %u in a row: %s; thread %lu", static_cast<unsigned long long>(g_frame),
                        static_cast<unsigned long long>(gap), tr.streak, what, tr.tid);
                }
                tr.streak += 1;  // the streak goes on
            } else {
                tr.before = tr.streak, tr.streak = 1, tr.first = now;
            }
            tr.gapFrom = 0;
        } else if (tr.last == 0) {
            tr.streak = 1, tr.first = now;
        } else {
            ++tr.streak;
        }
        tr.last = now;
    }
    // Draws that were not drawn in this frame.
    for (auto it = tracks.begin(); it != tracks.end();) {
        BlinkTrack& tr = it->second;
        const BlinkKey& k = it->first;
        if (k.parity != 2 && k.parity != frameParity) {
            ++it;
            continue;
        }
        uint64_t now = k.parity == 2 ? g_frame + 1 : step;
        if (!tr.seen && tr.last) {
            if (!tr.gapFrom) {
                tr.gapFrom = now;
                // It showed for one to three frames only, after a long absence or never before: an extra draw.
                if (tr.streak <= 3 && tr.before == 0 && tr.first > 150 && g_frame >= 300) {
                    ++events, ++shown;
                    int kind = kindOf(k);
                    ++byKind[kind][1];
                    if (kind == 1 && tr.streak == 1 && !g_blinkShot) {  // a main-picture draw that was in the frame before only: keep the pictures
                        g_blinkShot = true;
                        describe(k, g_blinkShotWhat, sizeof g_blinkShotWhat);
                    }
                    if (kind != 2 && logged++ < 4000) {
                        char what[300];
                        describe(k, what, sizeof what);
                        Log("blink: frame %llu: SHOWN for %u frame(s) only: %s; thread %lu", static_cast<unsigned long long>(g_frame),
                            tr.streak, what, tr.tid);
                    }
                }
            } else if (now - tr.gapFrom > 600) {  // long gone: forget it
                it = tracks.erase(it);
                continue;
            }
        }
        ++it;
    }
    {   // A frame in which draws got another slope bias: its sets and those of the frame before, in stream order.
        static uint64_t slopeBefore = 0;
        static int dumps = 0;
        if ((g_stateJitter[12] > slopeBefore + 20 && dumps < 6) || g_frame == 2000 || g_frame == 2600) {
            ++dumps;
            for (int which = 0; which < 2; ++which) {
                const auto& seq = which ? g_slopeSeq : g_slopeSeqPrev;
                char text[1900];
                int n = 0;
                text[0] = 0;
                for (const SlopeSet& e : seq) {
                    auto rt = g_rtNames.find(e.rt);
                    if (n < 1800)
                        n += sprintf_s(text + n, sizeof text - n, " [%g after %u draws, target %.12s]", static_cast<double>(*reinterpret_cast<const float*>(&e.value)),
                                       e.drawsBefore, rt != g_rtNames.end() ? rt->second.c_str() : "?");
                }
                Log("state: slope bias sets of %s (%llu, %zu draws in all):%s", which ? "the frame with other values" : "the frame before",
                    static_cast<unsigned long long>(g_frame - (which ? 0 : 1)), which ? g_current.size() : size_t{0}, text);
            }
        }
        slopeBefore = g_stateJitter[12];
        g_slopeSeqPrev.swap(g_slopeSeq);
        g_slopeSeq.clear();
    }
    if (g_frame % 600 == 599) {
        char text[400];
        int n = 0;
        text[0] = 0;
        for (int i = 0; i < 14; ++i)
            if (g_stateJitter[i]) n += sprintf_s(text + n, sizeof text - n, " %s %llu", kUsedRsName[i], static_cast<unsigned long long>(g_stateJitter[i]));
        Log("state: frame %llu: %llu draws so far with a render state other than in the frame before; per state:%s; slope bias: %llu sets, the device had "
            "another value than the hooks followed at %llu draws",
            static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_stateJitterDraws), n ? text : " none",
            static_cast<unsigned long long>(g_slopeSets), static_cast<unsigned long long>(g_slopeTrackerWrong));
    }
    if (g_frame % 600 == 599)
        Log("blink: frame %llu: by target: shadow maps %llu gone, %llu shown; main picture %llu gone, %llu shown; other targets %llu gone, %llu shown",
            static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(byKind[0][0]), static_cast<unsigned long long>(byKind[0][1]),
            static_cast<unsigned long long>(byKind[1][0]), static_cast<unsigned long long>(byKind[1][1]), static_cast<unsigned long long>(byKind[2][0]),
            static_cast<unsigned long long>(byKind[2][1]));
    if (g_frame % 600 == 599)
        Log("blink: frame %llu: %llu events so far (%llu draws gone for 1-3 frames, %llu draws shown for 1-3 frames only); %zu draws followed",
            static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(events), static_cast<unsigned long long>(gone),
            static_cast<unsigned long long>(shown), tracks.size());
}

void OnPresent(IDirect3DDevice9* d) {
    if (!g_enabled) return;
    StepVariants(g_frame);
    if (g_watchArmed && g_frame % 300 == 0) LogWatch();
    IDirect3DSurface9* back = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    {
        D3DSURFACE_DESC bd{};
        back->GetDesc(&bd);
        g_backW = bd.Width;
        if (g_pickKey && g_pickTimes == 0 && (GetAsyncKeyState(g_pickKey) & 0x8000)) {
            g_pickFrame = g_frame + 1, g_pickTimes = g_pickPerPress;
            Log("pick: key pressed, picks start at frame %llu", static_cast<unsigned long long>(g_pickFrame));
        }
        if (g_pickX >= 0 && g_pickTimes > 0 && g_frame == g_pickFrame) {
            Log("pick: frame %llu done, %d draws reached pixel %d,%d", static_cast<unsigned long long>(g_frame), g_pickHits, g_pickX, g_pickY);
            g_pickHits = 0, g_pickFrame += g_pickEvery, --g_pickTimes;
        }
    }
    if (CensusOnly()) {  // no picture capture: the frame's draws are counted and compared, nothing else
        DrawCensus();
        BlinkDetect();
        BlinkPictures(d, back);
        back->Release();
        g_current.clear();
        ++g_frame;
        return;
    }
    if (g_oracle) OracleFullFrame(d, back, g_curMode);
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    bool ok = Ensure(d, desc.Format) && SUCCEEDED(d->StretchRect(back, nullptr, g_small, nullptr, D3DTEXF_LINEAR)) &&
              SUCCEEDED(d->GetRenderTargetData(g_small, g_sys));
    back->Release();
    if (!ok) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    std::vector<uint8_t> f(kW * kH);
    for (int y = 0; y < kH; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
        for (int x = 0; x < kW; ++x) {  // X8R8G8B8 / A8R8G8B8: B, G, R
            const uint8_t* p = row + 4 * x;
            f[y * kW + x] = static_cast<uint8_t>((p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8);
        }
    }
    g_sys->UnlockRect();
    g_frames[0].swap(g_frames[1]);
    g_frames[1].swap(g_frames[2]);
    g_frames[2].swap(f);
    // The frame just drawn becomes the newest; the ring lines up with g_frames.
    g_draws[0].swap(g_draws[1]);
    g_draws[1].swap(g_draws[2]);
    g_draws[2].swap(g_current);
    OracleFrameDone();
    g_current.clear();
    ++g_frame;
    if (g_frame % 300 == 0 && !g_draws[2].empty()) {  // which draws sample a shadow map (newest frame)
        std::map<uintptr_t, int> stages;
        for (const Draw& x : g_draws[2]) ++stages[reinterpret_cast<uintptr_t>(x.state[kShadow])];
        char line[300];
        int n = 0;
        for (auto& [mask, count] : stages)
            if (n < 260) n = Append(line, sizeof line, n, " stages %llx: %d;", static_cast<unsigned long long>(mask), count);
        Log("glitch: draws by shadow-map stages:%s", line);
    }
    g_capRing[0].swap(g_capRing[1]);
    g_capRing[1].swap(g_capRing[2]);
    g_capRing[2].swap(g_capCurrent);
    g_capCurrent.clear();
    if (g_have + 1 >= 3) CheckCaptures(g_frame - 1);
    if (++g_have >= 3) Check();
    if (g_frame % 600 == 0) {
        Log("glitch: last 600 frames: %llu one-frame events (total %llu, %llu flagged pixels)",
            static_cast<unsigned long long>(g_windowEvents), static_cast<unsigned long long>(g_events),
            static_cast<unsigned long long>(g_flagged));
        g_windowEvents = 0;
        char hot[256] = "";
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 24; ++x)
                if (g_cells[y][x] >= 3) {
                    size_t len = strlen(hot);
                    Append(hot, sizeof hot, static_cast<int>(len), " (%d,%d):%u", x, y, g_cells[y][x]);
                }
        if (hot[0]) Log("glitch: repeated spots (grid cell 24x12: events so far):%s", hot);
    }
}

}  // namespace tscfix::glitch
