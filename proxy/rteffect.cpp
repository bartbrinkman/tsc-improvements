#include <intrin.h>
#include "rteffect.h"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "glitch.h"
#include "log.h"
#include "rtdevice.h"
#include "overlap.h"

namespace tscfix::rt {
namespace {

using Handle = const char*;  // D3DXHANDLE
struct Matrix { float m[16]; };
struct Vector4 { float v[4]; };

// ID3DXEffect slots (d3dx9_43; names in d3dnames.h).
enum Slot {
    kAddRef = 1, kRelease = 2, kSetValue = 20, kSetBool = 22, kGetBool = 23, kSetInt = 26, kSetFloat = 30, kSetFloatArray = 32,
    kSetVector = 34, kSetVectorArray = 36, kSetMatrix = 38, kSetMatrixArray = 40, kSetMatrixTranspose = 44,
    kSetTexture = 52, kGetPool = 57, kSetTechnique = 58, kIsParameterUsed = 62, kBegin = 63, kBeginPass = 64,
    kCommitChanges = 65, kEndPass = 66, kEnd = 67, kGetDevice = 68, kSlots = 79,
};

// Read-only metadata lookups: safe to run directly while the render thread uses the effect.
bool IsMetadata(int slot) {
    return (slot >= 3 && slot <= 19) || slot == kGetPool || slot == kIsParameterUsed || slot == kGetDevice;
}

struct Wrapper {
    void** vtbl;                 // our vtable
    void* real;                  // the D3DX effect
    std::atomic<ULONG> refs{1};
    // Current technique (for the pass-count cache), per recording thread: during an overlap
    // the game thread and a worker may drive the same effect (their calls run in stream order).
    Handle technique[3] = {};  // [2]: the second shadow thread
    // Unique per wrapper: a thread's remembered pass count must not survive the effect (a new
    // effect can get a freed one's address; t4-three-trim crashed in D3DX on that).
    uint32_t serial = NextSerial();
    static uint32_t NextSerial() {
        static std::atomic<uint32_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    SRWLOCK lock = SRWLOCK_INIT;  // passes, bools
    std::unordered_map<Handle, UINT> passes;
    // Bool parameters as last set through this wrapper (or read once): GetBool answers from
    // here instead of draining the queue to ask the effect.
    std::unordered_map<Handle, BOOL> bools;
    // The first few bool parameters live in a small table read and updated without the lock
    // (an effect has a handful; every SetBool used to take the lock and a hash lookup). A slot
    // is published by its handle, written last; value -1: not known.
    struct BoolSlot { volatile Handle h; volatile LONG v; };
    BoolSlot boolSlots[8] = {};
};
// Pass counts each recording thread looked up last, per effect (several threads share a role
// index, so this is per thread, not per role).
struct PassSeen { const Wrapper* w; uint32_t serial; Handle tech; UINT count; };
thread_local PassSeen t_passSeen[16];

// The parameter's slot, or null when it is not in the table (and `add` is off or it is full).
Wrapper::BoolSlot* BoolSlotOf(Wrapper* w, Handle h, bool add) {
    for (auto& s : w->boolSlots) {
        if (s.h == h) return &s;
        if (!s.h) {
            if (!add) return nullptr;
            AcquireSRWLockExclusive(&w->lock);
            Wrapper::BoolSlot* got = nullptr;
            for (auto& t : w->boolSlots) {
                if (t.h == h) { got = &t; break; }
                if (!t.h) { t.v = -1, t.h = h, got = &t; break; }
            }
            ReleaseSRWLockExclusive(&w->lock);
            return got;
        }
    }
    return nullptr;
}

void* g_vtbl[kSlots];
uint8_t* g_thunks = nullptr;

template <typename Fn>
Fn RealMethod(const Wrapper* w, int slot) {
    return reinterpret_cast<Fn>((*static_cast<void***>(w->real))[slot]);
}

// (measurement) drains by effect method: the draining thunks' start addresses map a return
// address back to its slot; the explicit DrainForEffect() calls count as slot kSlots.
uint8_t* g_thunkStart[80] = {};
volatile long g_drainsBySlot[81] = {};
// (measurement) effect calls from a thread beside the recorder (pipelined update), by slot.
volatile long g_directBySlot[82] = {};
void CountDirect(int slot) {
    static volatile long total = 0;
    InterlockedIncrement(&g_directBySlot[slot]);
    long n = InterlockedIncrement(&total);
    if (n == 1 || n % 500 == 0) {
        char line[512];
        int k = 0;
        for (int i = 0; i < 82 && k < 480; ++i)
            if (g_directBySlot[i]) k += sprintf_s(line + k, sizeof line - k, " %d:%ld", i, g_directBySlot[i]);
        Log("fx: effect calls beside the recorder by slot (80 explicit drain, 81 metadata):%s", line);
    }
}
__declspec(noinline) void DrainForEffect() {
    auto ret = static_cast<uint8_t*>(_ReturnAddress());
    int slot = 80;
    for (int i = 0; i < 79; ++i)
        if (g_thunkStart[i] && ret >= g_thunkStart[i] && ret < g_thunkStart[i] + 128) { slot = i; break; }
    if (DirectCaller()) {
        // Pipelined update: D3DX is not safe beside the render thread replaying effect calls.
        CountDirect(slot);
        PauseForDirectCall();
        return;
    }
    static volatile long total = 0;
    InterlockedIncrement(&g_drainsBySlot[slot]);
    if (InterlockedIncrement(&total) % 2000 == 0) {
        char line[512];
        int n = 0;
        for (int i = 0; i < 81 && n < 480; ++i)
            if (g_drainsBySlot[i]) n += sprintf_s(line + n, sizeof line - n, " %d:%ld", i, g_drainsBySlot[i]);
        Log("fx: effect drains by slot (80 = explicit):%s", line);
    }
    SyncForDirectAccess(kSyncEffect);
}
// The second shadow thread drives the same effects as the first at the same time, with its own
// techniques: it has its own slot (a shared one handed each thread the other's pass count).
int Role() { return overlap::OnSplitWorker() ? 2 : RecorderRole() == 1 ? 1 : 0; }

// D3DX lookups by name are not safe on one effect from two threads at once; while an overlap
// runs (the game thread and a worker both record), metadata calls take this lock.
CRITICAL_SECTION g_metaLock;
thread_local int t_metaHeld = 0;  // entered (1) or not (0) per nesting level, as a bit stack
thread_local int t_metaPaused = 0;  // bit stack like t_metaHeld: the render thread was parked
void MetaLock() {
    t_metaPaused <<= 1;
    if (DirectCaller()) {
        CountDirect(81);
        PauseRenderThread();
        t_metaPaused |= 1;
    }
    t_metaHeld <<= 1;
    if (OverlapRunning()) {
        EnterCriticalSection(&g_metaLock);
        t_metaHeld |= 1;
    }
}
void MetaUnlock() {
    if (t_metaHeld & 1) LeaveCriticalSection(&g_metaLock);
    t_metaHeld >>= 1;
    if (t_metaPaused & 1) ResumeRenderThread();
    t_metaPaused >>= 1;
}

// Locked metadata thunk (up to three arguments after this, none on the stack):
//   sub rsp,0x48; save rcx,rdx,r8,r9; call MetaLock; reload; call real method; save rax;
//   call MetaUnlock; restore rax; add rsp,0x48; ret
void* EmitLockedThunk(uint8_t*& p, int slot) {
    uint8_t* start = p;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    auto call = [&](void (*fn)()) {
        emit({0x48, 0xB8});  // mov rax, imm64
        auto a = reinterpret_cast<uint64_t>(fn);
        memcpy(p, &a, 8);
        p += 8;
        emit({0xFF, 0xD0});  // call rax
    };
    emit({0x48, 0x83, 0xEC, 0x48});        // sub rsp, 0x48
    emit({0x48, 0x89, 0x4C, 0x24, 0x28});  // mov [rsp+0x28], rcx
    emit({0x48, 0x89, 0x54, 0x24, 0x30});  // mov [rsp+0x30], rdx
    emit({0x4C, 0x89, 0x44, 0x24, 0x38});  // mov [rsp+0x38], r8
    emit({0x4C, 0x89, 0x4C, 0x24, 0x40});  // mov [rsp+0x40], r9
    call(&MetaLock);
    emit({0x48, 0x8B, 0x4C, 0x24, 0x28});  // mov rcx, [rsp+0x28]
    emit({0x48, 0x8B, 0x54, 0x24, 0x30});  // mov rdx, [rsp+0x30]
    emit({0x4C, 0x8B, 0x44, 0x24, 0x38});  // mov r8, [rsp+0x38]
    emit({0x4C, 0x8B, 0x4C, 0x24, 0x40});  // mov r9, [rsp+0x40]
    emit({0x48, 0x8B, 0x49, 0x08});        // mov rcx, [rcx+8]  (real effect)
    emit({0x48, 0x8B, 0x01});              // mov rax, [rcx]
    emit({0xFF, 0x90});                    // call [rax+disp32]
    int32_t disp = slot * 8;
    memcpy(p, &disp, 4);
    p += 4;
    emit({0x48, 0x89, 0x44, 0x24, 0x28});  // mov [rsp+0x28], rax
    call(&MetaUnlock);
    emit({0x48, 0x8B, 0x44, 0x24, 0x28});  // mov rax, [rsp+0x28]
    emit({0x48, 0x83, 0xC4, 0x48});        // add rsp, 0x48
    emit({0xC3});                          // ret
    p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 15) & ~uintptr_t(15));
    return start;
}

// --- generic thunks ----------------------------------------------------------------------------
// drain variant: save argument registers, call DrainForEffect, restore, then
//   mov rcx, [rcx+8]   (this -> real effect)
//   mov rax, [rcx]     (real vtable)
//   jmp [rax + slot*8]
void* EmitThunk(uint8_t*& p, int slot, bool drain) {
    uint8_t* start = p;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    if (drain) {
        emit({0x51});                          // push rcx
        emit({0x52});                          // push rdx
        emit({0x41, 0x50});                    // push r8
        emit({0x41, 0x51});                    // push r9
        emit({0x48, 0x83, 0xEC, 0x58});        // sub rsp, 0x58 (shadow space + xmm saves, keeps alignment)
        emit({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x20});  // movdqu [rsp+0x20], xmm1
        emit({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x30});  // movdqu [rsp+0x30], xmm2
        emit({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x40});  // movdqu [rsp+0x40], xmm3
        emit({0x48, 0xB8});                    // mov rax, imm64
        auto fn = reinterpret_cast<uint64_t>(&DrainForEffect);
        memcpy(p, &fn, 8);
        p += 8;
        emit({0xFF, 0xD0});                    // call rax
        emit({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x20});  // movdqu xmm1, [rsp+0x20]
        emit({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x30});  // movdqu xmm2, [rsp+0x30]
        emit({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x40});  // movdqu xmm3, [rsp+0x40]
        emit({0x48, 0x83, 0xC4, 0x58});        // add rsp, 0x58
        emit({0x41, 0x59});                    // pop r9
        emit({0x41, 0x58});                    // pop r8
        emit({0x5A});                          // pop rdx
        emit({0x59});                          // pop rcx
    }
    emit({0x48, 0x8B, 0x49, 0x08});            // mov rcx, [rcx+8]
    emit({0x48, 0x8B, 0x01});                  // mov rax, [rcx]
    emit({0xFF, 0xA0});                        // jmp [rax+disp32]
    int32_t disp = slot * 8;
    memcpy(p, &disp, 4);
    p += 4;
    p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 15) & ~uintptr_t(15));
    return start;
}

