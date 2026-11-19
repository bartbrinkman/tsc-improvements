#include "physicsfix.h"

#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <tlhelp32.h>

#include "log.h"
#include "sites.h"

namespace tscfix::physicsfix {
namespace {

// Game version v79.0a: register (manager, collideable), unregister (manager, collideable)
// and the manager's destructor, which frees the whole list.
constexpr sites::Fingerprint kRegister = {0x410be8980bb97421ull, 24};
constexpr sites::Fingerprint kUnregister = {0x321e89a0ac493d0aull, 16};
constexpr sites::Fingerprint kDestroy = {0x265bcfed432f83b0ull, 24};

struct Node { Node* next; Node* prev; void* value; };  // list node layout (next, prev, value)

using PairFn = void (*)(void* manager, void* collideable);
using DestroyFn = void* (*)(void* manager, unsigned flags);
PairFn g_register = nullptr, g_unregister = nullptr;
DestroyFn g_destroy = nullptr;

int g_listField = 0;
volatile bool g_active = false;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<void*, Node*> g_index;  // collideable -> its (most recently registered) node
void* g_manager = nullptr;

uint64_t g_removals = 0, g_moved = 0;
int64_t g_ticks = 0, g_worstFrameTicks = 0, g_frameTicks = 0;

Node* Head(void* manager) { return *reinterpret_cast<Node**>(static_cast<uint8_t*>(manager) + g_listField); }

void Register(void* manager, void* collideable) {
    g_register(manager, collideable);
    AcquireSRWLockExclusive(&g_lock);
    if (manager != g_manager) {
        g_index.clear();
        g_manager = manager;
    }
    Node* tail = Head(manager)->prev;  // registration appends at the tail
    if (tail && tail->value == collideable) g_index[collideable] = tail;
    ReleaseSRWLockExclusive(&g_lock);
}

void Unregister(void* manager, void* collideable) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    AcquireSRWLockExclusive(&g_lock);
    if (manager == g_manager) {
        auto it = g_index.find(collideable);
        if (it != g_index.end()) {
            Node* n = it->second;
            Node* head = Head(manager);
            g_index.erase(it);
            // Only a node that is still linked and still holds this collideable is moved.
            if (g_active && n != head && n->value == collideable && n->next->prev == n && n->prev->next == n &&
                head->next != n) {
                n->prev->next = n->next;
                n->next->prev = n->prev;
                n->next = head->next;
                n->prev = head;
                head->next->prev = n;
                head->next = n;
                ++g_moved;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_unregister(manager, collideable);  // the engine's search now ends at the first node
    QueryPerformanceCounter(&b);
    g_ticks += b.QuadPart - a.QuadPart;
    g_frameTicks += b.QuadPart - a.QuadPart;
    ++g_removals;
}

void* Destroy(void* manager, unsigned flags) {
    AcquireSRWLockExclusive(&g_lock);
    if (manager == g_manager) {
        g_index.clear();
        g_manager = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_destroy(manager, flags);
}

template <typename Fn>
bool Hook(HMODULE engine, const sites::Fingerprint& fp, Fn detour, Fn* original) {
    void* target = sites::Find(engine, fp);
    return target && MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

// The removal function loads the list head first thing (mov rax, [rcx+disp8] after its stack
// setup), so the offset can be read from the code instead of being configured.
int ListFieldFromCode(HMODULE engine) {
    auto* p = static_cast<const uint8_t*>(sites::Find(engine, kUnregister));
    if (!p || p[0] != 0x48 || p[1] != 0x83 || p[2] != 0xEC) return 0;  // sub rsp, imm8
    if (p[4] != 0x48 || p[5] != 0x8B || p[6] != 0x41) return 0;        // mov rax, [rcx+disp8]
    return p[7];
}

}  // namespace

bool Install(HMODULE engine, int listField) {
    if (listField <= 0) listField = ListFieldFromCode(engine);
    if (listField <= 0) {
        Log("physicsfix: list offset not found; disabled");
        return false;
    }
    g_listField = listField;
    g_index.reserve(1 << 16);
    MH_Initialize();
    // Destroy first, so the index can never outlive a manager it describes.
    bool ok = Hook(engine, kDestroy, &Destroy, &g_destroy) && Hook(engine, kRegister, &Register, &g_register) &&
              Hook(engine, kUnregister, &Unregister, &g_unregister);
    Log("physicsfix: %s", ok ? "installed" : "not installed (functions not found)");
    return ok;
}

void SetActive(bool active) { g_active = active; }

void OnFrame() {
    if (!g_unregister) return;
    if (g_frameTicks > g_worstFrameTicks) g_worstFrameTicks = g_frameTicks;
    g_frameTicks = 0;
    static unsigned frames = 0;
    if (++frames % 300 != 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (g_removals)
        Log("physicsfix: last 300 frames: %llu removals (%llu moved to front), %.2f ms total, worst frame %.2f ms%s",
            static_cast<unsigned long long>(g_removals), static_cast<unsigned long long>(g_moved),
            g_ticks * 1000.0 / f.QuadPart, g_worstFrameTicks * 1000.0 / f.QuadPart, g_active ? "" : " [index only]");
    g_removals = g_moved = 0;
    g_ticks = g_worstFrameTicks = 0;
}

// The cab camera hangs on a spring with damping (cab camera blueprint: SpringX/Y/ZCoefficient,
// DampingX/Y/ZCoefficient), registered with the physics when the camera becomes active. The
// standing train's speed is never exactly 0 (0.001 to 0.007 km/h of noise at Haarlem), and that
// noise keeps the lightly damped spring ringing: the picture turns +-0.1 pixel with a period of
// about 10 frames, which moves thin far objects and flips surfaces that sit close together.
// [graphics] cab_camera_spring_percent / cab_camera_damping_percent scale the blueprint's values
// at registration; cab_camera_spring=0 (default) registers no spring (the camera stays fixed to the
// cab); cab_camera_spring=1 keeps the game's spring.
namespace {
constexpr sites::Fingerprint kCabHeadSpring = {0x4345bf857b65c2baull, 16};  // (camera): registers the spring
constexpr size_t kCameraBlueprint = 0xa0, kBlueprintSpring = 0xc;              // 3 spring then 3 damping floats
using HeadSpringFn = void (*)(void* camera);
HeadSpringFn g_headSpring = nullptr;
int g_springPercent = 100, g_dampingPercent = 100;
bool g_spring = true;

// (diagnosis) [experiments] camera_watch=1: 15 s after the cab camera's activation, a helper thread
// looks for matrices in the game's memory with a row or column like one of the camera's axes
// (kAxes, read from the view matrix at the test view), samples them every 20 ms for 4 s, logs the
// ones that move, and puts hardware write breakpoints on up to four of them for 3 s: the engine
// code that writes the moving transform (where the standing train's rocking comes from).
bool g_watch = false;
int g_watchSkip = 0, g_watchWorld = 0;  // camera_watch_skip: leave out the N rarest families (they were looked at in an earlier run)
void* volatile g_camera = nullptr;
uintptr_t g_engineBase = 0;
// The camera's axes at the Haarlem test view (rows and columns of the view matrix's rotation).
const float kAxes[][3] = {{0.145f, -0.00108f, -0.98943f}, {0.98941f, -0.00677f, 0.145f}, {0.145f, 0.00686f, 0.98941f}, {-0.98943f, -0.0000885f, 0.145f}};
constexpr int kAxisCount = 4;
bool SafeRead(const void* from, void* to, size_t n) {
    __try {
        memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct BpHit { volatile LONG64 key; volatile LONG count; uint64_t ret; };
BpHit g_bp[4][32];
LONG CALLBACK BpHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = e->ContextRecord->Dr6;
    e->ContextRecord->Dr6 = 0;
    LONG64 key = static_cast<LONG64>(e->ContextRecord->Rip);
    uint64_t ret = 0;
    SafeRead(reinterpret_cast<const void*>(e->ContextRecord->Rsp), &ret, 8);  // (a leaf copy routine: its caller)
    for (int d = 0; d < 4; ++d) {
        if (!(dr6 & (1ull << d))) continue;
        for (BpHit& h : g_bp[d]) {
            LONG64 k = h.key;
            if (!k) k = InterlockedCompareExchange64(&h.key, key, 0);
            if (!k || k == key) {
                if (InterlockedIncrement(&h.count) == 1) h.ret = ret;
                break;
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
void ArmAll(const uintptr_t* at, int n) {
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
                c.Dr7 = 0;
                DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                for (int d = 0; d < n; ++d) *dr[d] = at[d], c.Dr7 |= (1ull << (2 * d)) | (1ull << (16 + 4 * d)) | (3ull << (18 + 4 * d));  // write, 4 bytes
                if (SetThreadContext(t, &c)) ++armed;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("camera watch: %d breakpoint(s) %s on %d thread(s)", n, n ? "armed" : "cleared", armed);
}
struct Match { uintptr_t at; int stride, axis; float lo[3], hi[3]; };
constexpr int kMaxMatches = 4096;
Match g_found[kMaxMatches];
int g_nfound = 0;
bool Near(float v, float want, float tol) { return v > want - tol && v < want + tol; }
void ScanRegion(const float* f, size_t n) {
    __try {
        for (size_t k = 0; k + 8 < n && g_nfound < kMaxMatches; ++k)
            for (int a = 0; a < kAxisCount; ++a) {
                if (!Near(f[k], kAxes[a][0], 0.003f)) continue;
                for (int stride = 1; stride <= 4; stride += 3)
                    if (k + 2 * stride < n && Near(f[k + stride], kAxes[a][1], 0.003f) && Near(f[k + 2 * stride], kAxes[a][2], 0.003f) && g_nfound < kMaxMatches)
                        g_found[g_nfound++] = {reinterpret_cast<uintptr_t>(f + k), stride, a, {}, {}};
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
DWORD WINAPI CameraWatch(void*) {
    Sleep(15000);
    MEMORY_BASIC_INFORMATION mbi;
    size_t scanned = 0;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && g_nfound < kMaxMatches; p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE || mbi.RegionSize > (512u << 20)) continue;
        scanned += mbi.RegionSize;
        ScanRegion(static_cast<const float*>(mbi.BaseAddress), mbi.RegionSize / 4);
    }
    Match* found = g_found;
    const int nfound = g_nfound;
    Log("camera watch: %d matches in %zu MB", nfound, scanned >> 20);
    for (int t = 0; t < 200; ++t) {  // 4 s
        for (int q = 0; q < nfound; ++q) {
            Match& m = found[q];
            float v[3];
            const float* f = reinterpret_cast<const float*>(m.at);
            if (!SafeRead(f, &v[0], 4) || !SafeRead(f + m.stride, &v[1], 4) || !SafeRead(f + 2 * m.stride, &v[2], 4)) continue;
            for (int i = 0; i < 3; ++i) {
                if (t == 0 || v[i] < m.lo[i]) m.lo[i] = v[i];
                if (t == 0 || v[i] > m.hi[i]) m.hi[i] = v[i];
            }
        }
        Sleep(20);
    }
    // Moving matches, then one breakpoint per matrix family (same first value), rarest families
    // first (the source of the copies), not on a thread stack (the low addresses here).
    std::vector<int> mov;
    std::unordered_map<int, int> family;
    auto key = [&](const Match& m) { return static_cast<int>(m.lo[0] * 1e6f); };
    int moving = 0;
    for (int q = 0; q < nfound; ++q) {
        const Match& m = found[q];
        float r = 0;
        for (int i = 0; i < 3; ++i) r = m.hi[i] - m.lo[i] > r ? m.hi[i] - m.lo[i] : r;
        if (!(r > 1e-7f && r < 1e-3f)) continue;
        if (++moving <= 60)
            Log("camera watch: %p stride %d: %.7f..%.7f %.7f..%.7f %.7f..%.7f", reinterpret_cast<void*>(m.at), m.stride, m.lo[0], m.hi[0], m.lo[1], m.hi[1],
                m.lo[2], m.hi[2]);
        ++family[key(m)];
        // camera_watch_world=1: only rows of a world transform (the right or forward axis, in a row).
        if (m.at >= 0x200000 && (!g_watchWorld || (m.stride == 1 && m.axis <= 1))) mov.push_back(q);
    }
    std::stable_sort(mov.begin(), mov.end(), [&](int a, int b) { return family[key(found[a])] < family[key(found[b])]; });
    {
        std::vector<std::pair<int, int>> fams(family.begin(), family.end());
        std::sort(fams.begin(), fams.end(), [](auto& a, auto& b) { return a.second < b.second; });
        char line[1024];
        int n = 0;
        for (auto& f : fams) n += n < 900 ? sprintf_s(line + n, sizeof line - n, " %d x%d", f.first, f.second) : 0;
        Log("camera watch: families:%s", line);
    }
    uintptr_t arm[4];
    int armKey[4];
    int narm = 0;
    std::vector<int> order;  // families by rarity
    for (int q : mov)
        if (std::find(order.begin(), order.end(), key(found[q])) == order.end()) order.push_back(key(found[q]));
    order.resize(order.size() < static_cast<size_t>(g_watchSkip) ? 0 : g_watchSkip);  // the families to leave out
    for (int q : mov) {
        const Match& m = found[q];
        if (std::find(order.begin(), order.end(), key(m)) != order.end()) continue;
        bool taken = false;
        for (int d = 0; d < narm; ++d) taken |= armKey[d] == key(m) || (arm[d] >> 12) == (m.at >> 12);
        if (taken || narm == 4) continue;
        int mi = 0;
        for (int i = 1; i < 3; ++i) mi = m.hi[i] - m.lo[i] > m.hi[mi] - m.lo[mi] ? i : mi;
        armKey[narm] = key(m), arm[narm++] = m.at + 4 * m.stride * mi;  // the element that moves most
        Log("camera watch: bp %d on %p (family %d, %d copies, axis %d stride %d)", narm - 1, reinterpret_cast<void*>(arm[narm - 1]), key(m), family[key(m)], m.axis,
            m.stride);
    }
    Log("camera watch: %d moving", moving);
    if (!narm) return 0;
    AddVectoredExceptionHandler(1, &BpHandler);
    ArmAll(arm, narm);
    Sleep(3000);
    ArmAll(arm, 0);
    for (int d = 0; d < narm; ++d)
        for (const BpHit& h : g_bp[d])
            if (h.key) {
                HMODULE mod = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(h.key), &mod))
                    GetModuleFileNameA(mod, name, MAX_PATH);
                const char* base = strrchr(name, 0x5c);
                uintptr_t eb = g_engineBase;
                Log("camera watch: bp %d (%p) written at %s+%llx x%ld (return address %llx, engine rva %llx)", d, reinterpret_cast<void*>(arm[d]),
                    base ? base + 1 : name, static_cast<unsigned long long>(h.key - reinterpret_cast<uintptr_t>(mod)), h.count,
                    static_cast<unsigned long long>(h.ret), static_cast<unsigned long long>(h.ret >= eb && h.ret < eb + 0x2000000 ? h.ret - eb : 0));
            }
    return 0;
}

// (diagnosis) consist_watch=7: the per-body force step (traction minus brake along
// the track, gradient, then addForce) is hooked; every 0.5 s the inputs for each body are logged:
// force state [0] traction, [1] brake, [2] brake reserve, [3] mass, and the body's velocity.
using ForceStepFn = void (*)(int64_t* self, float* state);
ForceStepFn g_forceStep = nullptr;
volatile LONG g_forceLog = 0;
void ForceStep(int64_t* self, float* state) {
    float in[4];
    memcpy(in, state, sizeof in);
    g_forceStep(self, state);
    static DWORD last = 0;
    static int64_t* seen[24];
    static int nseen = 0;
    DWORD now = GetTickCount();
    if (now - last > 500) last = now, nseen = 0;
    for (int i = 0; i < nseen; ++i)
        if (seen[i] == self) return;
    if (nseen >= 24 || InterlockedIncrement(&g_forceLog) > 2000) return;
    seen[nseen++] = self;
    {  // (slot check) the actor's mass, dampings and velocities through the assumed PhysX 2.8 slots
        int64_t* actor = nullptr;
        SafeRead(reinterpret_cast<uint8_t*>(self[1]) + 8, &actor, 8);
        if (actor) {
            void** vt = *reinterpret_cast<void***>(actor);
            using GetF = float (*)(void*);
            using GetV = float* (*)(void*, float*);
            float lv[3] = {}, av[3] = {};
            reinterpret_cast<GetV>(vt[58])(actor, lv);
            reinterpret_cast<GetV>(vt[60])(actor, av);
            Log("consist watch: actor %p mass(46) %.6g lindamp(53) %.6g angdamp(55) %.6g linvel(58) %.6g %.6g %.6g angvel(60) %.6g %.6g %.6g sleeping(?)",
                static_cast<void*>(actor), reinterpret_cast<GetF>(vt[46])(actor), reinterpret_cast<GetF>(vt[53])(actor), reinterpret_cast<GetF>(vt[55])(actor), lv[0], lv[1],
                lv[2], av[0], av[1], av[2]);
        }
    }
    Log("consist watch: force body %p in traction %.6g brake %.6g reserve %.6g mass %.6g | out traction %.6g brake %.6g reserve %.6g dir %.4f %.4f %.4f",
        static_cast<void*>(self), in[0], in[1], in[2], in[3], state[0], state[1], state[2], state[7], state[0xb], state[0xf]);
}

// (diagnosis) [experiments] consist_watch=1: the consist update (this game version
// only) is hooked to learn the physical consists; 15 s later their per-vehicle float array (consist
// +0xd0, 4 floats per entry; the engine checks them for NaN) is sampled for 4 s, the moving values
// are logged, and write breakpoints on the four that move most name the solver code that writes them.
using ConsistUpdateFn = void (*)(void* consist, char flag);
ConsistUpdateFn g_consistUpdate = nullptr;
void* volatile g_consists[16] = {};
bool g_consistWatch = false;
char g_ini[MAX_PATH] = "";
void ConsistUpdate(void* consist, char flag) {
    for (auto& c : g_consists) {
        void* v = c;
        if (v == consist) break;
        if (!v && InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&c), consist, nullptr) == nullptr) break;
    }
    g_consistUpdate(consist, flag);
}
DWORD WINAPI ConsistWatch(void*) {
    Sleep(15000);
    struct Val { uintptr_t at; float lo, hi; };
    std::vector<Val> vals;
    for (void* c : g_consists) {
        if (!c) continue;
        uint64_t arr = 0;
        uint32_t n = 0;
        SafeRead(static_cast<uint8_t*>(c) + 0xd0, &arr, 8);
        SafeRead(static_cast<uint8_t*>(c) + 0xc8, &n, 4);
        Log("consist watch: consist %p count %u array %p", c, n, reinterpret_cast<void*>(arr));
        for (uint32_t i = 0; arr && i < 64 * 4; ++i) vals.push_back({arr + 4 * i, 0, 0});
    }
    for (int t = 0; t < 200; ++t) {
        for (Val& v : vals) {
            float f = 0;
            if (!SafeRead(reinterpret_cast<void*>(v.at), &f, 4)) continue;
            if (t == 0 || f < v.lo) v.lo = f;
            if (t == 0 || f > v.hi) v.hi = f;
        }
        Sleep(20);
    }
    // consist_watch=6: the PhysX actor class's virtual functions (the vtable of the consist's first
    // body) are routed through counting thunks for 3 s: which actor functions the game calls, how
    // often, and the last caller (engine rva) of each.
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, g_ini) == 6) {
        auto* c = static_cast<uint8_t*>(g_consists[0]);
        uint64_t be[2] = {}, actor = 0, vt = 0;
        if (!c || !SafeRead(c + 0x1b0, be, 16) || !SafeRead(reinterpret_cast<void*>(be[0]), &actor, 8) || !SafeRead(reinterpret_cast<void*>(actor), &vt, 8)) return 0;
        constexpr int kSlots = 140;
        static volatile LONG64 counts[kSlots];
        static volatile uint64_t last[kSlots];
        uint64_t orig[kSlots];
        if (!SafeRead(reinterpret_cast<void*>(vt), orig, sizeof orig)) return 0;
        auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSlots * 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        for (int i = 0; i < kSlots; ++i) {
            uint8_t* t = code + i * 64;
            auto put64 = [](uint8_t*& at, uint64_t v) { memcpy(at, &v, 8), at += 8; };
            uint8_t* w = t;
            *w++ = 0x4C, *w++ = 0x8B, *w++ = 0x1C, *w++ = 0x24;  // mov r11, [rsp]
            *w++ = 0x48, *w++ = 0xB8, put64(w, reinterpret_cast<uint64_t>(&last[i]));  // mov rax, &last[i]
            *w++ = 0x4C, *w++ = 0x89, *w++ = 0x18;  // mov [rax], r11
            *w++ = 0x48, *w++ = 0xB8, put64(w, reinterpret_cast<uint64_t>(&counts[i]));  // mov rax, &counts[i]
            *w++ = 0xF0, *w++ = 0x48, *w++ = 0xFF, *w++ = 0x00;  // lock inc qword [rax]
            *w++ = 0x48, *w++ = 0xB8, put64(w, orig[i]);  // mov rax, original
            *w++ = 0xFF, *w++ = 0xE0;  // jmp rax
        }
        DWORD old = 0;
        auto* slots = reinterpret_cast<uint64_t*>(vt);
        if (!VirtualProtect(slots, sizeof orig, PAGE_READWRITE, &old)) return 0;
        for (int i = 0; i < kSlots; ++i) InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&slots[i]), reinterpret_cast<LONG64>(code + i * 64));
        Sleep(3000);
        for (int i = 0; i < kSlots; ++i) InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&slots[i]), static_cast<LONG64>(orig[i]));
        VirtualProtect(slots, sizeof orig, old, &old);
        uintptr_t eb = g_engineBase;
        for (int i = 0; i < kSlots; ++i)
            if (counts[i])
                Log("consist watch: actor slot %d (+0x%x) x%lld, last caller engine rva %llx", i, i * 8, counts[i],
                    static_cast<unsigned long long>(last[i] >= eb && last[i] < eb + 0x2000000 ? last[i] - eb : 0));
        Log("consist watch: moving values done");  // (the thunks stay allocated: a call may still be inside one)
        return 0;
    }
    // consist_watch=5: the cars' positions (consist +0x138: a 64-byte pose per body, position at
    // +0x30) every 10 ms for 6 s, as millimetres from the first sample (x and z), one line per 50 ms.
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, g_ini) == 5) {
        auto* c = static_cast<uint8_t*>(g_consists[0]);
        uint64_t poses = 0;
        if (!c || !SafeRead(c + 0x138, &poses, 8)) return 0;
        float first[12][3] = {};
        char line[1024];
        for (int t = 0; t < 600; ++t) {
            int n = sprintf_s(line, "consist watch: t %4d", t * 10);
            for (int b = 0; b < 12; ++b) {
                float p[3];
                if (!SafeRead(reinterpret_cast<void*>(poses + b * 0x40 + 0x30), p, 12)) break;
                if (t == 0) memcpy(first[b], p, 12);
                n += sprintf_s(line + n, sizeof line - n, " | %6.3f %6.3f", (p[0] - first[b][0]) * 1000, (p[2] - first[b][2]) * 1000);
            }
            if (t % 5 == 0) Log("%s", line);
            Sleep(10);
        }
        Log("consist watch: moving values done");
        return 0;
    }
    // consist_watch=4: the classes in the consist's body list (consist +0x1b0, begin and end) and
    // the vehicle list (+0x1e0): vtable and the functions at +0x48 and +0x1d0 (engine rvas).
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, g_ini) == 4) {
        auto* c = static_cast<uint8_t*>(g_consists[0]);
        static char names[8][MAX_PATH + 32];
        static int nameAt = 0;
        auto rva = [](uint64_t a) -> const char* {
            char* out = names[nameAt++ & 7];
            HMODULE mod = nullptr;
            char path[MAX_PATH] = "?";
            if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &mod))
                GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = strrchr(path, 0x5c);
            sprintf_s(out, MAX_PATH + 32, "%s+%llx", base ? base + 1 : path, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
            return out;
        };
        for (int list : {0x1b0, 0x1e0, 0x1f8}) {
            uint64_t be[2] = {};
            if (!c || !SafeRead(c + list, be, 16)) continue;
            for (uint64_t at = be[0]; at < be[1] && at < be[0] + 8 * 24; at += 8) {
                uint64_t obj = 0, vt = 0, f[0x60] = {}, sub = 0, svt = 0;
                SafeRead(reinterpret_cast<void*>(at), &obj, 8);
                SafeRead(reinterpret_cast<void*>(obj), &vt, 8);
                SafeRead(reinterpret_cast<void*>(vt), f, sizeof f);
                SafeRead(reinterpret_cast<void*>(obj + 8), &sub, 8);
                SafeRead(reinterpret_cast<void*>(sub), &svt, 8);
                Log("consist watch: list +0x%x entry %p vtable %s (+0x48 %s +0x1d0 %s) [+8] %p vtable %s", list, reinterpret_cast<void*>(obj), rva(vt),
                    rva(f[0x48 / 8]), rva(f[0x1d0 / 8]), reinterpret_cast<void*>(sub), rva(svt));
            }
        }
        Log("consist watch: moving values done");
        return 0;
    }
    // consist_watch=3: write breakpoints on consist_watch_bp = "ptr:field,..." (hex: the pointer
    // field of the consist, then the offset in the object it points to; ptr - = the consist itself).
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, g_ini) == 3) {
        char spec[256];
        GetPrivateProfileStringA("experiments", "consist_watch_bp", "", spec, sizeof spec, g_ini);
        uintptr_t arm[4];
        int narm = 0;
        void* c = g_consists[0];
        char* ctx = nullptr;
        for (char* tok = strtok_s(spec, ",", &ctx); tok && narm < 4 && c; tok = strtok_s(nullptr, ",", &ctx)) {
            char* colon = strchr(tok, ':');
            if (!colon) continue;
            uintptr_t obj = reinterpret_cast<uintptr_t>(c);
            if (*tok != '-') {
                uint64_t v = 0;
                SafeRead(static_cast<uint8_t*>(c) + strtoul(tok, nullptr, 16), &v, 8);
                obj = v;
            }
            arm[narm] = obj + strtoul(colon + 1, nullptr, 16);
            Log("consist watch: bp %d on %s (%p)", narm, tok, reinterpret_cast<void*>(arm[narm]));
            ++narm;
        }
        if (!narm) return 0;
        AddVectoredExceptionHandler(1, &BpHandler);
        ArmAll(arm, narm);
        Sleep(3000);
        ArmAll(arm, 0);
        for (int d = 0; d < narm; ++d)
            for (const BpHit& h : g_bp[d])
                if (h.key) {
                    uintptr_t eb = g_engineBase;
                    bool in = static_cast<uintptr_t>(h.key) >= eb && static_cast<uintptr_t>(h.key) < eb + 0x2000000;
                    Log("consist watch: bp %d written at %s %llx x%ld (return address engine rva %llx)", d, in ? "engine rva" : "address",
                        static_cast<unsigned long long>(in ? h.key - eb : h.key), h.count,
                        static_cast<unsigned long long>(h.ret >= eb && h.ret < eb + 0x2000000 ? h.ret - eb : 0));
                }
        Log("consist watch: moving values done");
        return 0;
    }
    // consist_watch=2: the consist object and the objects its pointer fields point to (first
    // 0x200 bytes each), as floats and as doubles: the values that move while the train stands.
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, g_ini) == 2) {
        vals.clear();
        struct Obj { int from; uintptr_t at; };
        std::vector<Obj> objs;
        for (void* c : g_consists) {
            if (!c) continue;
            objs.push_back({-1, reinterpret_cast<uintptr_t>(c)});
            uint64_t field[0x600 / 8];
            if (!SafeRead(c, field, sizeof field)) continue;
            for (int i = 0; i < 0x600 / 8; ++i) {
                uint8_t probe[8];
                if (field[i] > 0x10000 && field[i] < 0x7fffffffffffull && !(field[i] & 7) && SafeRead(reinterpret_cast<void*>(field[i]), probe, 8))
                    objs.push_back({i * 8, field[i]});
            }
        }
        struct Sample { uintptr_t at; int from; double lo[2], hi[2]; };  // [0] float, [1] double
        std::vector<Sample> sm;
        for (const Obj& o : objs)
            for (int k = 0; k < (o.from < 0 ? 0x600 : 0x200); k += 4) sm.push_back({o.at + k, o.from, {1e30, 1e30}, {-1e30, -1e30}});
        for (int t = 0; t < 200; ++t) {
            for (Sample& x : sm) {
                uint8_t b8[8];
                if (!SafeRead(reinterpret_cast<void*>(x.at), b8, 8)) continue;
                float f;
                double d;
                memcpy(&f, b8, 4), memcpy(&d, b8, 8);
                if (f == f) x.lo[0] = f < x.lo[0] ? f : x.lo[0], x.hi[0] = f > x.hi[0] ? f : x.hi[0];
                if (d == d && !(x.at & 7)) x.lo[1] = d < x.lo[1] ? d : x.lo[1], x.hi[1] = d > x.hi[1] ? d : x.hi[1];
            }
            Sleep(20);
        }
        int logged = 0;
        for (const Sample& x : sm)
            for (int k = 0; k < 2; ++k) {
                double a = x.lo[k], b = x.hi[k];
                if (!(b > a) || a < -1e7 || b > 1e7 || (b - a) > 10 || (k == 0 && b - a < 1e-9) || (k == 1 && (b - a < 1e-12 || (a > -1e-30 && b < 1e-30))) || logged >= 400) continue;
                ++logged;
                Log("consist watch: %s+0x%x %p as %s: %.10g .. %.10g (range %.3g)", x.from < 0 ? "consist" : "ptr", x.from < 0 ? 0 : x.from,
                    reinterpret_cast<void*>(x.at), k ? "double" : "float", a, b, b - a);
            }
        for (const Obj& o : objs) Log("consist watch: object consist+0x%x at %p", o.from < 0 ? 0 : o.from, reinterpret_cast<void*>(o.at));
        Log("consist watch: %zu objects, %d moving values", objs.size(), logged);
        return 0;
    }
    std::sort(vals.begin(), vals.end(), [](const Val& a, const Val& b) { return a.hi - a.lo > b.hi - b.lo; });
    int logged = 0;
    for (const Val& v : vals)
        if (v.hi > v.lo && logged++ < 40) Log("consist watch: %p %.9g .. %.9g (range %.3g)", reinterpret_cast<void*>(v.at), v.lo, v.hi, v.hi - v.lo);
    uintptr_t arm[4];
    int narm = 0;
    for (const Val& v : vals)
        if (v.hi > v.lo && v.hi - v.lo < 1.0f && narm < 4) arm[narm++] = v.at;
    if (!narm) return 0;
    AddVectoredExceptionHandler(1, &BpHandler);
    ArmAll(arm, narm);
    Sleep(3000);
    ArmAll(arm, 0);
    for (int d = 0; d < narm; ++d)
        for (const BpHit& h : g_bp[d])
            if (h.key) {
                uintptr_t eb = g_engineBase;
                bool in = static_cast<uintptr_t>(h.key) >= eb && static_cast<uintptr_t>(h.key) < eb + 0x2000000;
                Log("consist watch: bp %d (%p) written at %s %llx x%ld (return address engine rva %llx)", d, reinterpret_cast<void*>(arm[d]), in ? "engine rva" : "address",
                    static_cast<unsigned long long>(in ? h.key - eb : h.key), h.count,
                    static_cast<unsigned long long>(h.ret >= eb && h.ret < eb + 0x2000000 ? h.ret - eb : 0));
            }
    return 0;
}

