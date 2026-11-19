#include "rtdevice.h"
#include "trace.h"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>
#include <unordered_map>
#include <type_traits>
#include <utility>

#include "ab.h"
#include "d3dlayer.h"
#include "experiments.h"
#include "framelog.h"
#include "overlap.h"
#include "lookup.h"
#include "profiler.h"
#include "physicsfix.h"
#include "scripttiming.h"
#include "shadowcull.h"
#include "glitch.h"
#include "log.h"
#include "status.h"

// Default policy for every generated method: drain the queue (threaded mode), then
// call the runtime directly. Hot methods are overridden below. Creating an object or reading
// static device facts needs no drain: nothing queued can refer to a new object.
constexpr bool NoDrain(int slot) {
    switch (slot) {
        case 6: case 7:                               // GetDirect3D, GetDeviceCaps
        case 23: case 24: case 25: case 26: case 27:  // Create{,Volume,Cube}Texture, Create{Vertex,Index}Buffer
        case 28: case 29: case 36:                    // CreateRenderTarget, CreateDepthStencilSurface, CreateOffscreenPlainSurface
        case 86: case 91: case 106: case 118:         // CreateVertexDeclaration, CreateVertexShader, CreatePixelShader, CreateQuery
            return true;
        default:
            return false;
    }
}
#define TSCFIX_RT_FORWARD(slot, Name, Ret, args) \
    if (!NoDrain(slot)) BeforeSync(slot);        \
    return real_->Name args

#include "rt_device_gen.h"  // generated into build/ from the SDK's d3d9.h

#pragma comment(lib, "synchronization.lib")
#pragma warning(disable : 4324)  // intentional padding: producer/consumer indices on separate cache lines

namespace tscfix::rt {
namespace {

// The calling thread's id straight from the thread block (what GetCurrentThreadId returns).
__forceinline DWORD CurrentTid() { return __readgsdword(0x48); }

// Per-thread cache of RtDevice::Role, valid while the device's role epoch is unchanged.
struct RoleCache {
    const void* device;
    uint32_t epoch;
    int role;
};
thread_local RoleCache t_role{};
thread_local int t_ahead = 0;  // forks this thread made since its last ResumeAfterForks
thread_local int t_cont = 0;  // the segment it continues in after them (reserved with the first)
thread_local uint32_t t_forkGen = 0;  // the overlap those belong to (stale ones are dropped)
thread_local uint64_t t_records = 0;  // commands this thread recorded (see ThreadRecords)
thread_local uint32_t* t_slotCounts = nullptr;
thread_local bool t_direct = false;  // see SetDirectCaller
DWORD g_renderTid = 0;  // the render (replay) thread
int ReleaseOwnPauseBeforeWait();  // (see PauseForDirectCall)
bool OwnPauseHeld();  // the calling thread has the render thread parked
void RetakeOwnPause(int depth);
// Draw call sites while counting (measurement): the first three stack frames above the device.
struct DrawSite { void* f[3]; uint32_t n; };
thread_local DrawSite* t_drawSites = nullptr;
thread_local int t_drawSiteCap = 0;
__declspec(noinline) void NoteDrawSite() {
    void* f[8] = {};
    USHORT got = RtlCaptureStackBackTrace(5, 8, f, nullptr);  // above the geometry node draw chain
    void* key[3] = {got > 0 ? f[0] : nullptr, got > 1 ? f[1] : nullptr, got > 2 ? f[2] : nullptr};
    for (int i = 0; i < t_drawSiteCap; ++i) {
        DrawSite& d = t_drawSites[i];
        if (!d.n) { memcpy(d.f, key, sizeof key); d.n = 1; return; }
        if (!memcmp(d.f, key, sizeof key)) { ++d.n; return; }
    }
}  // per method slot, when set (see CountSlots); 255: raw

// --- single-producer / single-consumer command ring ---------------------------------------
// A command is [Header][closure][inline data], 16-byte aligned, never split across the end.
class Ring {
 public:
    static constexpr size_t kSize = size_t(32) << 20;

    Ring() { buf_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); }

    // Records f(const uint8_t* data); `data` is d1 (n1 bytes) followed by d2 (n2 bytes),
    // copied straight into the ring.
    template <typename F>
    __forceinline void Push(F&& f, const void* d1 = nullptr, size_t n1 = 0, const void* d2 = nullptr, size_t n2 = 0) {
        using Fn = std::decay_t<F>;
        size_t need = Align(sizeof(Header) + sizeof(Fn) + n1 + n2);
        size_t pos = head_.load(std::memory_order_relaxed);
        // Fast path: contiguous room and known free space; otherwise wrap or wait.
        if (kSize - pos % kSize < need || pos + need - cachedTail_ > kSize) pos = Reserve(need);
        auto* h = reinterpret_cast<Header*>(buf_ + pos % kSize);
        h->size = static_cast<uint32_t>(need);
        h->exec = &Exec<Fn>;
        auto* fn = new (h + 1) Fn(std::forward<F>(f));
        auto* out = reinterpret_cast<uint8_t*>(fn) + sizeof(Fn);
        if (n1) memcpy(out, d1, n1);
        if (n2) memcpy(out + n1, d2, n2);
        Publish(pos + need);
    }

    // Consumer: run at most one command; returns false when the queue is empty.
    // The producer's head is re-read only when the locally known work is used up, so
    // the two threads do not bounce a cache line per command.
    bool RunOne() {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == cachedHead_) {
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (t == cachedHead_) return false;
        }
        auto* h = reinterpret_cast<Header*>(buf_ + t % kSize);
        if (h->exec) h->exec(h);
        tail_.store(t + h->size, std::memory_order_release);
        return true;
    }

    // Producer: copies an already formatted command (see SideBuffer) into the ring.
    void PushBlock(const uint8_t* cmd, size_t size) {
        size_t pos = head_.load(std::memory_order_relaxed);
        if (kSize - pos % kSize < size || pos + size - cachedTail_ > kSize) pos = Reserve(size);
        memcpy(buf_ + pos % kSize, cmd, size);
        Publish(pos + size);
    }

    bool Empty() const { return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire); }
    const volatile size_t* HeadAddress() const { return reinterpret_cast<const volatile size_t*>(&head_); }
    size_t Head() const { return head_.load(std::memory_order_acquire); }
    size_t Tail() const { return tail_.load(std::memory_order_acquire); }

 private:
    struct alignas(16) Header {
        void (*exec)(Header*);
        uint32_t size;
    };
    static size_t Align(size_t n) { return (n + 15) & ~size_t(15); }

    template <typename Fn>
    static void Exec(Header* h) {
        auto* fn = reinterpret_cast<Fn*>(h + 1);
        (*fn)(reinterpret_cast<const uint8_t*>(fn) + sizeof(Fn));
        fn->~Fn();
    }

    size_t Reserve(size_t need) {
        size_t pos = head_.load(std::memory_order_relaxed);
        size_t room = kSize - pos % kSize;
        if (room < need) {  // pad to the end so a command is never split
            WaitForSpace(pos, room);
            auto* h = reinterpret_cast<Header*>(buf_ + pos % kSize);
            h->exec = nullptr;
            h->size = static_cast<uint32_t>(room);
            pos += room;
            Publish(pos);
        }
        WaitForSpace(pos, need);
        return pos;
    }

    // Producer: re-read the consumer's tail only when the locally known free space runs out.
    void WaitForSpace(size_t pos, size_t need) {
        while (pos + need - cachedTail_ > kSize) {
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (pos + need - cachedTail_ > kSize) _mm_pause();
        }
    }

    void Publish(size_t newHead) {
        head_.store(newHead, std::memory_order_release);
        if (consumerSleeping.load(std::memory_order_acquire)) WakeByAddressSingle(&head_);
    }

    uint8_t* buf_ = nullptr;
    alignas(64) std::atomic<size_t> head_{0};
    size_t cachedTail_ = 0;  // producer's view of tail_ (same line as head_: both producer-owned)
    alignas(64) std::atomic<size_t> tail_{0};
    size_t cachedHead_ = 0;  // consumer's view of head_

 public:
    alignas(64) std::atomic<bool> consumerSleeping{false};
};

// Commands recorded by a side recorder while a worker owns the ring (overlap). Same layout as
// the ring ([Header][closure][data], 16-byte aligned). At the join the buffer is not copied:
// one ring command runs it in place on the render thread. Two buffers alternate, so the next
// overlap records into the other one while the render thread may still be running this one.
// Cache-line aligned: each recording thread pushes into its own, and they sit in an array.
class alignas(64) SideBuffer {
 public:
    static constexpr size_t kSize = size_t(64) << 20;

    template <typename F>
    void Push(F&& f, const void* d1 = nullptr, size_t n1 = 0, const void* d2 = nullptr, size_t n2 = 0) {
        using Fn = std::decay_t<F>;
        static_assert(std::is_trivially_copyable_v<Fn>, "side-buffer commands are not destroyed");
        uint8_t*& buf = buf_[cur_];
        // Address space reserved whole, committed in steps as it fills (many segments exist).
        if (!buf) buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_READWRITE));
        size_t need = (sizeof(Header) + sizeof(Fn) + n1 + n2 + 15) & ~size_t(15);
        if (used_ + need > kSize) {  // cannot happen in a frame; fail loudly rather than corrupt
            Log("rt: side buffer full");
            __fastfail(7);
        }
        if (used_ + need > committed_[cur_]) {
            size_t to = (used_ + need + kCommitStep - 1) & ~(kCommitStep - 1);
            if (!VirtualAlloc(buf + committed_[cur_], to - committed_[cur_], MEM_COMMIT, PAGE_READWRITE)) {
                Log("rt: side buffer commit failed");
                __fastfail(7);
            }
            committed_[cur_] = to;
        }
        auto* h = reinterpret_cast<Header*>(buf + used_);
        h->exec = &Exec<Fn>;
        h->size = static_cast<uint32_t>(need);
        h->check = kCheck | (GetCurrentThreadId() & 0xffff);
        auto* fn = new (h + 1) Fn(std::forward<F>(f));
        auto* out = reinterpret_cast<uint8_t*>(fn) + sizeof(Fn);
        if (n1) memcpy(out, d1, n1);
        if (n2) memcpy(out + n1, d2, n2);
        used_ += need;
    }
    // Starts a new recording; waits (rarely) until the render thread is done with the buffer.
    void Reset() {
        used_ = 0;
        while (busy_[cur_]) _mm_pause();
    }
    // Queues one ring command that runs everything recorded here. Returns the bytes handed over.
    template <typename RingT>
    size_t HandTo(RingT& ring) {
        if (!used_) return 0;
        struct Run {
            uint8_t* begin;
            size_t size;
            volatile long* busy;
            int segment, which;
        } run{buf_[cur_], used_, &busy_[cur_], id_, cur_};
        busy_[cur_] = 1;
        ring.Push([run](const uint8_t*) {
            uint32_t lastCheck = 0;
            void* lastExec = nullptr;
            for (size_t at = 0; at < run.size;) {
                auto* h = reinterpret_cast<Header*>(run.begin + at);
                // A damaged command (something wrote into a buffer being recorded or replayed):
                // say where, and drop the rest of this buffer rather than jump to garbage.
                if ((h->check & 0xffff0000u) != kCheck || !h->size || at + h->size > run.size) {
                    Log("rt: damaged side command: segment %d buffer %d at %zu of %zu (check %08x size %u exec %p); "
                        "previous check %08x exec %p",
                        run.segment, run.which, at, run.size, h->check, h->size, reinterpret_cast<void*>(h->exec),
                        lastCheck, lastExec);
                    break;
                }
                lastCheck = h->check, lastExec = reinterpret_cast<void*>(h->exec);
                at += h->size;
                RenderPausePoint();
                h->exec(h);
            }
            InterlockedExchange(run.busy, 0);
        });
        size_t n = used_;
        cur_ ^= 1;
        used_ = 0;
        while (busy_[cur_]) _mm_pause();  // the other buffer is from two overlaps ago: done by now
        return n;
    }

 private:
    // Must match Ring's command header (check sits in its padding).
    struct alignas(16) Header {
        void (*exec)(Header*);
        uint32_t size;
        uint32_t check;  // kCheck | low 16 bits of the recording thread's id
    };
    static constexpr uint32_t kCheck = 0x5EC00000u;
 public:
    int id_ = 0;  // segment number (for the damage report)
 private:
    template <typename Fn>
    static void Exec(Header* h) {
        auto* fn = reinterpret_cast<Fn*>(h + 1);
        (*fn)(reinterpret_cast<const uint8_t*>(fn) + sizeof(Fn));
    }
    uint8_t* buf_[2] = {};
    size_t committed_[2] = {};
    static constexpr size_t kCommitStep = size_t(1) << 20;
    volatile long busy_[2] = {};
    int cur_ = 0;
    size_t used_ = 0;
};

void (*g_joinHook)() = nullptr;  // overlap: waits for the worker and ends the overlap
bool (*g_workerDoneHook)() = nullptr;
void (*g_sideAppendedHook)(uint64_t head) = nullptr;
volatile bool g_filterStates = false;  // drop render / sampler sets equal to the known value

struct SaveSite {  // see AddSaveRestoreSite
    char module[64];
    uintptr_t ret;
    uintptr_t base;
};
SaveSite g_saveSites[16];
int g_saveSiteCount = 0;  // overlap: the worker has finished (joining costs nothing)

// Keeps objects referenced by recorded commands alive without an AddRef per command
// (with D3DCREATE_MULTITHREADED every AddRef takes the runtime's device lock, which
// the render thread holds while it works). An object is AddRef'd the first time it is
// bound, stays pinned while it keeps being used, and is released by the render thread
// once it has been unused for a few frames.
class PinTable {
 public:
    static constexpr size_t kCapacity = 1 << 15;  // power of two, open addressing
    static constexpr uint64_t kKeepFrames = 3;