// --- recording helpers --------------------------------------------------------------------------
// Each recorded call is a POD blob [header struct][optional payload] executed on the render thread.
template <typename Blob>
inline void Push(void (*exec)(const uint8_t*), const Blob& b, const void* extra = nullptr, size_t extraSize = 0) {
    Enqueue(exec, &b, sizeof b, extra, extraSize);
}

struct HV { void* fx; Handle h; uint32_t v; };          // SetBool / SetInt / SetFloat (bits)
struct HP { void* fx; Handle h; uint32_t n; };          // header for arrays / values / matrices
struct HT { void* fx; Handle h; IDirect3DBaseTexture9* t; };
struct HTG { void* fx; Handle h; IDirect3DBaseTexture9* t; uintptr_t caller; DWORD tid; uint64_t r14, rdi; };  // (glitch detection)

// (glitch detection) The caller's nonvolatile r14/rdi at its call into this wrapper (called
// directly from engine code): this frame's context, unwound once.
__declspec(noinline) void CallerRegs(uint64_t* r14, uint64_t* rdi) {
    CONTEXT c;
    RtlCaptureContext(&c);
    for (int frame = 0; frame < 2; ++frame) {  // out of this helper, then out of the wrapper
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fe) return;
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
    }
    *r14 = c.R14, *rdi = c.Rdi;
}
struct H0 { void* fx; Handle h; };
struct HU { void* fx; UINT u; };
struct HB { void* fx; UINT flags; UINT assumed; Handle tech; DWORD tid; };  // Begin (assumed pass count)
volatile long g_beginMismatches = 0;

using SetDWFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, uint32_t);
using SetFloatFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, float);
using SetPtrFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const void*);
using SetArrFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const void*, UINT);
using SetTexFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, IDirect3DBaseTexture9*);
using SetTechFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle);
using BeginFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT*, DWORD);
struct TechniqueDesc {  // D3DXTECHNIQUE_DESC
    const char* name;
    UINT passes;
    UINT annotations;
};
using GetTechniqueDescFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, TechniqueDesc*);
constexpr int kGetTechniqueDesc = 5;
using UIntFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT);
using VoidFn = HRESULT(STDMETHODCALLTYPE*)(void*);

inline void** VT(void* fx) { return *static_cast<void***>(fx); }

#define TSCFIX_DIRECT_IF_NOT_RECORDING(...) \
    if (!Recording()) {                     \
        DrainForEffect();                   \
        return __VA_ARGS__;                 \
    }

HRESULT STDMETHODCALLTYPE SetBool(Wrapper* w, Handle h, BOOL v) {
    if (Wrapper::BoolSlot* slot = BoolSlotOf(w, h, true)) {
        slot->v = v ? 1 : 0;
    } else {
        AcquireSRWLockExclusive(&w->lock);
        w->bools[h] = v;
        ReleaseSRWLockExclusive(&w->lock);
    }
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<SetDWFn>(w, kSetBool)(w->real, h, v))
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HV*>(p);
        reinterpret_cast<SetDWFn>(VT(b.fx)[kSetBool])(b.fx, b.h, b.v); }, HV{w->real, h, uint32_t(v)});
    return D3D_OK;
}
using GetBoolFn = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, BOOL*);
HRESULT STDMETHODCALLTYPE GetBool(Wrapper* w, Handle h, BOOL* v) {
    Wrapper::BoolSlot* slot = BoolSlotOf(w, h, false);
    if (v && slot && slot->v >= 0) return *v = slot->v, D3D_OK;
    if (v && !slot) {
        AcquireSRWLockShared(&w->lock);
        auto it = w->bools.find(h);
        bool known = it != w->bools.end();
        BOOL cached = known ? it->second : FALSE;
        ReleaseSRWLockShared(&w->lock);
        if (known) return *v = cached, D3D_OK;
    }
    DrainForEffect();
    HRESULT hr = RealMethod<GetBoolFn>(w, kGetBool)(w->real, h, v);
    if (SUCCEEDED(hr) && v) {
        if (Wrapper::BoolSlot* known = BoolSlotOf(w, h, true)) {
            known->v = *v ? 1 : 0;
        } else {
            AcquireSRWLockExclusive(&w->lock);
            w->bools.emplace(h, *v);
            ReleaseSRWLockExclusive(&w->lock);
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE SetInt(Wrapper* w, Handle h, INT v) {
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<SetDWFn>(w, kSetInt)(w->real, h, uint32_t(v)))
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HV*>(p);
        reinterpret_cast<SetDWFn>(VT(b.fx)[kSetInt])(b.fx, b.h, b.v); }, HV{w->real, h, uint32_t(v)});
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE SetFloat(Wrapper* w, Handle h, float v) {
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<SetFloatFn>(w, kSetFloat)(w->real, h, v))
    uint32_t bits;
    memcpy(&bits, &v, 4);
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HV*>(p); float f; memcpy(&f, &b.v, 4);
        reinterpret_cast<SetFloatFn>(VT(b.fx)[kSetFloat])(b.fx, b.h, f); }, HV{w->real, h, bits});
    return D3D_OK;
}