void HeadSpring(void* camera) {
    auto* bp = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(camera) + kCameraBlueprint);
    if (g_consistWatch) {
        static bool started = false;
        if (!started) started = true, CloseHandle(CreateThread(nullptr, 0, &ConsistWatch, nullptr, 0, nullptr));
    }
    if (g_watch) {
        static bool started = false;
        g_camera = camera;
        if (!started) started = true, CloseHandle(CreateThread(nullptr, 0, &CameraWatch, nullptr, 0, nullptr));
        Log("camera watch: cab camera %p", camera);
    }
    if (!g_spring) {
        Log("physicsfix: cab camera spring not registered (camera fixed to the cab)");
        return;
    }
    if (!bp) return g_headSpring(camera);
    float v[6], saved[6];
    memcpy(saved, bp + kBlueprintSpring, sizeof saved);
    for (int i = 0; i < 6; ++i) v[i] = saved[i] * (i < 3 ? g_springPercent : g_dampingPercent) / 100.0f;
    memcpy(bp + kBlueprintSpring, v, sizeof v);
    Log("physicsfix: cab camera spring %.3f %.3f %.3f damping %.3f %.3f %.3f (blueprint %.3f %.3f %.3f / %.3f %.3f %.3f)",
        v[0], v[1], v[2], v[3], v[4], v[5], saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]);
    g_headSpring(camera);
    memcpy(bp + kBlueprintSpring, saved, sizeof saved);
}
}  // namespace

