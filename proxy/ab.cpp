#include "ab.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "log.h"
#include "status.h"

namespace tscfix::ab {
namespace {

constexpr int kSettleFrames = 10;  // frames ignored after each switch
constexpr bool kPattern[4] = {true, false, false, true};  // phase p (1-based) runs kPattern[(p - 1) % 4]

std::string g_feature;
int g_period = 150;
Toggle g_toggle = nullptr;
struct Feature { std::string name; Toggle toggle; };
std::vector<Feature> g_features;  // everything installed for this session; one is measured per window
HANDLE g_evStart = nullptr, g_evStop = nullptr, g_evDone = nullptr;

bool g_running = false, g_finished = false;
bool g_on = false;
int g_framesInPhase = 0;
LARGE_INTEGER g_lastQpc{};
ULONG64 g_lastCycles = 0;
LARGE_INTEGER g_startQpc{};
unsigned long long g_startTsc = 0;

struct Sample { float frameMs; float cpuCycles; unsigned phase; };
unsigned g_phase = 0;  // increments at every switch, so jitter never spans two phases
std::vector<Sample> g_samples[2];  // [off, on]
// Stutter accounting over every frame, including the settle frames right after a switch
// (a switch that itself causes a stall would otherwise go unnoticed): [state][0 = steady, 1 = settle].
constexpr float kHitchMs = 50.0f;
unsigned g_hitches[2][2] = {};
float g_worst[2][2] = {};

ULONG64 ThreadCycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

float Percentile(std::vector<float>& v, double p) {
    if (v.empty()) return 0;
    size_t k = static_cast<size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Blocks run in ABBA quartets (on, off, off, on). Each quartet gives one independent estimate,
// mean log(on) - mean log(off), which cancels linear drift in the scene and any ordering effect.
// Reports the mean over quartets with a bootstrap 95% interval.
struct Block { unsigned phase; bool on; float ms; float cpu; size_t frames; };

std::vector<Block> Blocks(double cyclesPerMs) {
    std::vector<Block> blocks;
    for (int s = 0; s < 2; ++s) {
        size_t i = 0;
        while (i < g_samples[s].size()) {
            unsigned phase = g_samples[s][i].phase;
            std::vector<float> ms, cpu;
            for (; i < g_samples[s].size() && g_samples[s][i].phase == phase; ++i) {
                ms.push_back(g_samples[s][i].frameMs);
                cpu.push_back(float(g_samples[s][i].cpuCycles / cyclesPerMs));
            }
            size_t n = ms.size();
            blocks.push_back({phase, s == 1, Percentile(ms, 0.5), Percentile(cpu, 0.5), n});
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.phase < b.phase; });
    return blocks;
}

void Interval(const std::vector<double>& d, double& mean, double& lo, double& hi) {
    mean = lo = hi = 0;
    if (d.empty()) return;
    for (double x : d) mean += x;
    mean /= d.size();
    std::vector<double> means;
    unsigned long long rng = 0x9E3779B97F4A7C15ull;
    for (int r = 0; r < 2000; ++r) {
        double sum = 0;
        for (size_t k = 0; k < d.size(); ++k) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            sum += d[(rng >> 33) % d.size()];
        }
        means.push_back(sum / d.size());
    }
    std::sort(means.begin(), means.end());
    lo = means[size_t(0.025 * (means.size() - 1))];
    hi = means[size_t(0.975 * (means.size() - 1))];
}

void WritePaired(FILE* f, double cyclesPerMs) {
    std::vector<Block> blocks = Blocks(cyclesPerMs);
    std::vector<double> dMs, dCpu;
    for (size_t i = 0; i + 3 < blocks.size(); ++i) {
        const Block* q = &blocks[i];
        if ((q[0].phase - 1) % 4 != 0 || q[3].phase != q[0].phase + 3) continue;
        if (!q[0].on || q[1].on || q[2].on || !q[3].on) continue;
        dMs.push_back(0.5 * (std::log(q[0].ms) + std::log(q[3].ms) - std::log(q[1].ms) - std::log(q[2].ms)));
        dCpu.push_back(0.5 * (std::log(q[0].cpu) + std::log(q[3].cpu) - std::log(q[1].cpu) - std::log(q[2].cpu)));
    }
    double m, lo, hi, cm, clo, chi;
    Interval(dMs, m, lo, hi);
    Interval(dCpu, cm, clo, chi);
    auto pct = [](double x) { return 100.0 * (std::exp(x) - 1.0); };
    fprintf(f, "# paired (%zu ABBA quartets): frame time %+.1f%% [%+.1f, %+.1f], "
               "game-thread CPU %+.1f%% [%+.1f, %+.1f] (95%% CI)\n",
            dMs.size(), pct(m), pct(lo), pct(hi), pct(cm), pct(clo), pct(chi));
    fprintf(f, "# blocks: phase state frames frame_ms_p50 cpu_ms_p50\n");
    for (const Block& b : blocks)
        fprintf(f, "# block %u %s %zu %.2f %.2f\n", b.phase, b.on ? "on" : "off", b.frames, b.ms, b.cpu);
}

void WriteReport() {
    // Thread cycle counts use the TSC reference clock; calibrate it against QPC over the run.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double seconds = double(now.QuadPart - g_startQpc.QuadPart) / freq.QuadPart;
    double cyclesPerMs = double(__rdtsc() - g_startTsc) / (seconds * 1000.0);

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-ab.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "# feature=%s period=%d settle=%d seconds=%.1f\n", g_feature.c_str(), g_period, kSettleFrames, seconds);
    // jitter: median |frame[i] - frame[i-1]| within a phase; uneven pacing shows up here
    // even when median and p95 look fine.
    fprintf(f, "state\tframes\tframe_ms_p50\tframe_ms_p95\tfps_p50\tgame_thread_cpu_ms_p50\tjitter_ms_p50\n");
    float p50[2] = {}, cpu50[2] = {};
    for (int s = 0; s < 2; ++s) {
        std::vector<float> ms, cpu, jitter;
        for (size_t i = 0; i < g_samples[s].size(); ++i) {
            const Sample& x = g_samples[s][i];
            ms.push_back(x.frameMs);
            cpu.push_back(float(x.cpuCycles / cyclesPerMs));
            if (i > 0 && x.phase == g_samples[s][i - 1].phase)
                jitter.push_back(std::fabs(x.frameMs - g_samples[s][i - 1].frameMs));
        }
        p50[s] = Percentile(ms, 0.5);
        float p95 = Percentile(ms, 0.95);
        cpu50[s] = Percentile(cpu, 0.5);
        fprintf(f, "%s\t%zu\t%.2f\t%.2f\t%.1f\t%.2f\t%.2f\n", s ? "on" : "off", g_samples[s].size(), p50[s], p95,
                p50[s] > 0 ? 1000.0 / p50[s] : 0.0, cpu50[s], Percentile(jitter, 0.5));
    }
    if (p50[0] > 0 && p50[1] > 0)
        fprintf(f, "# on vs off: frame time %+.1f%%, fps %+.1f%%, game-thread CPU %+.1f%%\n",
                100.0 * (p50[1] - p50[0]) / p50[0], 100.0 * (p50[0] / p50[1] - 1.0),
                cpu50[0] > 0 ? 100.0 * (cpu50[1] - cpu50[0]) / cpu50[0] : 0.0);
    WritePaired(f, cyclesPerMs);
    for (int s = 0; s < 2; ++s)
        fprintf(f, "# stutter %s: frames > %.0f ms: %u steady + %u right after a switch; worst %.1f / %.1f ms\n",
                s ? "on " : "off", kHitchMs, g_hitches[s][0], g_hitches[s][1], g_worst[s][0], g_worst[s][1]);
    fclose(f);
    Log("ab: report written (%zu off / %zu on frames)", g_samples[0].size(), g_samples[1].size());
}

// Bench names the feature for the next window in tscfix-ab-select.txt (game folder); without
// it the first registered feature is measured. Statistics start fresh for every window.
void SelectRequested() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-ab-select.txt");
    FILE* f = nullptr;
    if (!fopen_s(&f, path, "r") && f) {
        char name[64] = {};
        if (fgets(name, sizeof name, f)) {
            name[strcspn(name, "\r\n ")] = 0;
            for (const Feature& x : g_features)
                if (x.name == name) {
                    if (g_toggle && g_toggle != x.toggle) g_toggle(false);
                    g_feature = x.name;
                    g_toggle = x.toggle;
                }
        }
        fclose(f);
        DeleteFileA(path);
    }
    for (auto& v : g_samples) v.clear();
    for (auto& h : g_hitches) h[0] = h[1] = 0;
    for (auto& w : g_worst) w[0] = w[1] = 0;
    g_phase = 0;
    Log("ab: window starts for '%s'", g_feature.c_str());
}