// Fixed-size pointer arguments (matrix, vector).
template <int SlotV, size_t Bytes>
HRESULT STDMETHODCALLTYPE SetFixed(Wrapper* w, Handle h, const void* data) {
    if (!Recording() || !data) {
        DrainForEffect();
        return RealMethod<SetPtrFn>(w, SlotV)(w->real, h, data);
    }
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HP*>(p);
        reinterpret_cast<SetPtrFn>(VT(b.fx)[SlotV])(b.fx, b.h, p + sizeof(HP)); },
         HP{w->real, h, 0}, data, Bytes);
    return D3D_OK;
}

// Arrays: (handle, data, count) with a fixed element size; SetValue passes bytes as count.
template <int SlotV, size_t Elem>
HRESULT STDMETHODCALLTYPE SetArray(Wrapper* w, Handle h, const void* data, UINT count) {
    size_t bytes = Elem * count;
    if (SlotV == kSetValue) {  // may set a bool parameter: forget its cached value
        if (Wrapper::BoolSlot* slot = BoolSlotOf(w, h, false)) {
            slot->v = -1;
        } else {
            AcquireSRWLockExclusive(&w->lock);
            w->bools.erase(h);
            ReleaseSRWLockExclusive(&w->lock);
        }
    }
    if (!Recording() || !data || bytes > 4096) {
        DrainForEffect();
        return RealMethod<SetArrFn>(w, SlotV)(w->real, h, data, count);
    }
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HP*>(p);
        reinterpret_cast<SetArrFn>(VT(b.fx)[SlotV])(b.fx, b.h, p + sizeof(HP), b.n); },
         HP{w->real, h, count}, data, bytes);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SetTexture(Wrapper* w, Handle h, IDirect3DBaseTexture9* t) {
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<SetTexFn>(w, kSetTexture)(w->real, h, t))
    KeepAlive(t);
    if (glitch::Enabled()) {  // who recorded this texture parameter
        HTG g{w->real, h, t, reinterpret_cast<uintptr_t>(_ReturnAddress()), GetCurrentThreadId(), 0, 0};
        CallerRegs(&g.r14, &g.rdi);
        Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HTG*>(p);
            glitch::NoteFxTexture(b.caller, b.tid, b.r14, b.rdi);
            reinterpret_cast<SetTexFn>(VT(b.fx)[kSetTexture])(b.fx, b.h, b.t); }, g);
        return D3D_OK;
    }
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HT*>(p);
        reinterpret_cast<SetTexFn>(VT(b.fx)[kSetTexture])(b.fx, b.h, b.t); }, HT{w->real, h, t});
    return D3D_OK;
}