// A braked train that stands is never still. The per-bogie force step (TrainFizzix, PhysX 2.8)
// brakes with F = -30 m v below walking pace: it damps the velocity but never holds the position,
// so the coupling joints and the wheels' per-step placement on the rails keep the bogies creeping by
// millimetres (measured at Haarlem: cars 2 to 5 mm apart in 6 s, the cab turning by 4e-5 rad).
// The camera rides on that, and surfaces that lie close together (signal lenses, poles) flip in
// the depth test from frame to frame. [fixes] brake_hold (default on): when the game's own brake law
// holds a bogie (no traction, brake on, brake force left over, brake stronger than the gradient's
// pull) for half a second, the bogie is kept at the pose it had then, with zero velocity, until
// one of those stops being true (power, brake released, gradient); while all bogies of a consist are
// held, its car bodies are kept too. The actor calls use the PhysX
// 2.8 NxActor vtable (slots checked against the game's own calls: getGlobalPose 9, setGlobalPose
// 5, getLinearVelocity 58; getMass 46 reads the bogie's mass).
namespace {
constexpr sites::Fingerprint kForceStep = {0xf8c0380b0bfcddf0ull, 16};  // (bogie, force state)
constexpr sites::Fingerprint kConsistStep = {0xf75cae00456c8d57ull, 16};  // (consist, flag): reads the actors' poses
constexpr int kSetGlobalPose = 5, kGetGlobalPose = 9, kSetLinearVelocity = 56, kSetAngularVelocity = 57;
constexpr int kHoldSteps = 30;  // steps the brake must hold before the pose is kept (0.5 s at 60 Hz)
struct Held { void* actor; int steps; bool holding; ULONGLONG seen; float pose[12]; };
constexpr int kHeldSlots = 512;  // power of two
Held g_held[kHeldSlots];
volatile LONG g_holds = 0, g_releases = 0;
ForceStepFn g_holdStep = nullptr;

Held g_bodies[kHeldSlots];  // the other actors of held consists (car bodies)

Held* HeldIn(Held* table, void* actor, bool add) {
    size_t at = (reinterpret_cast<uintptr_t>(actor) >> 4) * 0x9E3779B97F4A7C15ull >> 55;
    for (int probe = 0; probe < 16; ++probe) {
        Held& h = table[(at + probe) & (kHeldSlots - 1)];
        if (h.actor == actor) {
            // An entry not touched for a second belongs to an actor that is gone; its address may
            // have been reused: start over.
            ULONGLONG now = GetTickCount64();
            if (now - h.seen > 1000) h.steps = 0, h.holding = false;
            h.seen = now;
            return &h;
        }
        if (!h.actor) return add ? (h.actor = actor, h.seen = GetTickCount64(), &h) : nullptr;
    }
    return nullptr;  // (full: that actor is not held)
}
Held* HeldFor(void* actor) { return HeldIn(g_held, actor, true); }

void Keep(void* actor, const float* pose) {
    static const float zero[3] = {0, 0, 0};
    void** vt = *reinterpret_cast<void***>(actor);
    reinterpret_cast<void (*)(void*, const float*)>(vt[kSetGlobalPose])(actor, pose);
    reinterpret_cast<void (*)(void*, const float*)>(vt[kSetLinearVelocity])(actor, zero);
    reinterpret_cast<void (*)(void*, const float*)>(vt[kSetAngularVelocity])(actor, zero);
}

// Once per frame, before the consist reads its actors' poses (consist +0x1b0: its actors, begin and
// end): while every bogie of the consist is held, its other actors (the car bodies, which hang on
// the bogies) are kept at the pose they had when the hold began, so the drawn train and the cab
// camera do not move at all.
using ConsistFn = void (*)(void* consist, char flag);
ConsistFn g_consistStep = nullptr;
void ConsistHold(void* consist, char flag) {
    for (auto& c : g_consists) {  // (for the consist_watch diagnosis)
        void* v = c;
        if (v == consist) break;
        if (!v && InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&c), consist, nullptr) == nullptr) break;
    }
    void** be[2] = {};
    if (SafeRead(static_cast<uint8_t*>(consist) + 0x1b0, be, sizeof be) && be[0] && be[1] > be[0] && be[1] - be[0] <= 64) {
        int bogies = 0, holding = 0;
        for (void** at = be[0]; at < be[1]; ++at)
            if (Held* h = HeldIn(g_held, *at, false)) ++bogies, holding += h->holding;
        bool held = bogies > 0 && holding == bogies;
        for (void** at = be[0]; at < be[1]; ++at) {
            if (!*at || HeldIn(g_held, *at, false)) continue;
            Held* b = HeldIn(g_bodies, *at, held);
            if (!b) continue;
            if (!held) {
                b->holding = false;
                continue;
            }
            if (!b->holding) {
                void** vt = **reinterpret_cast<void****>(at);
                reinterpret_cast<void* (*)(void*, float*)>(vt[kGetGlobalPose])(*at, b->pose);
                b->holding = true;
            }
            Keep(*at, b->pose);
        }
    }
    g_consistStep(consist, flag);
}

