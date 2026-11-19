#include "binder.h"

#include <MinHook.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "log.h"
#include "engine_sites.h"
#if __has_include("research_binder_table.h")
#include "research_binder_table.h"
#endif
#include "ripscan.h"

extern "C" ULONG _tls_index;

namespace tscfix::binder {
namespace {

uintptr_t kBinder = 0;  // resolved at install (engine_sites.h)
constexpr size_t kBinderSize = 0x19f0;  // the draw binder object
// Every function that receives the global binder (or a sub-object of it) in rcx.
uintptr_t kMethods[33] = {};  // resolved at install

struct MethodDetour {
    uintptr_t rva;
    void* detour;
    void** original;
};
MethodDetour g_detours[4];
int g_detourCount = 0;

uintptr_t g_base = 0;
uint8_t* g_global = nullptr;
uint8_t* g_copy = nullptr;
volatile bool g_substitute = false;
// Thread id of the thread in the shadow role while substituting (0 = none). Lives in the
// block near the engine so the instruction thunks can reach it RIP-relative.
volatile DWORD g_noTid = 0;  // until installed
volatile DWORD* g_shadowTidVar = &g_noTid;
uint8_t* g_near = nullptr;  // tid, binder copy, instruction thunks (within rel32 of the engine)
constexpr size_t kNearSize = 0x20000;
constexpr size_t kCopyOffset = 0x40;
constexpr size_t kThunkOffset = (kCopyOffset + kBinderSize + 0xff) & ~size_t{0xff};
DWORD g_roleTid = 0;  // shadow role, whether or not substituting

// Per-thread binder offset (copy - global) in this module's static TLS block, read by the stubs
// and thunks through TEB.ThreadLocalStoragePointer: zero for every thread that never set it, so
// only threads given a copy are redirected. Every thread has the block (we are loaded at process
// start).
thread_local uintptr_t t_offset = 0;
uint32_t g_tlsIndexDisp = 0, g_tlsVarDisp = 0;  // [gs:0x58] + index*8 -> block; block + var -> t_offset

void InitTlsDisplacements() {
    t_offset = 0;  // touch it so the block exists for this thread
    auto* blocks = *reinterpret_cast<uint8_t***>(__readgsqword(0x30) + 0x58);  // TEB.ThreadLocalStoragePointer
    g_tlsIndexDisp = _tls_index * 8;
    g_tlsVarDisp = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(&t_offset) - blocks[_tls_index]);
}

// Entry stub: if the current thread has a copy and rcx points into the global binder, move rcx
// to the same offset in the copy; then continue at the original function. Uses only rax, r10
// and r11 (volatile, not argument registers).
size_t EmitStub(uint8_t* p, void* trampoline) {
    uint8_t* s = p;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
    auto put64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    auto put32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
    put({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00});  // mov rax, gs:[0x58]
    put({0x48, 0x8B, 0x80});                                      // mov rax, [rax + index*8]
    put32(g_tlsIndexDisp);
    put({0x48, 0x8B, 0x80});                                      // mov rax, [rax + var] (t_offset)
    put32(g_tlsVarDisp);
    put({0x48, 0x85, 0xC0});                                      // test rax, rax
    put({0x74, 0x00});                                            // jz orig
    uint8_t* jz = p - 1;
    put({0x49, 0xBA});                                            // mov r10, global
    put64(reinterpret_cast<uint64_t>(g_global));
    put({0x49, 0x89, 0xCB});                                      // mov r11, rcx
    put({0x4D, 0x29, 0xD3});                                      // sub r11, r10
    put({0x49, 0x81, 0xFB});                                      // cmp r11, size
    put32(static_cast<uint32_t>(kBinderSize));
    put({0x73, 0x00});                                            // jae orig
    uint8_t* jae = p - 1;
    put({0x48, 0x01, 0xC1});                                      // add rcx, rax
    uint8_t* orig = p;
    *jz = static_cast<uint8_t>(orig - (jz + 1));
    *jae = static_cast<uint8_t>(orig - (jae + 1));
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                    // jmp [rip]
    put64(reinterpret_cast<uint64_t>(trampoline));
    return static_cast<size_t>(p - s);
}

uint8_t* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = target & ~(gran - 1);
    for (uintptr_t delta = gran; delta < 0x70000000; delta += gran)
        for (uintptr_t c : {start - delta, start + delta})
            if (void* q = VirtualAlloc(reinterpret_cast<void*>(c), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(q);
    return nullptr;
}

int32_t Rel32(const uint8_t* from_end, uintptr_t to) {
    return static_cast<int32_t>(static_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from_end));
}


// Patched bytes per instruction site, kept so the sites can be restored and patched again.
// Instructions that address the binder RIP-relative, found at start-up (ripscan), with their
// original and patched bytes (both taken from / made for this process's own copy of the engine).
struct PatchSite {
    uintptr_t rva;
    uint8_t length;
    uintptr_t target;
    uint8_t scratch, base, newLength;
    uint8_t original[15];
    uint8_t reencoded[15];
    uint8_t patch[15];
};
std::vector<PatchSite> g_sites;
void* g_methodTargets[64];
int g_methodTargetCount = 0;
bool g_patched = true;
uintptr_t g_thunkBegin = 0, g_thunkEnd = 0;  // a thread in here returns into a site's tail

// Each instruction that addresses the binder by absolute address becomes a call to a thunk:
//   push A; push B; mov A, gs:[0x58]; mov A, [A + index*8]; mov A, [A + var]; mov B, target; lea B, [B + A];
//   <the instruction re-encoded to address [B]>; pop B; pop A; ret
// Nothing in it touches the flags, so no flag save / restore is needed.
// (measurement) [experiments] binder_count_sites=1: every thunk counts its executions (all
// threads) in a table near the engine; a logger thread reports the busiest sites every 10 s.
bool g_countSites = false;
char g_ini[MAX_PATH] = "";
constexpr size_t kCounterOffset = 0x18000;
volatile LONG64* g_siteHits = nullptr;
DWORD WINAPI SiteHitLogger(void*) {
    std::vector<LONG64> last(g_sites.size(), 0);
    for (;;) {
        Sleep(10000);
        std::vector<std::pair<LONG64, size_t>> d;
        LONG64 total = 0;
        for (size_t i = 0; i < g_sites.size(); ++i) {
            LONG64 now = g_siteHits[i];
            d.push_back({now - last[i], i});
            total += now - last[i];
            last[i] = now;
        }
        std::sort(d.begin(), d.end(), [](auto& a, auto& b) { return a.first > b.first; });
        char line[1024] = "";
        for (size_t k = 0; k < 16 && k < d.size() && d[k].first; ++k) {
            size_t len = strlen(line);
            sprintf_s(line + len, sizeof line - len, " %llx:%lld", static_cast<unsigned long long>(g_sites[d[k].second].rva),
                      d[k].first / 10);
        }
        Log("binder: site executions %lld/s; busiest (rva:per s):%s", total / 10, line);
        static int reports = 0;
        if (++reports == 9) {  // once, in the drive: every site that ran (rva:executions in the 10 s)
            std::string all;
            for (auto& x : d)
                if (x.first) {
                    char e[32];
                    sprintf_s(e, " %llx:%lld", static_cast<unsigned long long>(g_sites[x.second].rva), x.first);
                    all += e;
                }
            Log("binder: all sites:%s", all.c_str());
        }
    }
}

bool PatchSites(uint8_t*& p) {
    if (g_sites.empty()) return false;
    const uint32_t indexDisp = g_tlsIndexDisp, varDisp = g_tlsVarDisp;
    if (g_countSites) g_siteHits = reinterpret_cast<volatile LONG64*>(g_near + kCounterOffset);
    for (PatchSite& s : g_sites) {
        uint8_t* thunk = p;
        auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
        if (g_siteHits) {  // lock inc qword [rip + counter]
            put({0xF0, 0x48, 0xFF, 0x05});
            int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(g_siteHits + (&s - g_sites.data())) -
                                               reinterpret_cast<intptr_t>(p + 4));
            memcpy(p, &rel, 4);
            p += 4;
        }
        const uint8_t A = s.scratch, B = s.base;  // A in r8..r15, B in rax..rdi (not rsp / rbp)
        put({0x41, static_cast<uint8_t>(0x50 + (A - 8))});                      // push A
        put({static_cast<uint8_t>(0x50 + B)});                                  // push B
        put({0x65, 0x4C, 0x8B, static_cast<uint8_t>(0x04 | ((A & 7) << 3)), 0x25, 0x58, 0, 0, 0});  // mov A, gs:[0x58]
        for (uint32_t disp : {indexDisp, varDisp}) {                            // mov A, [A + disp32]
            put({0x4D, 0x8B, static_cast<uint8_t>(0x80 | ((A & 7) << 3) | (A & 7))});
            if ((A & 7) == 4) put({0x24});
            memcpy(p, &disp, 4);
            p += 4;
        }
        put({0x48, static_cast<uint8_t>(0xB8 + B)});                            // mov B, imm64
        uint64_t target = s.target;
        memcpy(p, &target, 8);
        p += 8;
        put({0x4A, 0x8D, static_cast<uint8_t>((B << 3) | 4),
             static_cast<uint8_t>(((A & 7) << 3) | B)});                        // lea B, [B + A]
        memcpy(p, s.reencoded, s.newLength);
        p += s.newLength;
        put({static_cast<uint8_t>(0x58 + B)});                                  // pop B
        put({0x41, static_cast<uint8_t>(0x58 + (A - 8)), 0xC3});                // pop A; ret
        p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 15) & ~uintptr_t{15});

        auto* site = reinterpret_cast<uint8_t*>(g_base + s.rva);
        uint8_t* patch = s.patch;
        patch[0] = 0xE8;
        int32_t rel = Rel32(site + 5, reinterpret_cast<uintptr_t>(thunk));
        memcpy(patch + 1, &rel, 4);
        memset(patch + 5, 0x90, s.length - 5);
        DWORD old;
        VirtualProtect(site, s.length, PAGE_EXECUTE_READWRITE, &old);
        memcpy(site, patch, s.length);
        VirtualProtect(site, s.length, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    if (g_siteHits)
        if (HANDLE t = CreateThread(nullptr, 0, &SiteHitLogger, nullptr, 0, nullptr)) CloseHandle(t);
    return true;
}