// (glitch detection) The technique's name, for the draw log; render thread.
void NoteTechniqueName(void* fx, Handle h) {
    static std::unordered_map<uint64_t, const char*> names;
    uint64_t key = reinterpret_cast<uint64_t>(fx) * 31 ^ reinterpret_cast<uint64_t>(h);
    auto it = names.find(key);
    if (it == names.end()) {
        TechniqueDesc desc{};
        const char* name = SUCCEEDED(reinterpret_cast<GetTechniqueDescFn>(VT(fx)[kGetTechniqueDesc])(fx, h, &desc))
                               ? desc.name : nullptr;
        it = names.emplace(key, name).first;
    }
    glitch::NoteState(glitch::kTechnique, it->second);
}

HRESULT STDMETHODCALLTYPE SetTechnique(Wrapper* w, Handle h) {
    w->technique[Role()] = h;
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<SetTechFn>(w, kSetTechnique)(w->real, h))
    if (glitch::Enabled()) {
        Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const H0*>(p);
            NoteTechniqueName(b.fx, b.h);
            reinterpret_cast<SetTechFn>(VT(b.fx)[kSetTechnique])(b.fx, b.h); }, H0{w->real, h});
        return D3D_OK;
    }
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const H0*>(p);
        reinterpret_cast<SetTechFn>(VT(b.fx)[kSetTechnique])(b.fx, b.h); }, H0{w->real, h});
    return D3D_OK;
}