void BrakeHold(int64_t* bogie, float* state) {
    const float traction = state[0], brake = state[1] < 0 ? -state[1] : state[1], mass = state[3], grade = state[0xb] < 0 ? -state[0xb] : state[0xb];
    g_holdStep(bogie, state);
    if (mass == 0) return;  // (a second entry for the same actor, without brake or traction: it changes nothing)
    void* actor = nullptr;
    if (!bogie[1] || !SafeRead(reinterpret_cast<uint8_t*>(bogie[1]) + 8, &actor, 8) || !actor) return;
    Held* h = HeldFor(actor);
    if (!h) return;
    // state[2] after the step: the brake force the game's law did not need (it holds the bogie).
    bool holds = traction == 0 && brake > 0 && state[2] != 0 && brake > grade * mass * 9.81f;
    if (!holds) {
        if (h->holding && InterlockedIncrement(&g_releases) <= 8)
            Log("physicsfix: brake hold released, bogie %p (traction %.3g, brake %.3g)", actor, traction, brake);
        h->steps = 0, h->holding = false;
        return;
    }
    void** vt = *reinterpret_cast<void***>(actor);
    if (!h->holding) {
        if (++h->steps < kHoldSteps) return;
        reinterpret_cast<void* (*)(void*, float*)>(vt[kGetGlobalPose])(actor, h->pose);
        h->holding = true;
        if (InterlockedIncrement(&g_holds) <= 8) Log("physicsfix: brake holds bogie %p (mass %.1f)", actor, mass);
    }
    Keep(actor, h->pose);
}
}  // namespace