    // Main thread. Returns the object unchanged, for use in a capture list.
    template <typename T>
    T* Pin(T* obj, uint64_t frame) {
        if (!obj) return obj;
        size_t i = Hash(obj);
        for (;;) {
            Entry& e = slots_[i];
            if (e.obj == obj) { e.lastUsed = frame; return obj; }
            if (!e.obj) {
                if (count_ * 4 >= kCapacity * 3) { overflow_ = true; break; }  // full: fall back below
                obj->AddRef();
                e.obj = obj;
                e.lastUsed = frame;
                ++count_;
                return obj;
            }
            i = (i + 1) & (kCapacity - 1);
        }
        obj->AddRef();  // table full: leak-safe fallback, released on the next sweep pass via the list
        extra_[extraCount_ < kExtra ? extraCount_++ : kExtra - 1] = obj;
        return obj;
    }

    // Main thread, once per frame: collect objects unused for kKeepFrames frames.
    // Returns how many were moved to `out` (to be released on the render thread).
    size_t Sweep(uint64_t frame, IUnknown** out, size_t max, bool all = false) {
        size_t n = 0;
        for (size_t i = 0; i < kCapacity && n < max; ++i) {
            Entry& e = slots_[i];
            if (e.obj && (all || e.lastUsed + kKeepFrames < frame)) {
                out[n++] = e.obj;
                e.obj = nullptr;
                --count_;
            }
        }
        for (size_t k = 0; k < extraCount_ && n < max; ++k) out[n++] = extra_[k];
        extraCount_ = 0;
        if (n) Rehash();
        return n;
    }

    size_t Count() const { return count_; }

 private:
    struct Entry { IUnknown* obj = nullptr; uint64_t lastUsed = 0; };
    static size_t Hash(const void* p) { return (reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 49 & (kCapacity - 1); }
    // Open addressing needs re-insertion after removals so probe chains stay intact.
    void Rehash() {
        static Entry tmp[kCapacity];
        memcpy(tmp, slots_, sizeof slots_);
        memset(slots_, 0, sizeof slots_);
        for (const Entry& e : tmp) {
            if (!e.obj) continue;
            size_t i = Hash(e.obj);
            while (slots_[i].obj) i = (i + 1) & (kCapacity - 1);
            slots_[i] = e;
        }
    }
    static constexpr size_t kExtra = 4096;
    Entry slots_[kCapacity];
    size_t count_ = 0;
    IUnknown* extra_[kExtra];
    size_t extraCount_ = 0;
    bool overflow_ = false;
};

int SamplerIndex(DWORD s) { return s < 16 ? int(s) : (s >= 256 && s <= 260) ? int(s - 240) : -1; }

UINT VertexCount(D3DPRIMITIVETYPE type, UINT prims) {
    switch (type) {
        case D3DPT_POINTLIST: return prims;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_TRIANGLELIST: return prims * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        default: return 0;
    }
}

class RtDevice;
RtDevice* g_device = nullptr;
// Exclusive device access for a thread that must call the device directly while the render
// thread replays (a side recorder building geometry): the render thread parks at its next
// command boundary (or is idle) until the caller is done.
std::atomic<int> g_pauseRequests{0};
std::atomic<bool> g_renderPaused{false};
SRWLOCK g_exclusiveLock = SRWLOCK_INIT;
void RenderPausePoint() {
    if (!g_pauseRequests.load(std::memory_order_acquire)) return;
    g_renderPaused.store(true, std::memory_order_release);
    while (g_pauseRequests.load(std::memory_order_acquire)) YieldProcessor();
    g_renderPaused.store(false, std::memory_order_release);
}
// [experiments] park_keeps_state: a direct call made while the render thread is parked leaves its
// own shaders, textures and states on the device (D3DX validates a technique by applying it). The
// parked thread is in the middle of another thread's part of the command stream and would go on
// with that state: a few draws come out wrong for one frame (roof steelwork at East Croydon with
// two shadow threads). With this on, the device state is saved when the park begins and put back
// before the render thread goes on. The parking thread does both; it has the device to itself.
bool g_parkKeepsState = false;
IDirect3DDevice9* g_parkDevice = nullptr;
IDirect3DStateBlock9* g_parkBlock = nullptr;  // (used only under g_exclusiveLock)
volatile long g_parkSaves = 0;
void ParkSaveState() {
    if (!g_parkKeepsState || !g_parkDevice) return;
    if (!g_parkBlock && FAILED(g_parkDevice->CreateStateBlock(D3DSBT_ALL, &g_parkBlock))) g_parkBlock = nullptr;
    if (g_parkBlock) g_parkBlock->Capture(), InterlockedIncrement(&g_parkSaves);
}
void ParkRestoreState() {
    if (g_parkKeepsState && g_parkBlock) g_parkBlock->Apply();
}
void ParkDropBlock() {  // before a device Reset or release: a state block keeps the device from resetting
    AcquireSRWLockExclusive(&g_exclusiveLock);
    if (g_parkBlock) g_parkBlock->Release(), g_parkBlock = nullptr;
    ReleaseSRWLockExclusive(&g_exclusiveLock);
}
bool g_splitCores = false;
volatile bool g_descCache = true;  // A/B switch for the render-target GetDesc cache

// Keeps the game thread and the render thread on different physical cores, so they
// never share one core's execution units through SMT: the game thread gets the first
// half of the physical cores (both hardware threads each), the render thread the rest.
void SplitCores(HANDLE renderThread) {
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    auto* info = static_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(_alloca(len));
    if (!GetLogicalProcessorInformation(info, &len)) return;
    ULONG_PTR cores[64];
    int n = 0;
    for (DWORD i = 0; i < len / sizeof(*info) && n < 64; ++i)
        if (info[i].Relationship == RelationProcessorCore) cores[n++] = info[i].ProcessorMask;
    if (n < 2) return;
    ULONG_PTR game = 0, render = 0;
    for (int i = 0; i < n; ++i) (i < n / 2 ? game : render) |= cores[i];
    SetThreadAffinityMask(GetCurrentThread(), game);
    SetThreadAffinityMask(renderThread, render);
    Log("rt: %d physical cores; game thread mask 0x%llx, render thread mask 0x%llx", n,
        static_cast<unsigned long long>(game), static_cast<unsigned long long>(render));
}

// Shadowed transforms: view, projection, texture 0-7, world (others are not tracked).
inline int XfIndex(D3DTRANSFORMSTATETYPE s) {
    if (s == D3DTS_VIEW) return 0;
    if (s == D3DTS_PROJECTION) return 1;
    if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7) return 2 + (s - D3DTS_TEXTURE0);
    if (s == D3DTS_WORLD) return 10;
    return -1;
}

// Device state the recorder knows (generation-stamped; ++gen forgets everything except render
// targets). RtDevice keeps one per recording thread.
struct alignas(64) Shadow {  // one per recording thread, in an array: no shared lines
    uint32_t gen = 1;
    DWORD rs[256] = {};          uint32_t rsKnown[256] = {};
    DWORD tss[8][33] = {};       uint32_t tssKnown[8][33] = {};
    D3DVIEWPORT9 viewport = {};  uint32_t viewportKnown = 0;
    D3DMATRIX xf[11] = {};       uint32_t xfKnown[11] = {};  // view, projection, texture 0-7, world
    DWORD samp[16][14] = {};     uint32_t sampKnown[16][14] = {};  // sampler states 1..13 (index = type)
    float npatch = 0;            uint32_t npatchKnown = 0;
    IDirect3DSurface9* rt[4] = {};  bool rtKnown[4] = {};
    uint32_t loans[4] = {};  // GetRenderTarget results handed out without AddRef
    D3DSURFACE_DESC rtDesc[4] = {}; bool rtDescValid[4] = {};
    IDirect3DSurface9* ds = nullptr; bool dsKnown = false;
    bool invalAll = false, invalEffect = false;  // side buffer: ran something that forgot state
};

class RtDevice final : public RtDeviceGen {
 public:
    RtDevice(IDirect3DDevice9* real, Mode mode) : mode_(mode) {
        real_ = real;
        mainThread_ = CurrentTid();
        if (mode_ == Mode::Threaded) {
            thread_ = CreateThread(nullptr, 0, &RenderThreadMain, this, 0, nullptr);
            SetThreadDescription(thread_, L"tscfix render");
            SetThreadPriority(thread_, THREAD_PRIORITY_ABOVE_NORMAL);
            if (g_splitCores) SplitCores(thread_);
        }
        InitShadow();
        status::Set("render", mode_ == Mode::Threaded ? "render thread ON" : "device wrapper (pass-through)");
    }

    ~RtDevice() override {
        if (thread_) {
            Drain();
            stop_.store(true);
            WakeByAddressSingle(const_cast<size_t*>(ring_.HeadAddress()));
            WaitForSingleObject(thread_, 5000);
            CloseHandle(thread_);
        }
        SweepPins(true);
        ReleaseShadowSurfaces();
    }