// Begin returns the pass count, which depends only on the technique: ask the real
// effect once per technique (synchronously) and record afterwards.
HRESULT STDMETHODCALLTYPE Begin(Wrapper* w, UINT* passes, DWORD flags) {
    Handle tech = w->technique[Role()];
    PassSeen& seen = t_passSeen[(reinterpret_cast<uintptr_t>(w) >> 6) & 15];
    bool known = tech && seen.w == w && seen.serial == w->serial && seen.tech == tech;
    UINT cached = known ? seen.count : 0;
    if (!known) {
        AcquireSRWLockShared(&w->lock);
        auto it = w->passes.find(tech);
        known = it != w->passes.end();
        cached = known ? it->second : 0;
        ReleaseSRWLockShared(&w->lock);
        if (known) seen = {w, w->serial, tech, cached};
    }
    // An unknown technique's pass count comes from its description (metadata, safe while the
    // render thread uses the effect), so recording never waits for the queue here.
    if (Recording() && !known && tech) {
        TechniqueDesc desc{};
        if (SUCCEEDED(RealMethod<GetTechniqueDescFn>(w, kGetTechniqueDesc)(w->real, tech, &desc))) {
            AcquireSRWLockExclusive(&w->lock);
            w->passes[tech] = desc.passes;
            ReleaseSRWLockExclusive(&w->lock);
            known = true;
            cached = desc.passes;
        }
    }
    if (!Recording() || !known) {
        DrainForEffect();
        UINT n = 0;
        HRESULT hr = RealMethod<BeginFn>(w, kBegin)(w->real, &n, flags);
        if (SUCCEEDED(hr)) {
            AcquireSRWLockExclusive(&w->lock);
            w->passes[tech] = n;
            ReleaseSRWLockExclusive(&w->lock);
        }
        if (passes) *passes = n;
        return hr;
    }
    if (passes) *passes = cached;
    // The recording thread loops over `cached` passes; the effect, at its place in the stream,
    // may be on another technique (set by another segment): count when the two disagree.
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HB*>(p); UINT actual = 0;
        reinterpret_cast<BeginFn>(VT(b.fx)[kBegin])(b.fx, &actual, b.flags);
        if (actual != b.assumed) {
            long n = InterlockedIncrement(&g_beginMismatches);
            if (n <= 20 || n % 1000 == 0)
                Log("rteffect: Begin on %p recorded by thread %lu assumed %u pass(es) (technique %p), the effect has %u "
                    "(%ld so far)", b.fx, b.tid, b.assumed, b.tech, actual, n);
        } }, HB{w->real, flags, cached, tech, GetCurrentThreadId()});
    return D3D_OK;
}
// Passes and commits set device state on the render thread, out of sight of the
// main thread's shadow copy, so that copy is forgotten.
HRESULT STDMETHODCALLTYPE BeginPass(Wrapper* w, UINT pass) {
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<UIntFn>(w, kBeginPass)(w->real, pass))
    InvalidateEffectState();
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HU*>(p);
        reinterpret_cast<UIntFn>(VT(b.fx)[kBeginPass])(b.fx, b.u); }, HU{w->real, pass});
    return D3D_OK;
}

