#include "profiler.h"

#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3dlayer.h"
#include "log.h"
#include "luaprof.h"

namespace tscfix::profiler {
namespace {

constexpr int kMaxDepth = 48;
constexpr size_t kMaxSamples = 200000;

struct Module {
    uintptr_t base = 0;
    uintptr_t end = 0;
    const RUNTIME_FUNCTION* pdata = nullptr;
    DWORD pdataCount = 0;
    char name[64] = {};
};

struct Frame {
    uint16_t module;   // index into g_modules, 0xFFFF = unknown
    uint32_t rva;      // function start RVA if known, else the address RVA
};

struct Sample {
    int64_t qpc;  // when the sample was taken
    uint8_t depth;
    Frame leaf;  // exact instruction of the innermost frame (frames[0] holds its function start)
    Frame frames[kMaxDepth];
};

std::vector<Module> g_modules;
constexpr size_t kMaxFrames = 1 << 17;
int64_t* g_frames = nullptr;           // Present timestamps while sampling (game thread writes)
volatile LONG g_frameCount = 0;
volatile bool g_sampling = false;
constexpr double kSpikeMs = 100.0;
Sample* g_samples = nullptr;
size_t g_sampleCount = 0;
DWORD g_targetTid = 0;
int g_intervalUs = 1000;

void RefreshModules() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    std::vector<Module> list;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        auto base = reinterpret_cast<BYTE*>(mods[i]);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        Module m;
        m.base = reinterpret_cast<uintptr_t>(base);
        m.end = m.base + nt->OptionalHeader.SizeOfImage;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (dir.VirtualAddress && dir.Size) {
            m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
            m.pdataCount = dir.Size / sizeof(RUNTIME_FUNCTION);
        }
        K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], m.name, sizeof m.name);
        list.push_back(m);
    }
    std::sort(list.begin(), list.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    g_modules.swap(list);
}

// Lock-free lookups over the cached module list; safe while the target is suspended.
int FindModule(uintptr_t pc) {
    size_t lo = 0, hi = g_modules.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (pc < g_modules[mid].base) hi = mid;
        else if (pc >= g_modules[mid].end) lo = mid + 1;
        else return static_cast<int>(mid);
    }
    return -1;
}

const RUNTIME_FUNCTION* FindFunction(const Module& m, DWORD rva) {
    size_t lo = 0, hi = m.pdataCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& rf = m.pdata[mid];
        if (rva < rf.BeginAddress) hi = mid;
        else if (rva >= rf.EndAddress) lo = mid + 1;
        else return &rf;
    }
    return nullptr;
}

// Walks the stack described by ctx into s. Runs while the target is suspended.
void Unwind(CONTEXT& ctx, Sample& s) {
    s.depth = 0;
    s.leaf = {0xFFFF, 0};
    __try {
        for (int d = 0; d < kMaxDepth && ctx.Rip; ++d) {
            uintptr_t pc = ctx.Rip;
            int mi = FindModule(pc);
            const RUNTIME_FUNCTION* rf = nullptr;
            if (mi >= 0) {
                const Module& m = g_modules[mi];
                // Return addresses point after the call; look up pc-1 for callers.
                DWORD rva = static_cast<DWORD>((d ? pc - 1 : pc) - m.base);
                rf = m.pdata ? FindFunction(m, rva) : nullptr;
                s.frames[d] = {static_cast<uint16_t>(mi), rf ? rf->BeginAddress : static_cast<uint32_t>(pc - m.base)};
                if (!d) s.leaf = {static_cast<uint16_t>(mi), static_cast<uint32_t>(pc - m.base)};
            } else {
                s.frames[d] = {0xFFFF, 0};
            }
            s.depth = static_cast<uint8_t>(d + 1);
            if (mi < 0) break;
            if (rf) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, g_modules[mi].base, pc,
                                 const_cast<PRUNTIME_FUNCTION>(rf), &ctx, &handlerData, &establisher, nullptr);
            } else {  // leaf function: return address is on top of the stack
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Truncated stack; keep what we have.
    }
}

std::string FrameName(const Frame& f) {
    if (f.module == 0xFFFF) return "?";
    char buf[96];
    sprintf_s(buf, "%s!0x%x", g_modules[f.module].name, f.rva);
    return buf;
}

template <typename Keep>
void WriteReport(const char* path, double seconds, Keep keep) {
    std::unordered_map<std::string, uint32_t> stacks;
    for (size_t i = 0; i < g_sampleCount; ++i) {
        if (!keep(g_samples[i])) continue;
        const Sample& s = g_samples[i];
        std::string key;
        for (int d = s.depth - 1; d >= 0; --d) {
            key += FrameName(s.frames[d]);
            if (d) key += ';';
        }
        ++stacks[key];
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "# samples=%zu seconds=%.2f interval_us=%d thread=%lu\n", g_sampleCount, seconds,
            g_intervalUs, g_targetTid);
    for (const Module& m : g_modules) {
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(m.base), modPath, MAX_PATH);
        fprintf(f, "# module %s %s\n", m.name, modPath);
    }
    // Instruction-level hot spots: "# leaf module!0xrva count" (comment lines, so stack tools skip them).
    std::unordered_map<std::string, uint32_t> leaves;
    for (size_t i = 0; i < g_sampleCount; ++i)
        if (g_samples[i].leaf.module != 0xFFFF) ++leaves[FrameName(g_samples[i].leaf)];
    for (auto& [k, n] : leaves)
        if (n > 1) fprintf(f, "# leaf %s %u\n", k.c_str(), n);
    for (auto& [k, n] : stacks) fprintf(f, "%s %u\n", k.c_str(), n);
    fclose(f);
}