void SnapshotCopy(uint8_t* copy) {
    memcpy(copy, g_global, kBinderSize);
    // Pointers into the binder itself (stack tops, sub-objects) follow it into the copy.
    auto lo = reinterpret_cast<uintptr_t>(g_global), hi = lo + kBinderSize;
    size_t moved = 0;
    for (size_t off = 0; off + 8 <= kBinderSize; off += 8) {
        uintptr_t v;
        memcpy(&v, copy + off, 8);
        if (v >= lo && v < hi) {
            v = v - lo + reinterpret_cast<uintptr_t>(copy);
            memcpy(copy + off, &v, 8);
            ++moved;
        }
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("binder: first snapshot, %zu inner pointer(s) moved", moved);
    }
}

}  // namespace

uintptr_t Rva(uintptr_t rva) { return g_base + rva; }

uintptr_t Translate(const uint8_t* copy, uintptr_t p) {
    auto lo = reinterpret_cast<uintptr_t>(g_global);
    if (!copy || !lo || p < lo || p >= lo + kBinderSize) return p;
    return p - lo + reinterpret_cast<uintptr_t>(copy);
}

bool RegisterMethodDetour(uintptr_t rva, void* detour, void** original) {
    if (g_detourCount == 4) return false;
    g_detours[g_detourCount++] = {rva, detour, original};
    return true;
}
uint8_t* Global() { return g_global; }