template <int SlotV>
HRESULT STDMETHODCALLTYPE NoArgs(Wrapper* w) {
    TSCFIX_DIRECT_IF_NOT_RECORDING(RealMethod<VoidFn>(w, SlotV)(w->real))
    InvalidateEffectState();
    Push([](const uint8_t* p) { auto& b = *reinterpret_cast<const HU*>(p);
        reinterpret_cast<VoidFn>(VT(b.fx)[SlotV])(b.fx); }, HU{w->real, 0});
    return D3D_OK;
}

ULONG STDMETHODCALLTYPE AddRef(Wrapper* w) { return ++w->refs; }
ULONG STDMETHODCALLTYPE Release(Wrapper* w) {
    ULONG left = --w->refs;
    if (left == 0) {
        DrainForEffect();
        RealMethod<ULONG(STDMETHODCALLTYPE*)(void*)>(w, kRelease)(w->real);
        delete w;
    }
    return left;
}

void BuildVTable() {
    // A draining thunk is 81 bytes, padded to 96; 128 per slot leaves room.
    g_thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, 128 * kSlots, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t* p = g_thunks;
    InitializeCriticalSection(&g_metaLock);
    for (int i = 0; i < kSlots; ++i) {
        g_vtbl[i] = IsMetadata(i) ? EmitLockedThunk(p, i) : EmitThunk(p, i, true);
        if (i < 80) g_thunkStart[i] = static_cast<uint8_t*>(g_vtbl[i]);
    }
    auto set = [](int slot, auto fn) { g_vtbl[slot] = reinterpret_cast<void*>(fn); };
    set(kAddRef, &AddRef);
    set(kRelease, &Release);
    set(kSetBool, &SetBool);
    set(kGetBool, &GetBool);
    set(kSetInt, &SetInt);
    set(kSetFloat, &SetFloat);
    set(kSetValue, &SetArray<kSetValue, 1>);
    set(kSetFloatArray, &SetArray<kSetFloatArray, sizeof(float)>);
    set(kSetVector, &SetFixed<kSetVector, sizeof(Vector4)>);
    set(kSetVectorArray, &SetArray<kSetVectorArray, sizeof(Vector4)>);
    set(kSetMatrix, &SetFixed<kSetMatrix, sizeof(Matrix)>);
    set(kSetMatrixArray, &SetArray<kSetMatrixArray, sizeof(Matrix)>);
    set(kSetMatrixTranspose, &SetFixed<kSetMatrixTranspose, sizeof(Matrix)>);
    set(kSetTexture, &SetTexture);
    set(kSetTechnique, &SetTechnique);
    set(kBegin, &Begin);
    set(kBeginPass, &BeginPass);
    set(kCommitChanges, &NoArgs<kCommitChanges>);
    set(kEndPass, &NoArgs<kEndPass>);
    set(kEnd, &NoArgs<kEnd>);
}

}  // namespace

void* WrapEffect(void* realEffect) {
    if (!realEffect) return realEffect;
    static bool built = false;
    if (!built) {
        BuildVTable();
        built = true;
        Log("rt: effect wrapper ready");
    }
    auto* w = new Wrapper;
    w->vtbl = g_vtbl;
    w->real = realEffect;
    static_assert(offsetof(Wrapper, real) == 8, "thunks expect the real effect at +8");
    return w;
}

}  // namespace tscfix::rt