DWORD WINAPI SamplerThread(void*) {
    DWORD pid = GetCurrentProcessId();
    wchar_t name[96];
    swprintf_s(name, L"Local\\tscfix.profile.start.%lu", pid);
    HANDLE evStart = CreateEventW(nullptr, TRUE, FALSE, name);
    swprintf_s(name, L"Local\\tscfix.profile.stop.%lu", pid);
    HANDLE evStop = CreateEventW(nullptr, TRUE, FALSE, name);
    swprintf_s(name, L"Local\\tscfix.profile.done.%lu", pid);
    HANDLE evDone = CreateEventW(nullptr, TRUE, FALSE, name);

    g_samples = static_cast<Sample*>(VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_samples || !evStart || !evStop) {
        Log("profiler: setup failed (%lu)", GetLastError());
        return 0;
    }
    Log("profiler: ready, waiting for start event");
    WaitForSingleObject(evStart, INFINITE);
    HANDLE target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, g_targetTid);
    if (!target) {
        Log("profiler: cannot open thread %lu (%lu)", g_targetTid, GetLastError());
        return 0;
    }
    Log("profiler: sampling thread %lu", g_targetTid);
    luaprof::SetActive(true);
    d3d::ResetCensus();
    RefreshModules();
    Log("profiler: sampling every %d us, %zu modules", g_intervalUs, g_modules.size());

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    g_frameCount = 0;
    g_sampling = true;
    alignas(16) CONTEXT ctx;
    size_t sinceRefresh = 0;

    while (WaitForSingleObject(evStop, 0) == WAIT_TIMEOUT && g_sampleCount < kMaxSamples) {
        LARGE_INTEGER due;
        due.QuadPart = -10LL * g_intervalUs;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);

        if (SuspendThread(target) == static_cast<DWORD>(-1)) break;
        LARGE_INTEGER ctxTime;
        QueryPerformanceCounter(&ctxTime);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(target, &ctx)) {
            Unwind(ctx, g_samples[g_sampleCount]);
            g_samples[g_sampleCount].qpc = ctxTime.QuadPart;
            ResumeThread(target);
            ++g_sampleCount;
        } else {
            ResumeThread(target);
        }
        if (++sinceRefresh == 5000) {  // pick up modules loaded since start
            RefreshModules();
            sinceRefresh = 0;
        }
    }
    QueryPerformanceCounter(&t1);
    g_sampling = false;
    luaprof::SetActive(false);
    double seconds = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    Log("profiler: %zu samples in %.1f s", g_sampleCount, seconds);

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-profile.txt");
    WriteReport(path, seconds, [](const Sample&) { return true; });
    Log("profiler: wrote %s", path);
    // Slow frames: [start, end) intervals of frames longer than kSpikeMs.
    std::vector<std::pair<int64_t, int64_t>> spikes;
    double spikeSeconds = 0;
    for (LONG i = 1; i < g_frameCount; ++i) {
        double ms = double(g_frames[i] - g_frames[i - 1]) * 1000.0 / freq.QuadPart;
        if (ms > kSpikeMs) spikes.push_back({g_frames[i - 1], g_frames[i]}), spikeSeconds += ms / 1000.0;
    }
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-profile-spikes.txt");
    WriteReport(path, spikeSeconds, [&](const Sample& s) {
        for (auto& [a, b] : spikes)
            if (s.qpc >= a && s.qpc < b) return true;
        return false;
    });
    Log("profiler: %zu frame(s) over %.0f ms -> %s", spikes.size(), kSpikeMs, path);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-lua.txt");
    luaprof::WriteReport(path, seconds);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-d3d.txt");
    d3d::WriteCensus(path);
    SetEvent(evDone);
    return 0;
}

}  // namespace

void Start(DWORD targetThreadId, int intervalUs) {
    g_targetTid = targetThreadId;
    g_frames = static_cast<int64_t*>(VirtualAlloc(nullptr, sizeof(int64_t) * kMaxFrames, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_intervalUs = intervalUs;
    if (HANDLE t = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void Retarget(DWORD threadId) { g_targetTid = threadId; }

void OnPresent() {
    if (!g_sampling || !g_frames) return;
    LONG n = g_frameCount;
    if (n >= static_cast<LONG>(kMaxFrames)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_frames[n] = now.QuadPart;
    g_frameCount = n + 1;
}

}  // namespace tscfix::profiler