bool Install(HMODULE engine) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(engine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(engine) + dos->e_lfanew);
    (void)nt;
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_countSites = GetPrivateProfileIntA("experiments", "binder_count_sites", 0, ini) != 0;
        strcpy_s(g_ini, ini);
    }
    size_t found = 0;
    const uintptr_t* resolved = engine::Resolved(engine, &found);
    if (found != engine::kSiteCount) {
        Log("binder: %zu of %d engine sites found; disabled", found, int(engine::kSiteCount));
        return false;
    }
    kBinder = resolved[engine::BinderGlobal];
    for (int i = 0; i < 33; ++i) kMethods[i] = resolved[engine::BinderMethod0 + i];
    g_base = reinterpret_cast<uintptr_t>(engine);
    g_global = reinterpret_cast<uint8_t*>(g_base + kBinder);
    {  // Instructions that address the binder RIP-relative, found in the engine as loaded.
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        size_t skipped = 0;
        auto refs = ripscan::Find(engine, g_base + kBinder, g_base + kBinder + kBinderSize, &skipped);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        Log("binder: %zu instruction sites address the binder (%zu not redirectable), found in %.1f ms", refs.size(),
            skipped, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        g_sites.clear();
#if __has_include("research_binder_table.h")
        bool tableOnly = GetPrivateProfileIntA("experiments", "binder_table_only", 0, g_ini) != 0;
#else
        bool tableOnly = false;
#endif
        for (const auto& x : refs) {
#if __has_include("research_binder_table.h")
            if (tableOnly && std::find(std::begin(kOldBinderSites), std::end(kOldBinderSites), x.at - g_base) ==
                                 std::end(kOldBinderSites))
                continue;
#endif
            PatchSite ps = {};
            ps.rva = x.at - g_base;
            ps.length = x.length;
            ps.target = x.target;
            ps.scratch = x.scratch, ps.base = x.base, ps.newLength = x.newLength;
            memcpy(ps.original, reinterpret_cast<const void*>(x.at), x.length);
            memcpy(ps.reencoded, x.reencoded, x.newLength);
            g_sites.push_back(ps);
        }
    }
    g_near = AllocNear(g_base, kNearSize);
    if (!g_near) return false;
    g_shadowTidVar = reinterpret_cast<volatile DWORD*>(g_near);
    g_copy = g_near + kCopyOffset;
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_copy || !stubs) return false;
    InitTlsDisplacements();
    MH_Initialize();
    uint8_t* p = stubs;
    int ok = 0;
    for (uintptr_t rva : kMethods) {
        void* target = reinterpret_cast<void*>(g_base + rva);
        void* trampoline = nullptr;
        // Create with a placeholder detour first to learn the trampoline, then point it at the stub.
        if (MH_CreateHook(target, p, &trampoline) != MH_OK) {
            Log("binder: cannot hook %llx", static_cast<unsigned long long>(rva));
            continue;
        }
        // A registered C++ detour runs after the rcx swap and calls the original itself.
        void* next = trampoline;
        for (MethodDetour& d : g_detours)
            if (d.rva == rva && d.detour) {
                *d.original = trampoline;
                next = d.detour;
            }
        size_t n = EmitStub(p, next);
        if (g_methodTargetCount < 64) g_methodTargets[g_methodTargetCount++] = target;
        if (MH_EnableHook(target) != MH_OK) {
            Log("binder: cannot enable %llx", static_cast<unsigned long long>(rva));
            continue;
        }
        p += (n + 15) & ~size_t{15};
        ++ok;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, static_cast<size_t>(p - stubs));
    uint8_t* t = g_near + kThunkOffset;
    g_thunkBegin = reinterpret_cast<uintptr_t>(t);
    bool sites = PatchSites(t);
    g_thunkEnd = reinterpret_cast<uintptr_t>(t);
    Log("binder: %d of %zu binder methods redirectable, %zu instruction sites %s (%zu bytes of thunks)", ok,
        std::size(kMethods), g_sites.size(), sites ? "patched" : "NOT patched",
        static_cast<size_t>(t - (g_near + kThunkOffset)));
    return sites && ok == static_cast<int>(std::size(kMethods));
}