bool InstallBrakeHold(HMODULE engine) {
    MH_Initialize();
    void* target = sites::Find(engine, kForceStep);
    bool ok = target && MH_CreateHook(target, reinterpret_cast<void*>(&BrakeHold), reinterpret_cast<void**>(&g_holdStep)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    void* consist = ok ? sites::Find(engine, kConsistStep) : nullptr;
    bool bodies = consist && MH_CreateHook(consist, reinterpret_cast<void*>(&ConsistHold), reinterpret_cast<void**>(&g_consistStep)) == MH_OK &&
                  MH_EnableHook(consist) == MH_OK;
    Log("physicsfix: brake hold %s%s", ok ? "installed" : "NOT installed (force step not found)", ok && !bodies ? " (bogies only: consist step not found)" : "");
    return ok;
}

bool InstallCabCamera(HMODULE engine, int springPercent, int dampingPercent, bool spring, bool watch) {
    g_springPercent = springPercent, g_dampingPercent = dampingPercent, g_spring = spring, g_watch = watch;
    g_engineBase = reinterpret_cast<uintptr_t>(engine);
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
    g_watchSkip = GetPrivateProfileIntA("experiments", "camera_watch_skip", 0, ini);
    g_watchWorld = GetPrivateProfileIntA("experiments", "camera_watch_world", 0, ini);
    g_watch = GetPrivateProfileIntA("experiments", "camera_watch", 0, ini) != 0;
    strcpy_s(g_ini, ini);
    if (GetPrivateProfileIntA("experiments", "consist_watch", 0, ini)) {
        void* at = sites::Find(engine, kConsistStep);
        MH_Initialize();
        bool hooked = at && MH_CreateHook(at, reinterpret_cast<void*>(&ConsistUpdate), reinterpret_cast<void**>(&g_consistUpdate)) == MH_OK && MH_EnableHook(at) == MH_OK;
        Log("consist watch: consist update hook %s", hooked ? "installed" : "NOT installed");
        g_consistWatch = true;  // (the watch starts when the cab camera becomes active; the brake hold's hook also records the consists)
        if (GetPrivateProfileIntA("experiments", "consist_watch", 0, ini) == 7) {
            void* f = sites::Find(engine, kForceStep);
            bool ok = f && MH_CreateHook(f, reinterpret_cast<void*>(&ForceStep), reinterpret_cast<void**>(&g_forceStep)) == MH_OK && MH_EnableHook(f) == MH_OK;
            Log("consist watch: force step hook %s", ok ? "installed" : "NOT installed");
        }
    }
    MH_Initialize();
    void* target = sites::Find(engine, kCabHeadSpring);
    bool ok = target && MH_CreateHook(target, reinterpret_cast<void*>(&HeadSpring), reinterpret_cast<void**>(&g_headSpring)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    Log("physicsfix: cab camera spring hook %s (spring %d%%, damping %d%%%s)", ok ? "installed" : "NOT installed", springPercent,
        dampingPercent, spring ? "" : ", no spring");
    return ok;
}

}  // namespace tscfix::physicsfix