void SetState(bool on) {
    g_on = on;
    g_framesInPhase = 0;
    ++g_phase;
    if (g_toggle) g_toggle(on);
    std::string s = "A/B " + g_feature + (g_finished ? ": done" : on ? ": ON phase" : ": OFF phase");
    status::Set("ab", s.c_str());
}

}  // namespace

void Configure(const char* feature, int periodFrames, Toggle toggle) {
    if (!feature || !*feature || !toggle) return;
    g_features.push_back({feature, toggle});
    if (g_features.size() > 1) {
        Log("ab: also installed '%s'", feature);
        return;
    }
    g_feature = feature;
    g_period = periodFrames > 20 ? periodFrames : 150;
    g_toggle = toggle;
    DWORD pid = GetCurrentProcessId();
    wchar_t name[96];
    swprintf_s(name, L"Local\\tscfix.ab.start.%lu", pid);
    g_evStart = CreateEventW(nullptr, TRUE, FALSE, name);
    swprintf_s(name, L"Local\\tscfix.ab.stop.%lu", pid);
    g_evStop = CreateEventW(nullptr, TRUE, FALSE, name);
    swprintf_s(name, L"Local\\tscfix.ab.done.%lu", pid);
    g_evDone = CreateEventW(nullptr, TRUE, FALSE, name);
    for (auto& v : g_samples) v.reserve(8192);
    Log("ab: feature '%s', switching every %d frames", g_feature.c_str(), g_period);
}