void SetSubstitute(bool on) { g_substitute = on; }
bool Substituting() {
    DWORD tid = *g_shadowTidVar;
    return tid != 0 && tid == GetCurrentThreadId();
}

void EnterShadow() {
    g_roleTid = GetCurrentThreadId();
    if (!g_substitute || !g_copy) return;
    SnapshotCopy(g_copy);
    *g_shadowTidVar = g_roleTid;
}

void EnterShadowFor(DWORD tid) {
    if (!g_substitute || !g_copy) return;
    SnapshotCopy(g_copy);
    *g_shadowTidVar = tid;
}

void ActivateThread() {
    t_offset = Substituting() ? reinterpret_cast<uintptr_t>(g_copy) - reinterpret_cast<uintptr_t>(g_global) : 0;
}

void LeaveShadow() {
    t_offset = 0;
    *g_shadowTidVar = 0;
    g_roleTid = 0;
}

uint8_t* Copy() { return g_copy; }

uint8_t* Binder() { return g_global + t_offset; }

// Every other thread of the process, suspended for the lifetime of this object.
struct ThreadFreeze {
    HANDLE threads[512];
    int count = 0;
    ThreadFreeze() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{sizeof te};
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        for (BOOL more = Thread32First(snap, &te); more && count < 512; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) {
                CloseHandle(h);
                continue;
            }
            threads[count++] = h;
        }
        CloseHandle(snap);
    }
    ~ThreadFreeze() {
        for (int i = 0; i < count; ++i) ResumeThread(threads[i]), CloseHandle(threads[i]);
    }
    // No suspended thread is in the middle of a patched range (it may be at its first byte).
    bool Clear() const {
        for (int i = 0; i < count; ++i) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(threads[i], &c)) return false;
            if (c.Rip >= g_thunkBegin && c.Rip < g_thunkEnd) return false;
            for (const PatchSite& s : g_sites) {
                uintptr_t site = g_base + s.rva;
                if (c.Rip > site && c.Rip < site + s.length) return false;
            }
        }
        return true;
    }
};

