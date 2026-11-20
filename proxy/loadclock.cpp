#include "loadclock.h"

#include <MinHook.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "sites.h"

// The engine's loading fibers (game version v79.0a) share the game thread. The frame loop gives
// them a turn once per frame (the yield, "forced"). A worker fiber loads one object and then
// calls the yield "if my slice is used". The slice is 2.0 ms for all workers together.
//
// The slice clock is a function that returns GetTickCount() as a float, in milliseconds. It has
// three callers: the fiber setup, the yield, and a second yield. Each one either stores a reading
// as "start of this turn" or compares "reading - start" with the slice. No caller uses the
// absolute value. The tick count steps by the system tick (15.6 ms), and a float has 24 bits, so
// the reading steps by 2 ms after 4.7 hours of Windows uptime and by 64 ms after 6 days. A 2 ms
// slice then lasts until the next step.
//
// [fixes] precise_load_clock: the clock function is replaced by one from QueryPerformanceCounter:
// milliseconds as a float, counted from a zero point of our own. A float of milliseconds is exact
// to 0.008 ms only below 65 seconds, so the zero point moves forward when the reading passes 60
// seconds. The engine keeps one stored reading (the start of the turn). It is moved by the same
// amount in the same step, so "reading - start" does not change. The same is done when the clock
// is switched on or off during play. This is correct because all readers are fibers of one thread.
//
// [experiments] fiber_time: the yield is hooked. The time from the frame loop's yield call to its
// return is the time all loading fibers took in that frame.
namespace tscfix::loadclock {
namespace {

constexpr sites::Fingerprint kClock = {0xf44c3a5cb647ea50ull, 16};
constexpr sites::Fingerprint kYield = {0x45da47d4452a69d9ull, 64};  // (covers both operands read below)
constexpr sites::Fingerprint kFrameLoop = {0x4c99c43d1bb6f64bull, 16};
// In the yield: the instruction that subtracts the stored start, and the one that loads the index
// of the current fiber. Both address their operand relative to themselves.
constexpr size_t kStartInsn = 0x14, kStartDispAt = 4, kStartInsnLength = 8;
constexpr size_t kCurrentInsn = 0x2d, kCurrentDispAt = 3, kCurrentInsnLength = 7;
// In the frame loop: the call of the yield (5 bytes, target relative to its end).
constexpr size_t kFrameLoopCall = 0x895;
constexpr double kRebaseMs = 60000.0;

using ClockFn = float (*)();
using YieldFn = void (*)(char forced);
ClockFn g_clock = nullptr;
YieldFn g_yield = nullptr;
float* g_start = nullptr;       // engine: the reading at the start of the current fiber's turn
const int* g_current = nullptr; // engine: index of the current fiber (0 = the game thread's own)
void* g_frameLoopReturn = nullptr;

double g_msPerTick = 0;
LONGLONG g_zero = 0;            // counter value at reading 0
volatile bool g_want = false;
bool g_precise = false;         // which clock the stored start belongs to (fibers' thread only)
volatile DWORD g_thread = 0;
volatile LONG g_foreign = 0, g_rebases = 0;

LONGLONG Counter() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

float Clock() {
    DWORD thread = GetCurrentThreadId();
    if (!g_thread)  // (the first thread claims it, once)
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_thread), static_cast<LONG>(thread), 0);
    if (thread != g_thread) {  // not expected: only fibers of one thread read this clock
        InterlockedIncrement(&g_foreign);
        return g_precise ? static_cast<float>(static_cast<double>(Counter() - g_zero) * g_msPerTick) : g_clock();
    }
    bool want = g_want;
    if (want != g_precise) {  // change of clock: move the stored start to the new clock
        LONGLONG now = Counter();
        double stock = g_clock();
        double before = g_precise ? static_cast<double>(now - g_zero) * g_msPerTick : stock;
        if (want) g_zero = now;
        double after = want ? 0.0 : stock;
        *g_start = static_cast<float>(static_cast<double>(*g_start) + (after - before));
        g_precise = want;
        Log("loadclock: slice clock is now %s", want ? "precise" : "the engine's tick count");
    }
    if (!g_precise) return g_clock();
    LONGLONG now = Counter();
    double ms = static_cast<double>(now - g_zero) * g_msPerTick;
    if (ms >= kRebaseMs) {  // keep the float small: move the zero point and the stored start together
        *g_start = static_cast<float>(static_cast<double>(*g_start) - ms);
        g_zero = now;
        ms = 0.0;
        InterlockedIncrement(&g_rebases);
    }
    return static_cast<float>(ms);
}