void OnFrame() {
    if (!g_toggle) return;
    if (g_finished) {  // ready for another window in the same session
        g_finished = false;
        g_running = false;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    ULONG64 cycles = ThreadCycles();

    if (!g_running) {
        if (WaitForSingleObject(g_evStart, 0) != WAIT_OBJECT_0) return;
        ResetEvent(g_evStart);
        ResetEvent(g_evDone);
        SelectRequested();
        g_running = true;
        g_startQpc = qpc;
        g_startTsc = __rdtsc();
        SetState(true);
    } else {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        float frameMs = float(double(qpc.QuadPart - g_lastQpc.QuadPart) * 1000.0 / freq.QuadPart);
        bool settle = ++g_framesInPhase <= kSettleFrames;
        if (frameMs > kHitchMs) ++g_hitches[g_on][settle];
        if (frameMs > g_worst[g_on][settle]) g_worst[g_on][settle] = frameMs;
        if (!settle) g_samples[g_on].push_back({frameMs, float(cycles - g_lastCycles), g_phase});
        if (WaitForSingleObject(g_evStop, 0) == WAIT_OBJECT_0) {
            ResetEvent(g_evStop);
            g_finished = true;
            SetState(false);
            WriteReport();
            SetEvent(g_evDone);
            return;
        }
        if (g_framesInPhase >= g_period) SetState(kPattern[g_phase % 4]);  // next phase continues ABBA
    }
    g_lastQpc = qpc;
    g_lastCycles = cycles;
}

}  // namespace tscfix::ab