bool SetPatched(bool on) {
    if (on == g_patched || !g_base) return true;
    {
        ThreadFreeze freeze;
        if (!freeze.Clear()) return false;  // try again at a later frame
        for (const PatchSite& s : g_sites) {
            auto* site = reinterpret_cast<uint8_t*>(g_base + s.rva);
            DWORD old;
            VirtualProtect(site, s.length, PAGE_EXECUTE_READWRITE, &old);
            memcpy(site, on ? s.patch : s.original, s.length);
            VirtualProtect(site, s.length, old, &old);
        }
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    }
    for (int i = 0; i < g_methodTargetCount; ++i)
        on ? MH_QueueEnableHook(g_methodTargets[i]) : MH_QueueDisableHook(g_methodTargets[i]);
    MH_ApplyQueued();
    g_patched = on;
    return true;
}

uint8_t* NewCopy() {
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, kBinderSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}
void SnapshotInto(uint8_t* copy) {
    if (copy) SnapshotCopy(copy);
}
void SnapshotFromCurrent(uint8_t* copy) {
    uint8_t* src = Binder();
    if (!copy) return;
    if (src == g_global) return SnapshotCopy(copy);
    // From another copy: its inner pointers point into it; move them into the new one.
    memcpy(copy, src, kBinderSize);
    auto lo = reinterpret_cast<uintptr_t>(src), hi = lo + kBinderSize;
    for (size_t off = 0; off + 8 <= kBinderSize; off += 8) {
        uintptr_t v;
        memcpy(&v, copy + off, 8);
        if (v >= lo && v < hi) {
            v = v - lo + reinterpret_cast<uintptr_t>(copy);
            memcpy(copy + off, &v, 8);
        }
    }
}
void UseCopy(uint8_t* copy) {
    t_offset = copy ? reinterpret_cast<uintptr_t>(copy) - reinterpret_cast<uintptr_t>(g_global) : 0;
}

}  // namespace tscfix::binder