    // --- IUnknown ---------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
            *out = static_cast<IDirect3DDevice9*>(this);
            AddRef();
            return S_OK;
        }
        BeforeSync(0);
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (left == 0) {
            IDirect3DDevice9* real = real_;
            delete this;  // drains and stops the render thread first
            g_device = nullptr;
            ParkDropBlock();
            g_parkDevice = nullptr;
            ULONG realLeft = real->Release();
            Log("rt: device wrapper released (real device refs left: %lu)", realLeft);
        }
        return left;
    }

    // --- synchronous path ---------------------------------------------------------------
    void BeforeSync(int slot) override {
        if (mode_ != Mode::Threaded || OnRenderThread()) return;
        int role = Role();
        if (t_direct && role < 0) {
            // Pipelined update: runs beside the recorder; the runtime's lock orders the call.
            static volatile long logged[256] = {};
            if (InterlockedIncrement(&logged[slot < 256 ? slot : 255]) <= 2)
                Log("rt: direct call slot %d (%s) from a thread beside the recorder", slot, SlotName(slot));
            return;
        }
        if (role >= 1 && (CurrentTid() != joinThread_ || OwnForksOutstanding())) {
            // A forked segment's worker cannot join (the join waits for it): it calls directly,
            // out of queue order, and is counted; such calls are the next thing to remove.
            ++forkedSyncs_[slot < 256 ? slot : 255];
            if (!OwnPauseHeld()) ++forkedUnparked_;
            {  // (measurement) who: once per distinct thread / role
                static volatile long logged[256] = {};
                if (InterlockedIncrement(&logged[slot < 256 ? slot : 255]) <= 6)
                    Log("rt: out-of-order call slot %d by thread %lu role %d, replay thread %s (join thread %lu, queue holder %lu); "
                        "settle pending %d from %d; rt0 known %d (queue %d)",
                        slot, CurrentTid(), role, OwnPauseHeld() ? "parked by it" : "NOT parked",
                        joinThread_, mainThread_.load(std::memory_order_relaxed),
                        settlePending_[role].load() ? 1 : 0, settleFrom_[role], S().rtKnown[0] ? 1 : 0,
                        sh_[0].rtKnown[0] ? 1 : 0);
            }
            return;
        }
        if (role >= 1) JoinOverlap(slot);  // the side segments must reach the queue first
        if (!ring_.Empty()) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            Drain();
            QueryPerformanceCounter(&t1);
            syncWaitTicks_ += t1.QuadPart - t0.QuadPart;
            syncWaitBySlot_[slot < 256 ? slot : 255] += t1.QuadPart - t0.QuadPart;
            ++syncs_;
            ++syncsBySlot_[slot < 256 ? slot : 255];
        }
    }

    void Drain() {
        // A thread that parked the render thread for its own direct calls must let it go before
        // waiting for it (soakPipe18d: the game thread drained as the recorder while its pause from
        // an earlier direct effect call still held the render thread).
        // A scoped pause it holds (technique selection around a build whose effect calls drain) is
        // suspended for the wait and taken again after; drained, the render thread is idle anyway.
        int held = ReleaseOwnPauseBeforeWait();
        int spins = 0;
        while (!ring_.Empty()) {
            if (++spins < 4096) _mm_pause();
            else SwitchToThread();
        }
        RetakeOwnPause(held);
    }

    // "Main thread" here means the recording thread: the game thread, or a worker it has
    // handed recording to while it waits (see SetRecordingThread).
    // 0: records into the queue, 1: records into the side buffer (overlap), -1: neither.
    __forceinline int Role() const {
        uint32_t e = roleEpoch_.load(std::memory_order_acquire);
        if (t_role.device == this && t_role.epoch == e) return t_role.role;
        DWORD t = CurrentTid();
        int r = -1;
        if (t == mainThread_.load(std::memory_order_relaxed)) r = 0;
        else
            for (int k = 1; k < kSegs; ++k)
                if (t == segThread_[k].load(std::memory_order_relaxed)) {
                    r = k;
                    break;
                }
        t_role = {this, e, r};
        return r;
    }
    // Every change of who records where bumps the epoch, so cached roles are recomputed.
    // Own cache line: read on every recorded call, written only when roles change.
    alignas(64) std::atomic<uint32_t> roleEpoch_{1};
    char roleEpochPad_[60] = {};
    void RolesChanged() { roleEpoch_.fetch_add(1, std::memory_order_acq_rel); }
    bool OnMainThread() const { return Role() >= 0; }
    __forceinline Shadow& S() { int r = Role(); return sh_[r > 0 ? r : 0]; }
    __forceinline const Shadow& S() const { int r = Role(); return sh_[r > 0 ? r : 0]; }
    void SetRecorder(DWORD tid) {
        mainThread_.store(tid, std::memory_order_relaxed);
        RolesChanged();
    }
    // D3DX effects run on the render thread and call back into this wrapper.
    bool OnRenderThread() const { return CurrentTid() == renderThread_; }

    // Keeps an object alive until recorded commands using it have run (see PinTable).
    // Each recording thread remembers what it pinned this frame (a small direct-mapped table of
    // its own): binding the same buffers and textures again, which is nearly every call, then
    // touches neither the shared table nor its lock. With three threads recording, that lock
    // and the table's "last used" stamps were shared by every bind of every thread.
    struct PinSeen { const void* obj; uint64_t stamp; };
    static constexpr size_t kPinSeen = 1024;
    std::atomic<uint32_t> pinEpoch_{1};  // bumped when every pin is dropped (the caches are stale then)
    template <typename T>
    T* Keep(T* obj) {
        if (mode_ != Mode::Threaded || !OnMainThread()) return obj;
        if (!obj) return obj;
        static thread_local PinSeen seen[kPinSeen];
        uint64_t stamp = (framesSubmitted_ << 20) ^ pinEpoch_.load(std::memory_order_relaxed);
        PinSeen& e = seen[(reinterpret_cast<uintptr_t>(obj) >> 4) * 0x9E3779B97F4A7C15ull >> 54];
        if (e.obj == obj && e.stamp == stamp) return obj;
        {
            TableLock lock(this);
            pins_.Pin(obj, framesSubmitted_);
        }
        e.obj = obj, e.stamp = stamp;
        return obj;
    }

    // Releases pinned objects on the render thread (it already holds the runtime lock
    // most of the time, so this avoids contention). `all` drops every pin.
    void SweepPins(bool all) {
        static IUnknown* batch[1 << 14];
        size_t n = pins_.Sweep(framesSubmitted_, batch, std::size(batch), all);
        if (all) pinEpoch_.fetch_add(1, std::memory_order_relaxed);
        if (!n) return;
        for (size_t i = 0; i < n && descCount_; ++i) ForgetDesc(batch[i]);
        if (all) {  // called after draining: release right here
            for (size_t i = 0; i < n; ++i) batch[i]->Release();
            return;
        }
        Record(255, [n](const uint8_t* p) {
            auto objs = reinterpret_cast<IUnknown* const*>(p);
            for (size_t i = 0; i < n; ++i) objs[i]->Release();
        }, batch, n * sizeof(IUnknown*));
    }

    // Records a command, or runs it directly when not threaded / called from another thread.
    template <typename F>
    __forceinline void Record(int slot, F&& f, const void* data = nullptr, size_t extra = 0) {
        int role = mode_ == Mode::Threaded ? Role() : -1;
        if (role < 0) {
            BeforeSync(slot);
            f(static_cast<const uint8_t*>(data));
            return;
        }
        // Once the worker is done, end the overlap right away so this thread records into the
        // queue again (the render thread works on it as it comes instead of in one burst).
        if (role >= 1 && orderCount_ == 1 && (++Rc(role).sidePushes & 127) == 0 && g_workerDoneHook && g_workerDoneHook()) {
            JoinOverlap(kSyncOverlap);
            role = Role();
        }
        ++t_records;
        if (t_slotCounts) ++t_slotCounts[slot & 255];
        if (role >= 1) seg_[role].Push(std::forward<F>(f), data, extra);
        else ring_.Push(std::forward<F>(f), data, extra);
    }

    // --- recorded state ------------------------------------------------------------------
    // --- save / restore placeholders (see AddSaveRestoreSite) -------------------------------
    static constexpr DWORD kTokenBase = 0x7A5E0000;
    static constexpr int kTokens = 64;
    DWORD tokenState_[kTokens] = {};
    DWORD captured_[kTokens] = {};  // written by the render thread
    int nextToken_ = 0;
    uint32_t placeholders_ = 0;

    bool SaveRestoreCaller(void* ret) {
        auto r = reinterpret_cast<uintptr_t>(ret);
        for (int i = 0; i < g_saveSiteCount; ++i) {
            SaveSite& v = g_saveSites[i];
            if (!v.base) v.base = reinterpret_cast<uintptr_t>(GetModuleHandleA(v.module));
            if (v.base && r == v.base + v.ret) return true;
        }
        return false;
    }

 public:
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override {
        if (v - kTokenBase < static_cast<DWORD>(kTokens) && tokenState_[v - kTokenBase] == static_cast<DWORD>(s) + 1 &&
            OnMainThread()) {
            int i = static_cast<int>(v - kTokenBase);
            tokenState_[i] = 0;
            if (s < 256) S().rsKnown[s] = 0;  // the value is only known on the render thread
            Record(57, [d = real_, s, slot = &captured_[i]](const uint8_t*) { d->SetRenderState(s, *slot); });
            return D3D_OK;
        }
        int role = Role();
        if (s < 256 && role >= 0) {
            if (blockRecording_ && role == 0) {
                block_.rs[s] = v, block_.rsKnown[s] = block_.rsIn[s] = true;
            } else {
                Shadow& sh = sh_[role];
                if (sh.rsKnown[s] == sh.gen && sh.rs[s] == v) {
                    ++Rc(role).redundantRs;
                    if (g_filterStates) return D3D_OK;
                }
                sh.rs[s] = v, sh.rsKnown[s] = sh.gen;
            }
        }
        ++Rc(role).setRs;
        Record(57, [d = real_, s, v](const uint8_t*) { d->SetRenderState(s, v); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) override {
        TrySettle();
        if (v && s < 256 && S().rsKnown[s] == S().gen) return *v = S().rs[s], D3D_OK;
        if (v && mode_ == Mode::Threaded && OnMainThread() && SaveRestoreCaller(_ReturnAddress())) {
            int i = nextToken_;
            nextToken_ = (nextToken_ + 1) % kTokens;
            tokenState_[i] = static_cast<DWORD>(s) + 1;
            Record(58, [d = real_, s, slot = &captured_[i]](const uint8_t*) { d->GetRenderState(s, slot); });
            ++placeholders_;
            *v = kTokenBase + i;
            return D3D_OK;
        }
        if (mode_ == Mode::Threaded && OnMainThread() && !ring_.Empty()) {  // a caller still to read
            static void* seen[8];
            void* ret = _ReturnAddress();
            for (auto& x : seen)
                if (x == ret) break;
                else if (!x) {
                    x = ret;
                    HMODULE m = nullptr;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<const char*>(ret), &m);
                    char name[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, name, MAX_PATH);
                    Log("rt: GetRenderState(%d) waits for the queue, caller %s+%llx", s, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(m)));
                    break;
                }
        }
        BeforeSync(58);
        HRESULT hr = real_->GetRenderState(s, v);
        if (SUCCEEDED(hr) && v && s < 256) S().rs[s] = *v, S().rsKnown[s] = S().gen;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override {
        int role = Role();
        if (s < 16 && t < 14 && role >= 0) {
            if (blockRecording_ && role == 0) {
                block_.touchesSamplers = true;
            } else {
                Shadow& sh = sh_[role];
                if (sh.sampKnown[s][t] == sh.gen && sh.samp[s][t] == v) {
                    ++Rc(role).redundantSamp;
                    if (g_filterStates) return D3D_OK;
                }
                sh.samp[s][t] = v, sh.sampKnown[s][t] = sh.gen;
            }
        }
        ++Rc(role).setSamp;
        Record(69, [d = real_, s, t, v](const uint8_t*) { d->SetSamplerState(s, t, v); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) override {
        int role = Role();
        if (s < 8 && t < 33 && role >= 0) {
            if (blockRecording_ && role == 0) block_.tss[s][t] = v, block_.tssKnown[s][t] = block_.tssIn[s][t] = true;
            else {
                Shadow& sh = sh_[role > 0 ? role : 0];
                sh.tss[s][t] = v, sh.tssKnown[s][t] = sh.gen;
            }
        }
        Record(67, [d = real_, s, t, v](const uint8_t*) { d->SetTextureStageState(s, t, v); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override {
        TrySettle();
        if (v && s < 8 && t < 33 && S().tssKnown[s][t] == S().gen) return *v = S().tss[s][t], D3D_OK;
        BeforeSync(66);
        HRESULT hr = real_->GetTextureStageState(s, t, v);
        if (SUCCEEDED(hr) && v && s < 8 && t < 33) S().tss[s][t] = *v, S().tssKnown[s][t] = S().gen;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD s, IDirect3DBaseTexture9* t) override {
        if (glitch::Enabled() && s == 0) {  // (glitch detection) who recorded this binding
            uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
            DWORD tid = GetCurrentThreadId();
            Record(65, [d = real_, s, r = Keep(t), caller, tid](const uint8_t*) {
                glitch::NoteTexSetter(caller, tid);
                d->SetTexture(s, r);
            });
            return D3D_OK;
        }
        Record(65, [d = real_, s, r = Keep(t)](const uint8_t*) { d->SetTexture(s, r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) override {
        Record(100, [d = real_, n, r = Keep(vb), off, stride](const uint8_t*) {
            d->SetStreamSource(n, r, off, stride);
        });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT n, UINT div) override {
        Record(102, [d = real_, n, div](const uint8_t*) { d->SetStreamSourceFreq(n, div); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* ib) override {
        Record(104, [d = real_, r = Keep(ib)](const uint8_t*) { d->SetIndices(r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* s) override {
        Record(92, [d = real_, r = Keep(s)](const uint8_t*) { d->SetVertexShader(r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* s) override {
        Record(107, [d = real_, r = Keep(s)](const uint8_t*) { d->SetPixelShader(r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* v) override {
        Record(87, [d = real_, r = Keep(v)](const uint8_t*) { d->SetVertexDeclaration(r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override {
        Record(89, [d = real_, fvf](const uint8_t*) { d->SetFVF(fvf); });
        return D3D_OK;
    }

#define TSCFIX_RT_CONSTANTS(Name, Slot, T, Width)                                                        \
    HRESULT STDMETHODCALLTYPE Name(UINT start, const T* data, UINT count) override {                    \
        if (!data) {                                                                                     \
            BeforeSync(Slot);                                                                            \
            return real_->Name(start, data, count);                                                      \
        }                                                                                                \
        Record(Slot, [d = real_, start, count](const uint8_t* p) {                                      \
            d->Name(start, reinterpret_cast<const T*>(p), count); }, data, sizeof(T) * (Width) * count); \
        return D3D_OK;                                                                                   \
    }
    TSCFIX_RT_CONSTANTS(SetVertexShaderConstantF, 94, float, 4)
    TSCFIX_RT_CONSTANTS(SetVertexShaderConstantI, 96, int, 4)
    TSCFIX_RT_CONSTANTS(SetVertexShaderConstantB, 98, BOOL, 1)
    TSCFIX_RT_CONSTANTS(SetPixelShaderConstantF, 109, float, 4)
    TSCFIX_RT_CONSTANTS(SetPixelShaderConstantI, 111, int, 4)
    TSCFIX_RT_CONSTANTS(SetPixelShaderConstantB, 113, BOOL, 1)
#undef TSCFIX_RT_CONSTANTS

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override {
        if (!m) { BeforeSync(44); return real_->SetTransform(s, m); }
        int x = XfIndex(s);
        if (x >= 0 && OnMainThread()) {
            if (BlockRecording()) block_.xf[x] = *m, block_.xfKnown[x] = block_.xfIn[x] = true;
            else S().xf[x] = *m, S().xfKnown[x] = S().gen;
        }
        Record(44, [d = real_, s, m = *m](const uint8_t*) { d->SetTransform(s, &m); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* v) override {
        experiments::FlushShadowClear();
        if (v && experiments::Tracing())
            Log("trace:   SetViewport %lu,%lu %lux%lu", v->X, v->Y, v->Width, v->Height);
        if (!v) { BeforeSync(47); return real_->SetViewport(v); }
        if (OnMainThread()) {
            if (BlockRecording()) block_.viewport = *v, block_.viewportKnown = block_.viewportIn = true;
            else S().viewport = *v, S().viewportKnown = S().gen;
        }
        Record(47, [d = real_, v = *v](const uint8_t*) { d->SetViewport(&v); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) override {
        TrySettle();
        int x = XfIndex(s);
        if (m && x >= 0 && S().xfKnown[x] == S().gen) return *m = S().xf[x], D3D_OK;
        BeforeSync(45);
        HRESULT hr = real_->GetTransform(s, m);
        if (SUCCEEDED(hr) && m && x >= 0 && OnMainThread()) S().xf[x] = *m, S().xfKnown[x] = S().gen;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override {
        int x = XfIndex(s);
        if (x >= 0 && OnMainThread()) {
            if (BlockRecording()) block_.xfIn[x] = true, block_.xfKnown[x] = false;
            else S().xfKnown[x] = 0;
        }
        BeforeSync(46);
        return real_->MultiplyTransform(s, m);
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* v) override {
        TrySettle();
        if (v && S().viewportKnown == S().gen) return *v = S().viewport, D3D_OK;
        BeforeSync(48);
        HRESULT hr = real_->GetViewport(v);
        if (SUCCEEDED(hr) && v) S().viewport = *v, S().viewportKnown = S().gen;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* r) override {
        if (!r) { BeforeSync(75); return real_->SetScissorRect(r); }
        Record(75, [d = real_, r = *r](const uint8_t*) { d->SetScissorRect(&r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float* p) override {
        if (!p) { BeforeSync(55); return real_->SetClipPlane(i, p); }
        Record(55, [d = real_, i](const uint8_t* q) { d->SetClipPlane(i, reinterpret_cast<const float*>(q)); },
               p, 4 * sizeof(float));
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* m) override {
        if (!m) { BeforeSync(49); return real_->SetMaterial(m); }
        Record(49, [d = real_, m = *m](const uint8_t*) { d->SetMaterial(&m); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* l) override {
        if (!l) { BeforeSync(51); return real_->SetLight(i, l); }
        Record(51, [d = real_, i, l = *l](const uint8_t*) { d->SetLight(i, &l); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL on) override {
        Record(53, [d = real_, i, on](const uint8_t*) { d->LightEnable(i, on); });
        return D3D_OK;
    }
    // A new state block holds the device state "now". The runtime is behind the game
    // thread, so create the object right away (the runtime lock makes that safe) and
    // record a Capture: it runs in stream order and stores the right state.
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) override {
        if (Role() >= 1 && out) {
            // Side buffer: the state "now" is the worker's end state, unknown yet. Create the
            // object right away and record the Capture; no snapshot (Apply invalidates).
            HRESULT hr = real_->CreateStateBlock(type, out);
            if (SUCCEEDED(hr) && *out) {
                IDirect3DStateBlock9* sb = Keep(*out);
                {
                    TableLock lock(this);
                    Snapshot& snap = snapshots_[sb];
                    snap = Snapshot{};
                    snap.type = type;
                }
                Record(59, [sb](const uint8_t*) { sb->Capture(); });
            }
            return hr;
        }
        if (!IsRecording() || !out) {
            BeforeSync(59);
            HRESULT hr = real_->CreateStateBlock(type, out);
            if (SUCCEEDED(hr) && out && *out && OnMainThread()) {
                TableLock lock(this);
                snapshots_[*out].type = type;
            }
            return hr;
        }
        HRESULT hr = real_->CreateStateBlock(type, out);
        if (SUCCEEDED(hr) && *out) {
            IDirect3DStateBlock9* sb = Keep(*out);
            {
                TableLock lock(this);
                snapshots_[sb].type = type;
                if (type == D3DSBT_ALL) snapshots_[sb].full = true;
                SnapshotShadowLocked(sb);
            }
            Record(59, [sb](const uint8_t*) { sb->Capture(); });
        }
        return hr;
    }

    // State block contents, as the game thread's shadow state saw them at Capture time:
    // the recorded Capture runs at the same point in the stream, so it stores exactly this.
    struct Snapshot {
        D3DSTATEBLOCKTYPE type = static_cast<D3DSTATEBLOCKTYPE>(0);  // 0 = unknown
        bool full = false;  // D3DSBT_ALL: covers every state we shadow
        bool valid = false;
        D3DVIEWPORT9 viewport{}; bool viewportKnown = false;
        DWORD rs[256]{};         bool rsKnown[256]{};
        DWORD tss[8][33]{};      bool tssKnown[8][33]{};
        // Recorded with BeginStateBlock / EndStateBlock: holds exactly the states set in between
        // (the *In flags); Apply changes only those.
        bool recorded = false;
        bool viewportIn = false, rsIn[256]{}, tssIn[8][33]{};
        bool touchesSamplers = false;
        D3DMATRIX xf[11]{};      bool xfKnown[11]{}, xfIn[11]{};
    };
    std::unordered_map<IDirect3DStateBlock9*, Snapshot> snapshots_;
    Snapshot block_;              // the block being recorded (game thread)
    bool blockRecording_ = false;
    bool BlockRecording() const { return blockRecording_ && Role() == 0; }

 public:
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        BeforeSync(60);
        HRESULT hr = real_->BeginStateBlock();
        if (SUCCEEDED(hr) && Role() == 0) {
            block_ = Snapshot{};
            block_.recorded = block_.valid = true;
            blockRecording_ = true;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** out) override {
        BeforeSync(61);  // the recorded Sets reach the block first
        HRESULT hr = real_->EndStateBlock(out);
        if (blockRecording_ && Role() == 0) {
            blockRecording_ = false;
            if (SUCCEEDED(hr) && out && *out) {
                TableLock lock(this);
                snapshots_[*out] = block_;
            }
        }
        return hr;
    }
 private:

    void SnapshotShadowLocked(IDirect3DStateBlock9* sb) {  // table lock held
        auto it = snapshots_.find(sb);
        if (it == snapshots_.end()) return;
        if (it->second.recorded) {
            Snapshot& r = it->second;
            if (r.viewportIn) r.viewport = S().viewport, r.viewportKnown = S().viewportKnown == S().gen;
            for (int i = 0; i < 11; ++i)
                if (r.xfIn[i]) r.xf[i] = S().xf[i], r.xfKnown[i] = S().xfKnown[i] == S().gen;
            for (int i = 0; i < 256; ++i)
                if (r.rsIn[i]) r.rs[i] = S().rs[i], r.rsKnown[i] = S().rsKnown[i] == S().gen;
            for (int t = 0; t < 8; ++t)
                for (int k = 0; k < 33; ++k)
                    if (r.tssIn[t][k]) r.tss[t][k] = S().tss[t][k], r.tssKnown[t][k] = S().tssKnown[t][k] == S().gen;
            return;
        }
        if (!it->second.full) return;
        Snapshot& s = it->second;
        s.valid = true;
        s.viewport = S().viewport, s.viewportKnown = S().viewportKnown == S().gen;
        for (int i = 0; i < 11; ++i) s.xf[i] = S().xf[i], s.xfKnown[i] = S().xfKnown[i] == S().gen;
        for (int i = 0; i < 256; ++i) s.rs[i] = S().rs[i], s.rsKnown[i] = S().rsKnown[i] == S().gen;
        for (int t = 0; t < 8; ++t)
            for (int k = 0; k < 33; ++k) s.tss[t][k] = S().tss[t][k], s.tssKnown[t][k] = S().tssKnown[t][k] == S().gen;
    }

 public:
    void OnStateBlockCapture(IDirect3DStateBlock9* sb) {
        TableLock lock(this);
        if (Role() >= 1) {  // captures the worker's end state: unknown here
            auto it = snapshots_.find(sb);
            if (it != snapshots_.end() && it->second.recorded) {
                Snapshot& r = it->second;
                r.viewportKnown = false;
                for (auto& k : r.xfKnown) k = false;
                for (auto& k : r.rsKnown) k = false;
                for (auto& row : r.tssKnown) for (auto& k : row) k = false;
            } else if (it != snapshots_.end()) {
                it->second.valid = false;
            }
            return;
        }
        if (IsRecording()) SnapshotShadowLocked(sb);
    }
    // Returns true if the shadow state could be restored from the block's snapshot.
    bool OnStateBlockApply(IDirect3DStateBlock9* sb) {
        TableLock lock(this);
        if (Role() >= 0) {
            auto it = snapshots_.find(sb);
            if (it != snapshots_.end() && it->second.recorded) {
                const Snapshot& r = it->second;
                if (Role() >= 1) {  // the join merges only known entries: an unknown one must forget all
                    bool complete = !r.viewportIn || r.viewportKnown;
                    for (int i = 0; i < 11 && complete; ++i) complete = !r.xfIn[i] || r.xfKnown[i];
                    for (int i = 0; i < 256 && complete; ++i) complete = !r.rsIn[i] || r.rsKnown[i];
                    for (int t = 0; t < 8 && complete; ++t)
                        for (int k = 0; k < 33 && complete; ++k) complete = !r.tssIn[t][k] || r.tssKnown[t][k];
                    if (!complete) return ++sbSide_, false;
                }
                ++sbRecorded_;
                Shadow& d = S();
                if (r.touchesSamplers)
                    for (auto& row : d.sampKnown)
                        for (auto& k : row) k = 0;
                if (r.viewportIn) {
                    if (r.viewportKnown) d.viewport = r.viewport, d.viewportKnown = d.gen;
                    else d.viewportKnown = 0;
                }
                for (int i = 0; i < 11; ++i)
                    if (r.xfIn[i]) {
                        if (r.xfKnown[i]) d.xf[i] = r.xf[i], d.xfKnown[i] = d.gen;
                        else d.xfKnown[i] = 0;
                    }
                for (int i = 0; i < 256; ++i)
                    if (r.rsIn[i]) {
                        if (r.rsKnown[i]) d.rs[i] = r.rs[i], d.rsKnown[i] = d.gen;
                        else d.rsKnown[i] = 0;
                    }
                for (int t = 0; t < 8; ++t)
                    for (int k = 0; k < 33; ++k)
                        if (r.tssIn[t][k]) {
                            if (r.tssKnown[t][k]) d.tss[t][k] = r.tss[t][k], d.tssKnown[t][k] = d.gen;
                            else d.tssKnown[t][k] = 0;
                        }
                return true;
            }
        }
        if (Role() >= 1) {
            auto it = snapshots_.find(sb);
            if (it != snapshots_.end() &&
                (it->second.type == D3DSBT_PIXELSTATE || it->second.type == D3DSBT_VERTEXSTATE)) {
                InvalidateEffectState(false);
                return true;
            }
            ++sbSide_;
            return false;  // the caller forgets all shadowed state
        }
        if (!IsRecording()) return false;
        auto it = snapshots_.find(sb);
        if (it == snapshots_.end()) return ++sbUntracked_, false;
        if (it->second.type == D3DSBT_PIXELSTATE || it->second.type == D3DSBT_VERTEXSTATE) {
            InvalidateEffectState(false);  // these never contain the viewport
            return true;
        }
        if (!it->second.valid) return ++(it->second.full ? sbInvalidFull_ : sbPartial_), false;
        const Snapshot& s = it->second;
        ++sbRestored_;
        ++S().gen;  // start clean; known entries come from the snapshot
        if (s.viewportKnown) S().viewport = s.viewport, S().viewportKnown = S().gen;
        for (int i = 0; i < 11; ++i)
            if (s.xfKnown[i]) S().xf[i] = s.xf[i], S().xfKnown[i] = S().gen;
        for (int i = 0; i < 256; ++i)
            if (s.rsKnown[i]) S().rs[i] = s.rs[i], S().rsKnown[i] = S().gen;
        for (int t = 0; t < 8; ++t)
            for (int k = 0; k < 33; ++k)
                if (s.tssKnown[t][k]) S().tss[t][k] = s.tss[t][k], S().tssKnown[t][k] = S().gen;
        return true;
    }

    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL sw) override {
        Record(77, [d = real_, sw](const uint8_t*) { d->SetSoftwareVertexProcessing(sw); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        // Asked every frame; only consult the runtime once Present has reported trouble.
        if (mode_ == Mode::Threaded && lastPresent_.load(std::memory_order_relaxed) == D3D_OK) return D3D_OK;
        BeforeSync(3);
        return real_->TestCooperativeLevel();
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float n) override {
        if (OnMainThread()) S().npatch = n, S().npatchKnown = S().gen;
        Record(79, [d = real_, n](const uint8_t*) { d->SetNPatchMode(n); });
        return D3D_OK;
    }
    float STDMETHODCALLTYPE GetNPatchMode() override {
        if (S().npatchKnown == S().gen) return S().npatch;
        BeforeSync(80);
        S().npatch = real_->GetNPatchMode(), S().npatchKnown = S().gen;
        return S().npatch;
    }

    // --- render targets ----------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9* s) override {
        experiments::FlushShadowClear();
        if (experiments::Tracing()) {
            D3DSURFACE_DESC d{};
            if (s) s->GetDesc(&d);
            Log("trace:   SetRenderTarget %lu %p %ux%u fmt=%d", i, s, d.Width, d.Height, d.Format);
        }
        if (i >= 4 || (i == 0 && !s)) { BeforeSync(37); return real_->SetRenderTarget(i, s); }
        if (OnMainThread()) SetShadowRT(i, s);
        if (i == 0 && OnMainThread()) {  // D3D9 resets the viewport to the full target
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(s->GetDesc(&desc))) {
                S().viewport = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
                S().viewportKnown = S().gen;
            }
        }
        IDirect3DSurface9* kept = Keep(s);
        if (s && IsRecording()) RememberDesc(s);
        Record(37, [d = real_, i, r = kept](const uint8_t*) { d->SetRenderTarget(i, r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i, IDirect3DSurface9** out) override {
        TrySettle();
        if (out && i < 4 && S().rtKnown[i]) {
            *out = S().rt[i];
            if (!S().rt[i]) return D3DERR_NOTFOUND;
            // Lend our own reference instead of AddRef: the engine's matching Release is then
            // answered by SurfaceRelease without taking the runtime lock (see TakeLoan).
            if (g_descCache && OnMainThread()) ++S().loans[i];
            else S().rt[i]->AddRef();
            return D3D_OK;
        }
        BeforeSync(38);
        return real_->GetRenderTarget(i, out);
    }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* s) override {
        experiments::FlushShadowClear();
        if (experiments::Tracing()) Log("trace:   SetDepthStencilSurface %p", s);
        if (OnMainThread()) SetShadowDS(s);
        Record(39, [d = real_, r = Keep(s)](const uint8_t*) { d->SetDepthStencilSurface(r); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** out) override {
        TrySettle();
        if (out && S().dsKnown) {
            *out = S().ds;
            if (!S().ds) return D3DERR_NOTFOUND;
            S().ds->AddRef();
            return D3D_OK;
        }
        BeforeSync(40);
        return real_->GetDepthStencilSurface(out);
    }

    // --- drawing --------------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        Record(41, [d = real_](const uint8_t*) { d->BeginScene(); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE EndScene() override {
        Record(42, [d = real_](const uint8_t*) { d->EndScene(); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Clear(DWORD n, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD st) override {
        if (experiments::Tracing()) Log("trace:   Clear rects=%lu flags=0x%lx color=0x%08lx z=%.2f", n, flags, c, z);
        if (mode_ != Mode::Threaded && experiments::HoldShadowClear(real_, n, flags, c, z, st)) return D3D_OK;
        if (experiments::NotMyShadowMap()) return D3D_OK;
        Record(43, [d = real_, n, has = rects != nullptr, flags, c, z, st](const uint8_t* p) {
            d->Clear(n, has ? reinterpret_cast<const D3DRECT*>(p) : nullptr, flags, c, z, st);
        }, rects, rects ? n * sizeof(D3DRECT) : 0);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count) override {
        if (experiments::NotMyShadowMap()) return D3D_OK;
        Record(81, [d = real_, t, start, count](const uint8_t*) { d->DrawPrimitive(t, start, count); });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT verts, UINT start,
                                                   UINT count) override {
        if (experiments::NotMyShadowMap()) return D3D_OK;
        if (t_drawSites) NoteDrawSite();
        if (glitch::Enabled()) {  // (glitch detection) who recorded this draw
            uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
            DWORD tid = GetCurrentThreadId();
            static thread_local uint64_t seq = 0;  // this thread's draws in recording order
            uint64_t n = ++seq;
            Record(82, [d = real_, t, base, minIdx, verts, start, count, caller, tid, n](const uint8_t*) {
                glitch::NoteRecorder(caller, tid, n);
                d->DrawIndexedPrimitive(t, base, minIdx, verts, start, count);
            });
            return D3D_OK;
        }
        Record(82, [d = real_, t, base, minIdx, verts, start, count](const uint8_t*) {
            d->DrawIndexedPrimitive(t, base, minIdx, verts, start, count);
        });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) override {
        if (experiments::NotMyShadowMap()) return D3D_OK;
        size_t bytes = size_t(VertexCount(t, count)) * stride;
        if (!data || !bytes || bytes > (1u << 20)) { BeforeSync(83); return real_->DrawPrimitiveUP(t, count, data, stride); }
        Record(83, [d = real_, t, count, stride](const uint8_t* p) { d->DrawPrimitiveUP(t, count, p, stride); },
               data, bytes);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minIdx, UINT verts, UINT count,
                                                     const void* idx, D3DFORMAT fmt, const void* data,
                                                     UINT stride) override {
        if (experiments::NotMyShadowMap()) return D3D_OK;
        size_t ib = size_t(VertexCount(t, count)) * (fmt == D3DFMT_INDEX32 ? 4 : 2);
        size_t vb = size_t(minIdx + verts) * stride;
        if (!idx || !data || !ib || ib + vb > (1u << 20)) {
            BeforeSync(84);
            return real_->DrawIndexedPrimitiveUP(t, minIdx, verts, count, idx, fmt, data, stride);
        }
        // Copy both arrays into one block: [indices][vertices].
        uint8_t* tmp = static_cast<uint8_t*>(_alloca(ib + vb));
        memcpy(tmp, idx, ib);
        memcpy(tmp + ib, data, vb);
        Record(84, [d = real_, t, minIdx, verts, count, fmt, stride, ib](const uint8_t* p) {
            d->DrawIndexedPrimitiveUP(t, minIdx, verts, count, p, fmt, p + ib, stride);
        }, tmp, ib + vb);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                          const RECT* dr, D3DTEXTUREFILTERTYPE f) override {
        struct Rects { RECT s, d; bool hs, hd; } r{};
        if (sr) r.s = *sr, r.hs = true;
        if (dr) r.d = *dr, r.hd = true;
        Record(34, [d = real_, a = Keep(src), b = Keep(dst), r, f](const uint8_t*) {
            d->StretchRect(a, r.hs ? &r.s : nullptr, b, r.hd ? &r.d : nullptr, f);
        });
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) override {
        Record(31, [d = real_, a = Keep(src), b = Keep(dst)](const uint8_t*) {
            d->UpdateTexture(a, b);
        });
        return D3D_OK;
    }

    // --- frame boundary --------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Present(const RECT* sr, const RECT* dr, HWND w, const RGNDATA* dirty) override {
        if (Role() >= 1) JoinOverlap(kSyncOverlap);
        experiments::FlushShadowClear();
        if (OnMainThread()) {
            framelog::OnFrame();
            TSCFIX_TRACE(trace::kFrame, trace::kFramePresent);
            ab::OnFrame();  // may switch modes; runs before this frame is submitted
            experiments::OnFrame();
            lookup::OnFrame();
            shadowcull::OnFrame();
            profiler::OnPresent();
            physicsfix::OnFrame();
            scripttiming::OnFrame();
            overlap::OnFrame();
            glitch::OracleOnFrame();
        }
        if (mode_ != Mode::Threaded || sr || dr || dirty) {
            BeforeSync(17);
            HRESULT hr = real_->Present(sr, dr, w, dirty);
            static DWORD lastTick = GetTickCount();
            static unsigned perSecond = 0;
            ++perSecond;
            if (GetTickCount() - lastTick >= 1000) {
                Log("rt: %u presents in the last second", perSecond);
                perSecond = 0;
                lastTick = GetTickCount();
            }
            static int logged = 0;
            if (logged < 8 || (FAILED(hr) && logged < 30)) {
                ++logged;
                Log("rt: Present src=%s dst=%s window=%p dirty=%p -> 0x%08lx", sr ? "rect" : "-", dr ? "rect" : "-", w,
                    dirty, hr);
                if (sr) Log("rt:   src rect %ld,%ld-%ld,%ld", sr->left, sr->top, sr->right, sr->bottom);
                if (dr) Log("rt:   dst rect %ld,%ld-%ld,%ld", dr->left, dr->top, dr->right, dr->bottom);
            }
            return hr;
        }
        uint64_t frame = ++framesSubmitted_;
        Record(17, [this, d = real_, w, frame](const uint8_t*) {
            HRESULT hr = d->Present(nullptr, nullptr, w, nullptr);
            lastPresent_.store(hr, std::memory_order_relaxed);
            framesPresented_.store(frame, std::memory_order_release);
        });
        // At most one frame in flight: wait until the previous frame has been presented.
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        while (framesPresented_.load(std::memory_order_acquire) + 1 < frame) _mm_pause();
        QueryPerformanceCounter(&t1);
        paceWaitTicks_ += t1.QuadPart - t0.QuadPart;
        if (frame % 16 == 0) SweepPins(false);
        ReportStats(frame);
        return lastPresent_.load(std::memory_order_relaxed);
    }

    // The back buffer surface stays the same object until Reset: cache it instead of draining.
    IDirect3DSurface9* backBuffer_ = nullptr;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT chain, UINT index, D3DBACKBUFFER_TYPE type,
                                            IDirect3DSurface9** out) override {
        bool main = chain == 0 && index == 0 && type == D3DBACKBUFFER_TYPE_MONO && out;
        if (main && backBuffer_) {
            backBuffer_->AddRef();
            *out = backBuffer_;
            return D3D_OK;
        }
        BeforeSync(18);
        HRESULT hr = real_->GetBackBuffer(chain, index, type, out);
        if (SUCCEEDED(hr) && main && *out && !backBuffer_) {
            backBuffer_ = *out;
            backBuffer_->AddRef();
        }
        return hr;
    }

    // The Steam overlay asks for swap chain 0 every frame: the object stays the same until Reset.
    IDirect3DSwapChain9* swapChain_ = nullptr;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT chain, IDirect3DSwapChain9** out) override {
        if (chain == 0 && out && swapChain_) {
            swapChain_->AddRef();
            *out = swapChain_;
            return D3D_OK;
        }
        BeforeSync(14);
        HRESULT hr = real_->GetSwapChain(chain, out);
        if (SUCCEEDED(hr) && chain == 0 && out && *out && !swapChain_) {
            swapChain_ = *out;
            swapChain_->AddRef();
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override {
        if (Role() >= 1) JoinOverlap(kSyncOverlap);
        BeforeSync(16);
        if (backBuffer_) backBuffer_->Release(), backBuffer_ = nullptr;
        if (swapChain_) swapChain_->Release(), swapChain_ = nullptr;
        InvalidateShadowState();
        ReleaseShadowSurfaces();  // Reset fails while default-pool surfaces are referenced
        SweepPins(true);
        ParkDropBlock();
        HRESULT hr = real_->Reset(pp);
        InitShadow();
        return hr;
    }

    // --- shadow state ------------------------------------------------------------------------------
    uint32_t invalAll_ = 0, sbRestored_ = 0, sbUnknown_ = 0;
    uint32_t sbSide_ = 0, sbUntracked_ = 0, sbInvalidFull_ = 0, sbPartial_ = 0, sbRecorded_ = 0;
    // Per-call counters, per recording role (index role + 1) on their own cache lines: up to four
    // threads record at once, and one shared counter line would bounce between their cores.
    struct alignas(64) RoleCount {
        uint32_t setRs, redundantRs, setSamp, redundantSamp, invalEffect, sidePushes;
    };
    RoleCount rc_[33] = {};  // roles -1 .. kSegs - 1
    RoleCount& Rc(int role) { return rc_[role + 1]; }
    uint32_t SumRc(uint32_t RoleCount::*f) const {
        uint32_t n = 0;
        for (const RoleCount& c : rc_) n += c.*f;
        return n;
    }
    void ClearRc(uint32_t RoleCount::*f) {
        for (RoleCount& c : rc_) c.*f = 0;
    }

    void InvalidateShadowState() {
        ++invalAll_;
        ++S().gen;
        if (Role() >= 1) S().invalAll = true;
        // Render targets are not affected by state blocks; keep them.
    }

    // Effect passes can set render, sampler and texture-stage states and shaders, but not
    // the viewport or render targets: forget only what they can change.
    // Transforms survive too: no effect file of the game or its sky middleware sets them.
    // Vertex / pixel state blocks pass keepTransforms = false.
    void InvalidateEffectState(bool keepTransforms = true) {
        int role = Role();
        ++Rc(role).invalEffect;
        Shadow& sh = sh_[role > 0 ? role : 0];  // S(), with the role looked up once
        if (role >= 1) sh.invalEffect = true;
        bool vpKnown = sh.viewportKnown == sh.gen;
        bool npKnown = sh.npatchKnown == sh.gen;
        uint32_t oldGen = sh.gen;
        ++sh.gen;
        if (vpKnown) sh.viewportKnown = sh.gen;
        if (npKnown) sh.npatchKnown = sh.gen;
        if (keepTransforms)
            for (auto& k : sh.xfKnown)
                if (k == oldGen) k = sh.gen;
    }

    void SyncForDirect(int reason) {
        if (OnMainThread()) BeforeSync(reason);
    }

    bool IsRecording() const { return mode_ == Mode::Threaded && OnMainThread(); }
    bool OwnsQueue() const { return mode_ == Mode::Threaded && Role() == 0; }

    uint64_t QueueHead() const { return ring_.Head(); }
    bool QueueReached(uint64_t pos) const { return ring_.Tail() >= pos; }

    // Live switch between recording for the render thread and direct calls (A/B tests).
    // Called on the game thread at a frame boundary.
    void SetThreadedActive(bool on) {
        if (!thread_) return;
        if (!on && mode_ == Mode::Threaded) {
            BeforeSync(17);       // everything recorded so far reaches the runtime first
            mode_ = Mode::PassThrough;
            SweepPins(true);      // nothing is in flight any more
        } else if (on) {
            mode_ = Mode::Threaded;
        }
        status::Set("render", mode_ == Mode::Threaded ? "render thread ON" : "render thread OFF");
    }

    // --- overlap: two recording threads ---------------------------------------------------------
    // The game thread hands the queue to `worker` and records into the side buffer from now on,
    // with a shadow state that knows nothing yet. EndOverlap (game thread, worker idle) appends
    // the side buffer to the queue and takes the queue back.
    bool BeginOverlap(DWORD worker) {
        if (mode_ != Mode::Threaded || Role() != 0 || overlapping_) return false;
        for (int k = 1; k < kSegs; ++k) sh_[k] = Shadow{}, seg_[k].Reset(), seg_[k].id_ = k;
        order_[0] = 1;
        orderCount_ = 1;
        joinThread_ = CurrentTid();
        ++overlapGen_;
        overlapping_ = true;
        SetSeg(1, joinThread_, 1);
        mainThread_.store(worker, std::memory_order_release);
        RolesChanged();
        return true;
    }

    // Called by a side recorder: `worker` records the next part of the stream from here, and
    // the caller continues after it. Its segment is split into [caller so far][worker][caller
    // continued]; the worker starts with the caller's known state, the continuation with none.
    bool ForkSegment(DWORD worker) {
        int a = Role();
        if (!overlapping_ || a < 1) return false;
        // The segment order is also changed by the overlap's worker when it forks the second shadow
        // thread (ForkAhead), at about the same moment the game thread forks the scenery here.
        // Without the lock both picked free segments and shifted the order at once: in some frames
        // the parts of the frame were replayed in a wrong order (a lamp mast at Amersfoort drawn
        // lit for one frame, 3 to 6 times in 40 frames; runs flick-four, flick-forkold).
        SrwExclusive lock(&orderLock_);
        int b = 0, c = 0;
        for (int k = 1; k < kSegs && !c; ++k) {
            bool used = false;
            for (int i = 0; i < orderCount_; ++i) used |= order_[i] == k;
            if (!used) (b ? c : b) = k;
        }
        if (!c) return false;
        int at = 0;
        while (order_[at] != a) ++at;
        for (int i = orderCount_ - 1; i > at; --i) order_[i + 2] = order_[i];
        order_[at + 1] = b, order_[at + 2] = c;
        orderCount_ += 2;
        CopyKnownState(sh_[a], sh_[b]);
        sh_[c] = Shadow{};
        seg_[b].Reset(), seg_[c].Reset();
        SetSeg(b, worker, 2);
        SetSeg(c, CurrentTid(), 3);
        SetSeg(a, 0, 4);
        RolesChanged();
        ++forks_;
        return true;
    }
    // Called by a side recorder that keeps recording: `worker` gets the next free slot after
    // the caller's segment and any slots reserved before it (in reservation order). The worker
    // starts with the caller's known state as of now. ResumeAfterForks moves the caller behind
    // all of them.
    bool ForkAhead(DWORD worker, bool fresh) {
        int a = Role();
        if (!overlapping_ || a < 0) return false;
        SrwExclusive lock(&orderLock_);
        if (t_forkGen != overlapGen_) t_ahead = 0, t_cont = 0, t_forkGen = overlapGen_;
        // The first fork of a group also reserves the segment the caller continues in (placed
        // right after the caller), so ResumeAfterForks cannot fail once a fork exists.
        if (!t_ahead) {
            int c = FreeSegment();
            if (!c) return false;
            int at0 = a == 0 ? 0 : PositionOf(a) + 1;
            for (int i = orderCount_ - 1; i >= at0; --i) order_[i + 1] = order_[i];
            order_[at0] = c;
            ++orderCount_;
            sh_[c] = Shadow{};
            seg_[c].Reset();
            forkOwner_[c] = 0;
            t_cont = c;
        }
        int b = FreeSegment();
        if (!b) {
            if (!t_ahead) {  // give the reservation back
                int at0 = PositionOf(t_cont);
                for (int i = at0; i < orderCount_ - 1; ++i) order_[i] = order_[i + 1];
                --orderCount_;
                t_cont = 0;
            }
            return false;
        }
        // After the caller's segment and the forks it made so far (before its continuation); the
        // queue holder (the overlap's worker, role 0) records ahead of every side segment.
        int at = a == 0 ? 0 : PositionOf(a) + 1;
        at += t_ahead;
        if (at < 0 || at > orderCount_ || orderCount_ + 1 > kSegs) {  // never: the bookkeeping is off
            Log("rt: fork refused: position %d of %d (caller %d, ahead %d)", at, orderCount_, a, t_ahead);
            return false;
        }
        for (int i = orderCount_ - 1; i >= at; --i) order_[i + 1] = order_[i];
        order_[at] = b;
        ++orderCount_;
        ++t_ahead;
        if (fresh) sh_[b] = Shadow{};
        else CopyKnownState(sh_[a], sh_[b]);
        seg_[b].Reset();
        forkOwner_[b] = CurrentTid();
        SetSeg(b, worker, 5);
        RolesChanged();
        ++forks_;
        return true;
    }
    bool ResumeAfterForks() {
        int a = Role();
        if (!overlapping_ || a < 0 || !t_ahead || !t_cont || t_forkGen != overlapGen_) {
            static volatile long logged = 0;
            if (t_ahead && InterlockedIncrement(&logged) <= 5)
                Log("rt: fork group dropped at resume (overlapping %d, role %d, ahead %d, cont %d, stale %d)",
                    overlapping_ ? 1 : 0, a, t_ahead, t_cont, t_forkGen != overlapGen_ ? 1 : 0);
            t_ahead = 0, t_cont = 0;  // nothing to resume into: forget the group
            return false;
        }
        SrwExclusive lock(&orderLock_);
        int c = t_cont;  // reserved by the first fork, after all of them
        t_ahead = 0;
        t_cont = 0;
        sh_[c] = Shadow{};  // what the workers leave behind is unknown until they are done
        settleFrom_[c] = a;
        settlePending_[c].store(true, std::memory_order_release);
        settlesPending_.fetch_add(1, std::memory_order_relaxed);
        seg_[c].Reset();
        forkOwner_[c] = 0;
        SetSeg(c, CurrentTid(), 6);
        if (a == 0) mainThread_.store(0, std::memory_order_release);  // the queue waits for the join
        else SetSeg(a, 0, 7);
        RolesChanged();
        return true;
    }
    int PositionOf(int seg) const {
        for (int i = 0; i < orderCount_; ++i)
            if (order_[i] == seg) return i;
        return -1;
    }
    int FreeSegment() const {
        for (int k = 1; k < kSegs; ++k) {
            bool used = false;
            for (int i = 0; i < orderCount_; ++i) used |= order_[i] == k;
            if (!used) return k;
        }
        return 0;
    }
    // Order changes come from the game thread and the overlap's worker (both fork): every one of
    // them holds this lock.
    struct SrwExclusive {
        SRWLOCK* l;
        explicit SrwExclusive(SRWLOCK* x) : l(x) { AcquireSRWLockExclusive(l); }
        ~SrwExclusive() { ReleaseSRWLockExclusive(l); }
    };
    SRWLOCK orderLock_ = SRWLOCK_INIT;
    DWORD forkOwner_[32] = {};  // by segment: the thread that forked it (kSegs, declared below)

    // The caller's segment behind forked workers starts with whatever they leave behind. Once
    // they are all done, that is known: the caller's segment before the forks (or the queue's
    // state, segment 0), then each forked segment in stream order. Getters call this so a query
    // after the forks is answered from the shadow instead of draining the queue (which also ends
    // the overlap early).
    int settleFrom_[32] = {};  // by segment (kSegs, declared below)
    std::atomic<bool> settlePending_[32] = {};
    std::atomic<int> settlesPending_{0};
    __forceinline void TrySettle() {
        if (settlesPending_.load(std::memory_order_relaxed)) SettleForks();
    }
    void SettleForks() {
        int c = Role();
        if (c < 1 || !settlePending_[c].load(std::memory_order_acquire)) return;
        SrwExclusive lock(&orderLock_);
        int a = settleFrom_[c];
        int at = a == 0 ? -1 : PositionOf(a), ct = PositionOf(c);
        if (ct < 0) return;
        for (int i = at + 1; i < ct; ++i)
            if (segThread_[order_[i]].load(std::memory_order_acquire)) return;  // still recording
        Shadow start;
        CopyKnownState(sh_[a], start);
        bool all = sh_[a].invalAll, eff = sh_[a].invalEffect;
        for (int i = at + 1; i < ct; ++i) {
            const Shadow& f = sh_[order_[i]];
            Shadow t;
            CopyKnownState(f, t);
            t.invalAll = f.invalAll, t.invalEffect = f.invalEffect;
            all |= f.invalAll, eff |= f.invalEffect;
            MergeState(start, t);
        }
        all |= sh_[c].invalAll, eff |= sh_[c].invalEffect;
        MergeState(start, sh_[c]);
        // Still forgets for the merge into the main shadow what any of them forgot.
        start.invalAll = all, start.invalEffect = eff;
        sh_[c] = start;
        settleFrom_[c] = 0;
        settlePending_[c].store(false, std::memory_order_relaxed);
        settlesPending_.fetch_sub(1, std::memory_order_relaxed);
        ++settles_;
    }
    uint64_t settles_ = 0;
    // A segment the calling thread forked is still being recorded by its worker.
    bool OwnForksOutstanding() {
        DWORD me = CurrentTid();
        SrwExclusive lock(&orderLock_);
        for (int i = 0; i < orderCount_; ++i) {
            int k = order_[i];
            if (forkOwner_[k] == me && segThread_[k].load(std::memory_order_acquire)) return true;
        }
        return false;
    }

    // The forked worker is done recording (it never joins).
    void SegmentDone() {
        int r = Role();
        if (r >= 1 && CurrentTid() != joinThread_) {
            SetSeg(r, 0, 8);
            RolesChanged();
        }
    }

    // Known render, texture-stage, viewport, transform and n-patch state, as of `from`'s end.
    static void CopyKnownState(const Shadow& from, Shadow& to) {
        to = Shadow{};
        for (int i = 0; i < 256; ++i)
            if (from.rsKnown[i] == from.gen) to.rs[i] = from.rs[i], to.rsKnown[i] = to.gen;
        for (int t = 0; t < 8; ++t)
            for (int k = 0; k < 33; ++k)
                if (from.tssKnown[t][k] == from.gen) to.tss[t][k] = from.tss[t][k], to.tssKnown[t][k] = to.gen;
        if (from.viewportKnown == from.gen) to.viewport = from.viewport, to.viewportKnown = to.gen;
        for (int i = 0; i < 11; ++i)
            if (from.xfKnown[i] == from.gen) to.xf[i] = from.xf[i], to.xfKnown[i] = to.gen;
        if (from.npatchKnown == from.gen) to.npatch = from.npatch, to.npatchKnown = to.gen;
        for (int i = 0; i < 16; ++i)
            for (int k = 0; k < 14; ++k)
                if (from.sampKnown[i][k] == from.gen) to.samp[i][k] = from.samp[i][k], to.sampKnown[i][k] = to.gen;
        // Render targets and depth buffer: the copy holds its own references.
        for (int i = 0; i < 4; ++i)
            if (from.rtKnown[i]) {
                to.rt[i] = from.rt[i], to.rtKnown[i] = true, to.loans[i] = 0;
                to.rtDesc[i] = from.rtDesc[i], to.rtDescValid[i] = from.rtDescValid[i];
                if (to.rt[i]) to.rt[i]->AddRef();
            }
        if (from.dsKnown) {
            to.ds = from.ds, to.dsKnown = true;
            if (to.ds) to.ds->AddRef();
        }
    }

    // `q` ran after everything `p` knows: what it set wins, what it did not touch keeps p's
    // value, unless it ran something that forgot state. q's references move to p.
    // References it drops are released through the recorded stream (ReleaseLater): a direct
    // Release waits for the runtime lock the replay thread holds (2.6% of the render hop thread,
    // profRender1).
    void MergeState(Shadow& p, Shadow& q) {
        if (q.invalAll) {
            ++p.gen;
        } else if (q.invalEffect) {
            D3DVIEWPORT9 vp = p.viewport;
            bool vpKnown = p.viewportKnown == p.gen;
            float np = p.npatch;
            bool npKnown = p.npatchKnown == p.gen;
            for (auto& k : p.xfKnown)
                if (k == p.gen) k = p.gen + 1;
            ++p.gen;
            if (vpKnown) p.viewport = vp, p.viewportKnown = p.gen;
            if (npKnown) p.npatch = np, p.npatchKnown = p.gen;
        }
        for (int i = 0; i < 256; ++i)
            if (q.rsKnown[i] == q.gen) p.rs[i] = q.rs[i], p.rsKnown[i] = p.gen;
        for (int t = 0; t < 8; ++t)
            for (int k = 0; k < 33; ++k)
                if (q.tssKnown[t][k] == q.gen) p.tss[t][k] = q.tss[t][k], p.tssKnown[t][k] = p.gen;
        if (q.viewportKnown == q.gen) p.viewport = q.viewport, p.viewportKnown = p.gen;
        for (int i = 0; i < 11; ++i)
            if (q.xfKnown[i] == q.gen) p.xf[i] = q.xf[i], p.xfKnown[i] = p.gen;
        if (q.npatchKnown == q.gen) p.npatch = q.npatch, p.npatchKnown = p.gen;
        for (int i = 0; i < 16; ++i)
            for (int k = 0; k < 14; ++k)
                if (q.sampKnown[i][k] == q.gen) p.samp[i][k] = q.samp[i][k], p.sampKnown[i][k] = p.gen;
        for (int i = 0; i < 4; ++i) {
            if (!q.rtKnown[i]) continue;
            if (p.rt[i]) {
                for (; p.loans[i]; --p.loans[i]) p.rt[i]->AddRef();
                ReleaseLater(p.rt[i]);
            }
            p.rt[i] = q.rt[i], p.rtKnown[i] = true, p.loans[i] = q.loans[i];
            p.rtDesc[i] = q.rtDesc[i], p.rtDescValid[i] = q.rtDescValid[i];
        }
        if (q.dsKnown) {
            if (p.ds) ReleaseLater(p.ds);
            p.ds = q.ds, p.dsKnown = true;
        }
        q = Shadow{};
    }

    // The thread that began the overlap, with every forked worker done: the segments go into
    // the queue in stream order and that thread takes the queue back.
    void EndOverlap() {
        if (!overlapping_ || CurrentTid() != joinThread_) return;
        // Forked workers may still be recording (the join can come from inside the joining
        // thread's own ring pass): their segments are handed over only when complete. They
        // never wait for this thread.
        for (int spins = 0;; ++spins) {
            int busy = 0;
            for (int k = 1; k < kSegs && !busy; ++k) {
                DWORD t = segThread_[k].load(std::memory_order_acquire);
                if (t && t != joinThread_) busy = k;
            }
            if (!busy) break;
            if (spins == 0) {
                static volatile long logged = 0;
                if (InterlockedIncrement(&logged) <= 5) Log("rt: join waits for forked segment %d", busy);
            }
            if (spins > 64) Sleep(0);
            else YieldProcessor();
        }
        for (auto& p : settlePending_) p.store(false, std::memory_order_relaxed);
        settlesPending_.store(0, std::memory_order_relaxed);
        for (int& f : settleFrom_) f = 0;
        for (DWORD& o : forkOwner_) o = 0;
        for (int i = 0; i < orderCount_; ++i) MergeState(sh_[0], sh_[order_[i]]);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < orderCount_; ++i) sideBytes_ += seg_[order_[i]].HandTo(ring_);
        if (g_sideAppendedHook) g_sideAppendedHook(ring_.Head());
        QueryPerformanceCounter(&t1);
        appendTicks_ += t1.QuadPart - t0.QuadPart;
        ++overlaps_;
        mainThread_.store(CurrentTid(), std::memory_order_release);
        for (int k = 0; k < kSegs; ++k) SetSeg(k, 0, 9);
        RolesChanged();
        orderCount_ = 0;
        overlapping_ = false;
    }
    // A side segment needs the queue (a call that must run directly): wait for the workers
    // through the registered hook, which ends the overlap.
    void JoinOverlap(int slot) {
        ++joinsBySlot_[slot < 256 ? slot : 255];
        if (g_joinHook) g_joinHook();
        if (CurrentTid() == joinThread_) EndOverlap();  // no hook: the workers must already be idle
    }
    bool Overlapping() const { return overlapping_; }
    Ring& Queue() { return ring_; }
    // History of segment owner changes (crash report): thread, segment, old -> new, site.
    struct SegChange { DWORD tid, old, now; int seg, site; };
    SegChange segLog_[64] = {};
    std::atomic<uint32_t> segLogAt_{0};
    void SetSeg(int k, DWORD v, int site) {
        DWORD old = segThread_[k].exchange(v, std::memory_order_acq_rel);
        if (old == v) return;
        uint32_t i = segLogAt_.fetch_add(1, std::memory_order_relaxed) & 63;
        segLog_[i] = {CurrentTid(), old, v, k, site};
    }
    void LogThreadRole() {
        uint32_t end = segLogAt_.load();
        for (uint32_t i = end > 32 ? end - 32 : 0; i < end; ++i) {
            const SegChange& c = segLog_[i & 63];
            Log("crash: seg change: thread %lu seg %d %lu -> %lu (site %d)", c.tid, c.seg, c.old, c.now, c.site);
        }
        char order[160];
        int n = 0;
        for (int i = 0; i < orderCount_ && n < 150; ++i)
            n += sprintf_s(order + n, sizeof order - n, " %d:%lu", order_[i], segThread_[order_[i]].load());
        order[n] = 0;
        Log("crash: rt role %d, queue holder %lu, overlapping %d, join thread %lu, order%s", Role(),
            mainThread_.load(), overlapping_ ? 1 : 0, joinThread_, order);
    }
    bool IsThreaded() const { return mode_ == Mode::Threaded; }

    // Effects: header blob + optional payload, copied once into the ring. Callers check
    // Recording() first, so this always records.
    void RecordRaw(void (*exec)(const uint8_t*), const void* blob, size_t size, const void* payload, size_t payloadSize) {
        int role = Role();
        ++t_records;
        if (t_slotCounts) ++t_slotCounts[255];
        if (role >= 1) seg_[role].Push([exec](const uint8_t* p) { exec(p); }, blob, size, payload, payloadSize);
        else ring_.Push([exec](const uint8_t* p) { exec(p); }, blob, size, payload, payloadSize);
    }

 private:
    void InitShadow() {
        for (DWORD i = 0; i < 4; ++i) {
            IDirect3DSurface9* s = nullptr;
            real_->GetRenderTarget(i, &s);
            SetShadowRT(i, s);
            if (s) s->Release();
        }
        IDirect3DSurface9* ds = nullptr;
        real_->GetDepthStencilSurface(&ds);
        SetShadowDS(ds);
        if (ds) ds->Release();
    }
    // Outstanding loans become real references before we drop our own.
    void SettleLoans(int i) {
        for (; S().loans[i]; --S().loans[i]) S().rt[i]->AddRef();
    }
    void ReleaseShadowSurfaces() {
        for (int i = 0; i < 4; ++i) {
            if (S().rt[i]) SettleLoans(i), S().rt[i]->Release();
            S().rt[i] = nullptr;
            S().rtKnown[i] = false;
        }
        if (S().ds) S().ds->Release();
        S().ds = nullptr;
        S().dsKnown = false;
    }
    // Shadow render targets hold a reference so a pointer handed out stays valid.
    // Releases a surface the shadow state no longer refers to on the render thread, after the
    // commands recorded so far: a texture level's Release takes the runtime lock, which the
    // render thread holds while it works (recording threads stalled on it ~1 ms/frame).
    void ReleaseLater(IUnknown* obj) {
        int role = mode_ == Mode::Threaded ? Role() : -1;
        auto f = [obj](const uint8_t*) { obj->Release(); };
        if (role >= 1) seg_[role].Push(f);
        else if (role == 0) ring_.Push(f);
        else obj->Release();
    }
    void SetShadowRT(DWORD i, IDirect3DSurface9* s) {
        if (s) s->AddRef();
        if (S().rt[i]) SettleLoans(i), ReleaseLater(S().rt[i]);
        S().rt[i] = s;
        S().rtKnown[i] = true;
        S().rtDescValid[i] = false;
        if (s) {
            HookSurfaceMethods(s);
            d3d::HookSurface(s);
            // Through the object's own vtable: surface classes differ (back buffer vs texture
            // level), and S().rtDescValid[i] is false here, so this reaches the right original.
            S().rtDescValid[i] = SUCCEEDED(s->GetDesc(&S().rtDesc[i]));
        }
    }

 public:
    // Answers GetDesc for surfaces we keep alive as render targets (immutable data), so the
    // engine's per-frame GetRenderTarget/GetDesc pattern does not wait on the runtime lock.
    bool CachedDesc(IDirect3DSurface9* s, D3DSURFACE_DESC* out) {
        if (!OnMainThread()) return false;
        const Shadow& sh = S();
        for (int i = 0; i < 4; ++i)
            if (sh.rt[i] == s && sh.rtDescValid[i]) return *out = sh.rtDesc[i], true;
        TableLock lock(this);
        for (size_t i = 0; i < descCount_; ++i)
            if (descs_[i].s == s) return *out = descs_[i].d, true;
        return false;
    }

    // A Release on the main thread of a render target we lent out (GetRenderTarget without
    // AddRef) returns the loan. Which Release is matched does not matter: only the count does,
    // and our own reference keeps the surface alive while loans are outstanding.
    bool TakeLoan(IDirect3DSurface9* s) {
        if (!OnMainThread()) return false;
        for (int i = 0; i < 4; ++i)
            if (S().rt[i] == s && S().loans[i]) return --S().loans[i], true;
        return false;
    }

    // Surfaces bound as render targets are pinned (alive); remember their description
    // until the pin is dropped.
    void RememberDesc(IDirect3DSurface9* s) {
        TableLock lock(this);
        for (size_t i = 0; i < descCount_; ++i)
            if (descs_[i].s == s) return;
        if (descCount_ == std::size(descs_)) return;
        D3DSURFACE_DESC d;
        if (SUCCEEDED(s->GetDesc(&d))) descs_[descCount_++] = {s, d};
    }
    void ForgetDesc(IUnknown* obj) {
        for (size_t i = 0; i < descCount_; ++i)
            if (static_cast<IUnknown*>(descs_[i].s) == obj) {
                descs_[i] = descs_[--descCount_];
                return;
            }
    }

    using SurfaceGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DSURFACE_DESC*);
    using SurfaceReleaseFn = ULONG(STDMETHODCALLTYPE*)(IDirect3DSurface9*);

 private:
    // Surface classes (back buffer, texture level, ...) have their own vtables; each patched
    // vtable remembers its originals. Entries are only ever added.
    struct SurfaceVtable {
        std::atomic<void**> vt{nullptr};
        SurfaceGetDescFn getDesc = nullptr;
        SurfaceReleaseFn release = nullptr;
    };
    static inline SurfaceVtable g_surfaceVtables[8];

    static const SurfaceVtable& Originals(IDirect3DSurface9* s) {
        void** vt = *reinterpret_cast<void***>(s);
        for (auto& e : g_surfaceVtables)
            if (e.vt.load(std::memory_order_acquire) == vt) return e;
        return g_surfaceVtables[0];  // unreachable: only patched vtables call the hooks
    }
    static HRESULT STDMETHODCALLTYPE SurfaceGetDesc(IDirect3DSurface9* s, D3DSURFACE_DESC* out) {
        if (out && g_descCache && g_device && g_device->CachedDesc(s, out)) return D3D_OK;
        return Originals(s).getDesc(s, out);
    }
    static ULONG STDMETHODCALLTYPE SurfaceRelease(IDirect3DSurface9* s) {
        if (g_device && g_device->TakeLoan(s)) return 1;
        return Originals(s).release(s);
    }
    // Recording threads (SetShadowRT); serialized, since overlap workers record too.
    static inline SRWLOCK g_surfaceHookLock = SRWLOCK_INIT;
    static void HookSurfaceMethods(IDirect3DSurface9* s) {
        constexpr int kRelease = 2, kGetDesc = 12;  // IUnknown(3), IDirect3DResource9(8), GetContainer, GetDesc
        void** vt = *reinterpret_cast<void***>(s);
        if (vt[kGetDesc] == reinterpret_cast<void*>(&SurfaceGetDesc)) return;
        AcquireSRWLockExclusive(&g_surfaceHookLock);
        struct Unlock {
            ~Unlock() { ReleaseSRWLockExclusive(&g_surfaceHookLock); }
        } unlock;
        if (vt[kGetDesc] == reinterpret_cast<void*>(&SurfaceGetDesc)) return;
        SurfaceVtable* slot = nullptr;
        for (auto& e : g_surfaceVtables)
            if (!e.vt.load(std::memory_order_relaxed)) { slot = &e; break; }
        if (!slot) return;
        DWORD old;
        if (VirtualProtect(vt, (kGetDesc + 1) * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            slot->getDesc = reinterpret_cast<SurfaceGetDescFn>(vt[kGetDesc]);
            slot->release = reinterpret_cast<SurfaceReleaseFn>(vt[kRelease]);
            slot->vt.store(vt, std::memory_order_release);  // published before the patch below
            vt[kGetDesc] = reinterpret_cast<void*>(&SurfaceGetDesc);
            vt[kRelease] = reinterpret_cast<void*>(&SurfaceRelease);
            VirtualProtect(vt, (kGetDesc + 1) * sizeof(void*), old, &old);
            Log("rt: surface GetDesc/Release hooked on vtable %p", static_cast<void*>(vt));
        }
    }
    void SetShadowDS(IDirect3DSurface9* s) {
        if (s) s->AddRef();
        if (S().ds) ReleaseLater(S().ds);  // a direct Release waits for the runtime lock (profShadow1: 3%)
        S().ds = s;
        S().dsKnown = true;
    }

    void ReportStats(uint64_t frame) {
        if (frame % 600 != 0) return;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double perFrameMs = 1000.0 / f.QuadPart / 600.0;
        int top[3] = {-1, -1, -1};
        for (int s = 0; s < 256; ++s)
            for (int k = 0; k < 3; ++k)
                if (top[k] < 0 || syncsBySlot_[s] > syncsBySlot_[top[k]]) {
                    for (int m = 2; m > k; --m) top[m] = top[m - 1];
                    top[k] = s;
                    break;
                }
        auto name = [](int s) {
            return s < 119 ? kRtDeviceMethodNames[s] : s == kSyncLock ? "resource lock"
                 : s == kSyncStateBlock ? "state block" : s == kSyncEffect ? "effect"
                 : s == kSyncQuery ? "query" : "?";
        };
        Log("rt: last 600 frames: syncs/frame %.1f (%s %.1f, %s %.1f, %s %.1f) sync wait %.2f ms/frame, "
            "pacing wait %.2f ms/frame, render thread idle %.2f ms/frame", syncs_ / 600.0, name(top[0]), syncsBySlot_[top[0]] / 600.0, name(top[1]),
            syncsBySlot_[top[1]] / 600.0, name(top[2]), syncsBySlot_[top[2]] / 600.0, syncWaitTicks_ * perFrameMs,
            paceWaitTicks_ * perFrameMs, idleTicks_.exchange(0, std::memory_order_relaxed) * perFrameMs);
        Log("rt:   shadow invalidations/frame: all %.1f (state block without snapshot %.1f), effect %.1f, "
            "state block restored %.1f, recorded block applied %.1f; unknown: side %.1f, untracked %.1f, full but invalid %.1f, partial type %.1f",
            invalAll_ / 600.0, sbUnknown_ / 600.0, SumRc(&RoleCount::invalEffect) / 600.0, sbRestored_ / 600.0, sbRecorded_ / 600.0, sbSide_ / 600.0,
            sbUntracked_ / 600.0, sbInvalidFull_ / 600.0, sbPartial_ / 600.0);
        {  // every wait reason: count and wait per frame
            static const char* kKinds[] = {"VB", "IB", "texture", "surface"};
            static const char* kCls[] = {"plain", "read-only", "no-overwrite", "discard"};
            char line[1536];
            int n = 0;
            for (int s = 0; s < 256 && n < 1400; ++s) {
                if (!syncsBySlot_[s]) continue;
                char nm[48];
                if (s >= kSyncLockBase && s < kSyncLockBase + 16)
                    sprintf_s(nm, "lock %s %s", kKinds[(s - kSyncLockBase) / 4], kCls[(s - kSyncLockBase) % 4]);
                else
                    sprintf_s(nm, "%s", name(s));
                n += sprintf_s(line + n, sizeof line - n, " %s %.1f/%.2fms;", nm, syncsBySlot_[s] / 600.0,
                               syncWaitBySlot_[s] * perFrameMs);
            }
            Log("rt:   waits per frame (count/ms):%s", n ? line : " none");
            Log("rt:   state sets/frame: render %.0f (%.0f redundant), sampler %.0f (%.0f redundant)%s",
                (SumRc(&RoleCount::setRs) + (g_filterStates ? SumRc(&RoleCount::redundantRs) : 0)) / 600.0,
                SumRc(&RoleCount::redundantRs) / 600.0,
                (SumRc(&RoleCount::setSamp) + (g_filterStates ? SumRc(&RoleCount::redundantSamp) : 0)) / 600.0,
                SumRc(&RoleCount::redundantSamp) / 600.0,
                g_filterStates ? ", redundant ones dropped" : "");
            for (auto f : {&RoleCount::setRs, &RoleCount::redundantRs, &RoleCount::setSamp, &RoleCount::redundantSamp})
                ClearRc(f);
            if (forks_) {
                n = 0;
                for (int s2 = 0; s2 < 256 && n < 1400; ++s2)
                    if (forkedSyncs_[s2]) n += sprintf_s(line + n, sizeof line - n, " %s %u;", name(s2), forkedSyncs_[s2]);
                Log("rt:   forks %.2f/frame, state settled after forks %.2f/frame; calls a forked worker made out of order:%s"
                    " (%u of them with the replay thread running)",
                    forks_ / 600.0, settles_ / 600.0, n ? line : " none", forkedUnparked_);
                forkedUnparked_ = 0;
                settles_ = 0;
                forks_ = 0;
                for (auto& c : forkedSyncs_) c = 0;
            }
            for (auto& w : syncWaitBySlot_) w = 0;
        }
        if (overlaps_) {
            int jt[3] = {-1, -1, -1};
            for (int sl = 0; sl < 256; ++sl)
                for (int k = 0; k < 3; ++k)
                    if (jt[k] < 0 || joinsBySlot_[sl] > joinsBySlot_[jt[k]]) {
                        for (int m = 2; m > k; --m) jt[m] = jt[m - 1];
                        jt[k] = sl;
                        break;
                    }
            auto jname = [&](int sl) { return sl == kSyncOverlap ? "overlap-only call" : name(sl); };
            Log("rt:   overlap joins by call: %s %llu, %s %llu, %s %llu", jname(jt[0]),
                static_cast<unsigned long long>(joinsBySlot_[jt[0]]), jname(jt[1]),
                static_cast<unsigned long long>(joinsBySlot_[jt[1]]), jname(jt[2]),
                static_cast<unsigned long long>(joinsBySlot_[jt[2]]));
            memset(joinsBySlot_, 0, sizeof joinsBySlot_);
        }
        invalAll_ = sbRestored_ = sbUnknown_ = 0;
        ClearRc(&RoleCount::invalEffect);
        sbSide_ = sbUntracked_ = sbInvalidFull_ = sbPartial_ = sbRecorded_ = 0;
        syncs_ = 0;
        syncWaitTicks_ = paceWaitTicks_ = 0;
        memset(syncsBySlot_, 0, sizeof syncsBySlot_);
    }

    static DWORD WINAPI RenderThreadMain(void* self) {
        auto* dev = static_cast<RtDevice*>(self);
        Ring& ring = dev->ring_;
        dev->renderThread_ = CurrentTid();
        g_renderTid = dev->renderThread_;
        while (!dev->stop_.load(std::memory_order_acquire)) {
            RenderPausePoint();
            if (ring.RunOne()) continue;
            LARGE_INTEGER i0, i1;
            QueryPerformanceCounter(&i0);
            int spins = 0;
            while (ring.Empty() && ++spins < 20000) _mm_pause();
            if (!ring.Empty()) continue;
            // Idle: sleep until the producer publishes more work.
            size_t seen = ring.Head();
            ring.consumerSleeping.store(true, std::memory_order_release);
            if (ring.Empty() && !dev->stop_.load()) WaitOnAddress(const_cast<size_t*>(ring.HeadAddress()), &seen, sizeof seen, 2);
            ring.consumerSleeping.store(false, std::memory_order_release);
            QueryPerformanceCounter(&i1);
            dev->idleTicks_.fetch_add(i1.QuadPart - i0.QuadPart, std::memory_order_relaxed);
        }
        while (ring.RunOne()) {
        }
        return 0;
    }

    Mode mode_;
    std::atomic<DWORD> mainThread_{0};
    // Overlap side segments 1..kSegs-1: their recording threads, buffers and replay order.
    static constexpr int kSegs = 32;
    static_assert(sizeof(settleFrom_) == kSegs * sizeof(int));
    std::atomic<DWORD> segThread_[kSegs] = {};
    int order_[kSegs] = {};
    int orderCount_ = 0;
    DWORD joinThread_ = 0;  // began the overlap; only it joins
    uint32_t overlapGen_ = 0;  // counts overlaps (per-thread fork bookkeeping from older ones is stale)
    uint32_t forks_ = 0;
    uint32_t forkedSyncs_[256] = {};
    uint32_t forkedUnparked_ = 0;  // of those, made while the replay thread was running
    volatile bool overlapping_ = false;
    SRWLOCK tableLock_ = SRWLOCK_INIT;  // pins_ and descs_ while two threads record
    struct TableLock {
        RtDevice* d;
        bool held;
        explicit TableLock(RtDevice* dev) : d(dev), held(dev->overlapping_) {
            if (held) AcquireSRWLockExclusive(&d->tableLock_);
        }
        ~TableLock() {
            if (held) ReleaseSRWLockExclusive(&d->tableLock_);
        }
    };
    SideBuffer seg_[kSegs];
    uint64_t overlaps_ = 0, sideBytes_ = 0, joinsBySlot_[256] = {};
    int64_t appendTicks_ = 0;
    std::atomic<DWORD> renderThread_{0};
    HANDLE thread_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<ULONG> refs_{1};
    Ring ring_;
    PinTable pins_;

    std::atomic<uint64_t> framesPresented_{0};
    uint64_t framesSubmitted_ = 0;
    std::atomic<HRESULT> lastPresent_{D3D_OK};

    // Shadow state (generation-stamped; ++gen forgets everything except render targets), one per
    // recording thread: [0] the queue's producer, [1] the game thread while it records into the
    // side buffer during an overlap (BeginOverlap .. EndOverlap).
    Shadow sh_[kSegs];
    struct DescEntry { IDirect3DSurface9* s; D3DSURFACE_DESC d; };
    DescEntry descs_[256] = {};
    size_t descCount_ = 0;

    uint64_t syncs_ = 0;
    int64_t syncWaitTicks_ = 0, paceWaitTicks_ = 0;
    std::atomic<int64_t> idleTicks_{0};  // render thread waiting for commands (between reports)
    uint32_t syncsBySlot_[256] = {};
    int64_t syncWaitBySlot_[256] = {};
};

}  // namespace

void SetSplitCores(bool on) { g_splitCores = on; }

void SetDescCacheActive(bool on) { g_descCache = on; }

void SetRecordingThread(DWORD tid) {
    if (g_device) g_device->SetRecorder(tid);
}

void SetThreadedActive(bool on) {
    if (g_device) g_device->SetThreadedActive(on);
}

bool BeginOverlap(DWORD worker) { return g_device && g_device->BeginOverlap(worker); }

void EndOverlap() {
    if (g_device) g_device->EndOverlap();
}

void SetOverlapJoinHook(void (*hook)()) { g_joinHook = hook; }
void SetOverlapWorkerDoneHook(bool (*hook)()) { g_workerDoneHook = hook; }
void SetSideAppendedHook(void (*hook)(uint64_t head)) { g_sideAppendedHook = hook; }
void SetFilterStates(bool on) { g_filterStates = on; }
void SetParkKeepsState(bool on) { g_parkKeepsState = on; }
long ParkSaves() { return InterlockedExchange(&g_parkSaves, 0); }
uint64_t QueueHead() { return g_device ? g_device->QueueHead() : 0; }
bool QueueReached(uint64_t pos) { return !g_device || g_device->QueueReached(pos); }

int RecorderRole() { return g_device && g_device->IsThreaded() ? g_device->Role() : -1; }

IDirect3DDevice9* Wrap(IDirect3DDevice9* real, Mode mode) {
    if (mode == Mode::Off || !real) return real;
    Log("rt: wrapping device (mode %d)", static_cast<int>(mode));
    g_device = new RtDevice(real, mode);
    g_parkDevice = real;
    return g_device;
}

void SyncForDirectAccess(int reason) {
    if (g_device) g_device->SyncForDirect(reason);
}

bool Recording() { return g_device && g_device->IsRecording(); }

bool OwnsQueue() { return g_device && g_device->OwnsQueue(); }
bool SideRecorder() { return g_device && g_device->IsRecording() && !g_device->OwnsQueue(); }
uint64_t g_exclusiveScopes = 0;
ExclusiveDevice::ExclusiveDevice() {
    if (!g_device || !SideRecorder()) return;
    AcquireSRWLockExclusive(&g_exclusiveLock);
    held_ = true;
    ++g_exclusiveScopes;
    g_pauseRequests.fetch_add(1, std::memory_order_acq_rel);
    Ring& ring = g_device->Queue();
    // Parked at a boundary, or asleep with nothing to run (it passes a pause point on waking).
    while (!g_renderPaused.load(std::memory_order_acquire) &&
           !(ring.consumerSleeping.load(std::memory_order_acquire) && ring.Empty()))
        YieldProcessor();
}
ExclusiveDevice::~ExclusiveDevice() {
    if (!held_) return;
    g_pauseRequests.fetch_sub(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_exclusiveLock);
}
// A thread beside the recorder (pipelined update) that must call D3DX or the device directly parks
// the render thread at a command boundary first. Nested per thread.
thread_local int t_pauseDepth = 0;
thread_local bool t_pauseHeld = false;  // the outermost pause got the lock
thread_local bool t_callPause = false;
thread_local bool t_pauseSaved = false;  // this pause saved the device state
DWORD RenderThreadId() { return g_renderTid; }
long PauseRequests() { return g_pauseRequests.load(); }
void PauseRenderThread() {
    if (!g_device || !g_renderTid || CurrentTid() == g_renderTid || t_pauseDepth++) return;  // (no render thread yet: nothing to park)
    // Another thread's pause may be held by a thread that waits for us (soakPipe20: render hop
    // paused + joining the worker, worker waiting for a lock the game thread held while it asked
    // for a pause): after 100 ms go on without one rather than deadlock.
    ULONGLONG t0 = GetTickCount64();
    while (!TryAcquireSRWLockExclusive(&g_exclusiveLock)) {
        YieldProcessor();
        if (GetTickCount64() - t0 > 100) {
            static volatile long logged = 0;
            if (InterlockedIncrement(&logged) <= 5) Log("rt: render pause not available within 100 ms (thread %lu); going on without", CurrentTid());
            t_pauseHeld = false;
            return;
        }
    }
    t_pauseHeld = true;
    g_pauseRequests.fetch_add(1, std::memory_order_acq_rel);
    Ring& ring = g_device->Queue();
    // The render thread may be blocked on something the caller holds (the runtime lock inside a
    // D3DX call): give up waiting after 100 ms rather than deadlock (the request stays until resume).
    ULONGLONG start = GetTickCount64();
    while (!g_renderPaused.load(std::memory_order_acquire) &&
           !(ring.consumerSleeping.load(std::memory_order_acquire) && ring.Empty())) {
        YieldProcessor();
        if (GetTickCount64() - start > 100) {
            static volatile long logged = 0;
            if (InterlockedIncrement(&logged) <= 5) Log("rt: render thread did not park within 100 ms (thread %lu)", CurrentTid());
            return;  // (not parked: the device is not ours to read)
        }
    }
    t_pauseSaved = true;
    ParkSaveState();
}
void ResumeRenderThread() {
    if (!g_device || !g_renderTid || CurrentTid() == g_renderTid || !t_pauseDepth || --t_pauseDepth) return;
    if (!t_pauseHeld) return;
    t_pauseHeld = false;
    if (t_pauseSaved) ParkRestoreState();
    t_pauseSaved = false;
    g_pauseRequests.fetch_sub(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_exclusiveLock);
}
void PauseForDirectCall() {
    if (t_callPause) return;
    t_callPause = true;
    PauseRenderThread();
}
void ResumeAfterDirectCalls() {
    if (!t_callPause) return;
    t_callPause = false;
    ResumeRenderThread();
}
namespace {  // (declared before RtDevice, in this unnamed namespace)
bool OwnPauseHeld() { return t_pauseDepth > 0 && t_pauseHeld; }
int ReleaseOwnPauseBeforeWait() {
    ResumeAfterDirectCalls();
    int depth = t_pauseDepth;
    if (!depth) return 0;
    t_pauseDepth = 1;
    ResumeRenderThread();  // releases the render thread (depth 1 -> 0)
    return depth;
}
void RetakeOwnPause(int depth) {
    if (!depth) return;
    PauseRenderThread();
    t_pauseDepth = depth;
}
}  // namespace
bool OverlapRunning() { return g_device && g_device->Overlapping(); }
void LogThreadRole() {
    if (g_device) g_device->LogThreadRole();
}
bool ForkSegment(DWORD worker) { return g_device && g_device->ForkSegment(worker); }
bool ForkAhead(DWORD worker, bool fresh) { return g_device && g_device->ForkAhead(worker, fresh); }
uint64_t ThreadRecords() { return t_records; }
void CountSlots(uint32_t* counts) { t_slotCounts = counts; }
void SetDirectCaller(bool on) { t_direct = on; }
bool DirectCaller() { return t_direct && RecorderRole() < 0; }
void CountDrawSites(void* sites, int cap) { t_drawSites = static_cast<DrawSite*>(sites); t_drawSiteCap = cap; }
const char* SlotName(int s) { return s < 119 ? kRtDeviceMethodNames[s] : s == 255 ? "raw (generated)" : "?"; }
bool ResumeAfterForks() { return g_device && g_device->ResumeAfterForks(); }
void SegmentDone() {
    if (g_device) g_device->SegmentDone();
}

void AddSaveRestoreSite(const char* module, uintptr_t returnOffset) {
    if (g_saveSiteCount == 16) return;
    auto& v = g_saveSites[g_saveSiteCount++];
    strncpy_s(v.module, module, _TRUNCATE);
    v.ret = returnOffset;
    v.base = 0;
}


void Enqueue(void (*exec)(const uint8_t*), const void* blob, size_t size, const void* payload, size_t payloadSize) {
    g_device->RecordRaw(exec, blob, size, payload, payloadSize);
}

void KeepAlive(IUnknown* obj) {
    if (g_device && obj) g_device->Keep(obj);
}

void InvalidateShadow() {
    if (g_device) g_device->InvalidateShadowState();
}

void InvalidateEffectState() {
    if (g_device) g_device->InvalidateEffectState();
}

void StateBlockCaptured(IDirect3DStateBlock9* sb) {
    if (g_device) g_device->OnStateBlockCapture(sb);
}

void StateBlockApplied(IDirect3DStateBlock9* sb) {
    if (g_device && !g_device->OnStateBlockApply(sb)) {
        ++g_device->sbUnknown_;
        g_device->InvalidateShadowState();
    }
}

}  // namespace tscfix::rt