// --- [experiments] fiber_time ---------------------------------------------------------------
struct Stats {
    int frames;
    double total, longest;      // ms in the frame loop's yield
    int over4, over8, over16, over33;
    int waits;                  // other yields of the game thread's own fiber that switched away
    double waitTotal, waitLongest;
};
Stats g_stats[2];               // [0] engine clock, [1] precise clock
constexpr int kReportFrames = 600;

void FiberYield(char forced) {
    void* from = _ReturnAddress();
    bool frame = from == g_frameLoopReturn;
    if (!frame && *g_current != 0) {  // a loading fiber gives up its turn: not timed
        g_yield(forced);
        return;
    }
    LONGLONG t0 = Counter();
    g_yield(forced);
    double ms = static_cast<double>(Counter() - t0) * g_msPerTick;
    Stats& s = g_stats[g_precise ? 1 : 0];
    if (!frame) {
        if (ms < 0.01) return;  // no fiber had work, or the slice was not used yet
        ++s.waits;
        s.waitTotal += ms;
        if (ms > s.waitLongest) s.waitLongest = ms;
        return;
    }
    ++s.frames;
    s.total += ms;
    if (ms > s.longest) s.longest = ms;
    s.over4 += ms > 4.0;
    s.over8 += ms > 8.0;
    s.over16 += ms > 16.0;
    s.over33 += ms > 33.0;
    if (s.frames < kReportFrames) return;
    Log("fibertime: %s clock, %d frames: mean %.3f ms, longest %.2f ms; frames over 4/8/16/33 ms: %d/%d/%d/%d; "
        "other waits of the game thread: %d, total %.1f ms, longest %.2f ms",
        g_precise ? "precise" : "engine", s.frames, s.total / s.frames, s.longest, s.over4, s.over8, s.over16, s.over33,
        s.waits, s.waitTotal, s.waitLongest);
    if (g_precise && (g_rebases || g_foreign))
        Log("fibertime: clock zero point moved %ld times; %ld readings from another thread", g_rebases, g_foreign);
    s = Stats{};
}

// The operand of an instruction that addresses it relative to itself; nullptr if outside the module.
BYTE* Operand(HMODULE engine, BYTE* insn, size_t dispAt, size_t insnLength) {
    int32_t disp;
    memcpy(&disp, insn + dispAt, 4);
    BYTE* at = insn + insnLength + disp;
    auto base = reinterpret_cast<BYTE*>(engine);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return at >= base && at + 8 <= base + nt->OptionalHeader.SizeOfImage ? at : nullptr;
}

}  // namespace

bool Install(HMODULE engine, bool clock, bool fiberTime) {
    // All fingerprints first: a hook changes the first bytes of its function.
    auto clockFn = static_cast<BYTE*>(sites::Find(engine, kClock));
    auto yieldFn = static_cast<BYTE*>(sites::Find(engine, kYield));
    auto frameLoop = static_cast<BYTE*>(sites::Find(engine, kFrameLoop));
    if (!clockFn || !yieldFn) {
        Log("loadclock: %s not found (game version changed, or hooked before this); disabled", clockFn ? "fiber yield" : "slice clock");
        return false;
    }
    g_start = reinterpret_cast<float*>(Operand(engine, yieldFn + kStartInsn, kStartDispAt, kStartInsnLength));
    g_current = reinterpret_cast<const int*>(Operand(engine, yieldFn + kCurrentInsn, kCurrentDispAt, kCurrentInsnLength));
    if (!g_start || !g_current) {
        Log("loadclock: fiber state not found; disabled");
        return false;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);

    MH_Initialize();
    bool any = false;
    if (clock) {
        bool ok = MH_CreateHook(clockFn, reinterpret_cast<void*>(&Clock), reinterpret_cast<void**>(&g_clock)) == MH_OK &&
                  MH_EnableHook(clockFn) == MH_OK;
        Log("loadclock: slice clock %s", ok ? "hooked (the engine's tick count until switched on)" : "hook failed");
        any |= ok;
    }
    if (fiberTime) {
        // The frame loop's call must be the yield, else the frame cannot be told from other calls.
        BYTE* call = frameLoop ? frameLoop + kFrameLoopCall : nullptr;
        if (!call || Operand(engine, call, 1, 5) != yieldFn) {
            Log("fibertime: the frame loop's yield call not found; instrument off");
        } else {
            g_frameLoopReturn = call + 5;
            bool ok = MH_CreateHook(yieldFn, reinterpret_cast<void*>(&FiberYield), reinterpret_cast<void**>(&g_yield)) == MH_OK &&
                      MH_EnableHook(yieldFn) == MH_OK;
            Log("fibertime: fiber yield %s; one line per %d frames", ok ? "hooked" : "hook failed", kReportFrames);
            any |= ok;
        }
    }
    return any;
}

void SetActive(bool active) { g_want = active; }

}  // namespace tscfix::loadclock
