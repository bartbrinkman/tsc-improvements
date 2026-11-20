#include "psfade.h"
#include "trace.h"
#include "d3dlayer.h"

#include "sites.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "d3dnames.h"
#include "flickercap.h"
#include "glitch.h"
#include "log.h"
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include "postfx.h"
#include "resolve.h"
#include "rtdevice.h"
#include "rteffect.h"
#include "status.h"

namespace tscfix::d3d {
namespace {

Options g_opt;

enum Iface { kDev = 0, kFx = 1, kIfaceCount = 2 };
constexpr int kMaxSlots = 128;
constexpr int kDevSlots = 119;
constexpr int kFxSlots = 79;

// Device slots we override (the rest get counting thunks when the census is on).
enum DevSlot {
    kReset = 16, kPresent = 17, kCreateTexture = 23, kCreateCubeTexture = 25, kCreateVB = 26, kCreateIB = 27,
    kCreateOffscreen = 36,
    kCreateStateBlock = 59, kBeginStateBlock = 60, kEndStateBlock = 61, kSetRenderState = 57, kSetTexture = 65,
    kSetTextureStageState = 67, kSetSamplerState = 69, kSetVertexDeclaration = 87, kSetFVF = 89,
    kSetVertexShader = 92, kSetVSConstF = 94, kSetStreamSource = 100, kSetIndices = 104,
    kSetPixelShader = 107, kSetPSConstF = 109, kCreateQuery = 118,
};
enum FxSlot { kFxBegin = 63, kFxEnd = 67 };

// Thunk memory: code, the current original of every slot, and counters, kept in one
// block so the thunks can address them RIP-relative.
struct Block {
    uint8_t code[kIfaceCount][kMaxSlots][16];
    void* original[kIfaceCount][kMaxSlots];
    volatile uint64_t count[kIfaceCount][kMaxSlots];
    volatile uint64_t filtered[kMaxSlots];
};
Block* g_b = nullptr;
void* g_devWant[kMaxSlots];  // what each device slot should point to (nullptr = untouched)

inline void Count(Iface i, int slot) {
    if (g_opt.census) ++g_b->count[i][slot];
}
template <typename Fn>
inline Fn Orig(Iface i, int slot) {
    return reinterpret_cast<Fn>(g_b->original[i][slot]);
}

void* EmitThunk(Iface iface, int slot) {
    // lock inc qword [rip+count]; jmp [rip+original]
    uint8_t* c = g_b->code[iface][slot];
    auto rel = [](const uint8_t* next, const volatile void* to) {
        return static_cast<int32_t>(reinterpret_cast<const uint8_t*>(const_cast<const void*>(to)) - next);
    };
    c[0] = 0xF0;
    c[1] = 0x48;
    c[2] = 0xFF;
    c[3] = 0x05;
    int32_t d1 = rel(c + 8, &g_b->count[iface][slot]);
    memcpy(c + 4, &d1, 4);
    c[8] = 0xFF;
    c[9] = 0x25;
    int32_t d2 = rel(c + 14, &g_b->original[iface][slot]);
    memcpy(c + 10, &d2, 4);
    return c;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// --- shadow state for the redundant-state filter ---------------------------------------
// Each entry carries the generation it was written in; Invalidate() starts a new
// generation, so every entry becomes unknown at the cost of one increment.
using Gen = uint32_t;
struct Shadow {
    bool recording = false;
    Gen gen = 1;
    DWORD rs[256];          Gen rsKnown[256];
    DWORD ss[21][14];       Gen ssKnown[21][14];
    DWORD tss[8][33];       Gen tssKnown[8][33];
    void* tex[21];          Gen texKnown[21];
    void* vs; void* ps; void* decl; void* ib;
    Gen vsKnown, psKnown, declKnown, ibKnown;
    struct Stream { void* vb; UINT off; UINT stride; } stream[16];
    Gen streamKnown[16];
    float vsc[256][4];      Gen vscKnown[256];
    float psc[224][4];      Gen pscKnown[224];
} g_s;

inline void Invalidate() { ++g_s.gen; }
inline bool Known(Gen g) { return g == g_s.gen; }
volatile bool g_filterActive = true;     // A/B: hooks stay installed, skipping can be paused
volatile bool g_noSaveActive = true;
inline bool Filtering() { return g_opt.stateFilter && g_filterActive && !g_s.recording; }
inline bool NoSaveState() { return g_opt.effectNoSaveState && g_noSaveActive; }
inline int SamplerIndex(DWORD s) { return s < 16 ? int(s) : (s >= 256 && s <= 260) ? int(s - 240) : -1; }
inline void Skipped(int slot) { if (g_opt.census) ++g_b->filtered[slot]; }

// --- device overrides ---------------------------------------------------------------
HRESULT STDMETHODCALLTYPE SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
    Count(kDev, kSetRenderState);
    v = postfx::BlendValue(st, v);
    if (Filtering() && st < 256) {
        if (Known(g_s.rsKnown[st]) && g_s.rs[st] == v) return Skipped(kSetRenderState), D3D_OK;
        HRESULT hr = Orig<decltype(&SetRenderState)>(kDev, kSetRenderState)(d, st, v);
        if (SUCCEEDED(hr)) g_s.rs[st] = v, g_s.rsKnown[st] = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetRenderState)>(kDev, kSetRenderState)(d, st, v);
}

// Sampler states an effect sets between its Begin and End, with the values they had before.
// The game begins effects with state saving and draws later passes (the cab's shading among
// them) with the sampler filters that End put back; with DONOTSAVESTATE those passes got
// whatever filter the last effect left (seen as blocky, stepped shading in the cab). End now
// puts the sampler states back itself: a handful of calls per frame instead of a state block
// per effect. Per thread: an effect's Begin, passes and End run on one thread (the game thread,
// or the render thread replaying them).
struct FxSamplers {
    int depth = 0;
    uint32_t epoch = 1;
    int n = 0;
    IDirect3DDevice9* dev = nullptr;
    struct Entry { DWORD sampler; D3DSAMPLERSTATETYPE type; DWORD before, now; } e[21 * 14];
    uint32_t seen[21][14] = {};   // epoch the state was first set in
    uint16_t at[21][14];          // its entry
};
thread_local FxSamplers t_fx;

HRESULT STDMETHODCALLTYPE SetSamplerState(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    Count(kDev, kSetSamplerState);
    if (t == D3DSAMP_MAXANISOTROPY) v = resolve::Anisotropy(v);
    int i = SamplerIndex(s);
    if (t_fx.depth && i >= 0 && t < 14) {
        FxSamplers& f = t_fx;
        if (f.seen[i][t] != f.epoch) {
            DWORD before = 0;
            if (SUCCEEDED(d->GetSamplerState(s, t, &before))) {
                f.seen[i][t] = f.epoch;
                f.at[i][t] = static_cast<uint16_t>(f.n);
                f.e[f.n++] = {s, t, before, v};
                f.dev = d;
            }
        } else {
            f.e[f.at[i][t]].now = v;
        }
    }
    if (Filtering() && i >= 0 && t < 14) {
        if (Known(g_s.ssKnown[i][t]) && g_s.ss[i][t] == v) return Skipped(kSetSamplerState), D3D_OK;
        HRESULT hr = Orig<decltype(&SetSamplerState)>(kDev, kSetSamplerState)(d, s, t, v);
        if (SUCCEEDED(hr)) g_s.ss[i][t] = v, g_s.ssKnown[i][t] = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetSamplerState)>(kDev, kSetSamplerState)(d, s, t, v);
}

HRESULT STDMETHODCALLTYPE SetTextureStageState(IDirect3DDevice9* d, DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    Count(kDev, kSetTextureStageState);
    if (Filtering() && s < 8 && t < 33) {
        if (Known(g_s.tssKnown[s][t]) && g_s.tss[s][t] == v) return Skipped(kSetTextureStageState), D3D_OK;
        HRESULT hr = Orig<decltype(&SetTextureStageState)>(kDev, kSetTextureStageState)(d, s, t, v);
        if (SUCCEEDED(hr)) g_s.tss[s][t] = v, g_s.tssKnown[s][t] = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetTextureStageState)>(kDev, kSetTextureStageState)(d, s, t, v);
}

// A bound object stays referenced by the runtime, so an equal pointer is the same object.
HRESULT STDMETHODCALLTYPE SetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) {
    Count(kDev, kSetTexture);
    resolve::OnSetTexture(d, s, t);
    int i = SamplerIndex(s);
    if (Filtering() && i >= 0) {
        if (Known(g_s.texKnown[i]) && g_s.tex[i] == t) return Skipped(kSetTexture), D3D_OK;
        HRESULT hr = Orig<decltype(&SetTexture)>(kDev, kSetTexture)(d, s, t);
        if (SUCCEEDED(hr)) g_s.tex[i] = t, g_s.texKnown[i] = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetTexture)>(kDev, kSetTexture)(d, s, t);
}

#define TSCFIX_PTR_SETTER(Name, Slot, Type, field, known)                                   \
    HRESULT STDMETHODCALLTYPE Name(IDirect3DDevice9* d, Type p) {                           \
        Count(kDev, Slot);                                                                  \
        if (Filtering()) {                                                                  \
            if (Known(g_s.known) && g_s.field == p) return Skipped(Slot), D3D_OK;                  \
            HRESULT hr = Orig<decltype(&Name)>(kDev, Slot)(d, p);                           \
            if (SUCCEEDED(hr)) g_s.field = p, g_s.known = g_s.gen;                             \
            return hr;                                                                      \
        }                                                                                   \
        return Orig<decltype(&Name)>(kDev, Slot)(d, p);                                     \
    }
TSCFIX_PTR_SETTER(SetVertexShader, kSetVertexShader, IDirect3DVertexShader9*, vs, vsKnown)
// The pixel shader: the picture passes of the mod (resolve.h, postfx.h) put their own shader in the
// place of the game's first, as GlitchSetPS and SetPixelShaderResolve do. The filter then compares
// the shader that goes to the runtime, not the one the game gave: two game shaders can get the
// same replacement, and one game shader can get another replacement later. The substitution runs
// at every set, also at a set that the filter skips: it keeps the passes' own state (which pass
// is set now).
HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* p) {
    Count(kDev, kSetPixelShader);
    p = postfx::Substitute(d, resolve::Substitute(d, p));
    if (Filtering()) {
        if (Known(g_s.psKnown) && g_s.ps == p) return Skipped(kSetPixelShader), D3D_OK;
        HRESULT hr = Orig<decltype(&SetPixelShader)>(kDev, kSetPixelShader)(d, p);
        if (SUCCEEDED(hr)) g_s.ps = p, g_s.psKnown = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetPixelShader)>(kDev, kSetPixelShader)(d, p);
}
TSCFIX_PTR_SETTER(SetIndices, kSetIndices, IDirect3DIndexBuffer9*, ib, ibKnown)
// (glitch detection) the same setters feeding the draw log, installed instead of the filtering
// ones when glitch_detect is on and the state filter is off.
HRESULT STDMETHODCALLTYPE GlitchSetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* p) {
    glitch::NoteState(glitch::kVS, p);
    return Orig<decltype(&GlitchSetVS)>(kDev, kSetVertexShader)(d, p);
}
HRESULT STDMETHODCALLTYPE GlitchSetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* p) {
    p = postfx::Substitute(d, resolve::Substitute(d, p));
    glitch::NoteState(glitch::kPS, p);
    return Orig<decltype(&GlitchSetPS)>(kDev, kSetPixelShader)(d, p);
}
HRESULT STDMETHODCALLTYPE GlitchSetIndices(IDirect3DDevice9* d, IDirect3DIndexBuffer9* p) {
    glitch::NoteState(glitch::kIB, p);
    return Orig<decltype(&GlitchSetIndices)>(kDev, kSetIndices)(d, p);
}
HRESULT STDMETHODCALLTYPE GlitchSetStreamSource(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (n == 0) glitch::NoteState(glitch::kVB, vb);
    if (n >= 1 && n <= 3) {
        glitch::NoteBinding(static_cast<int>(n), reinterpret_cast<uintptr_t>(vb));
        glitch::NoteBinding(static_cast<int>(3 + n), (static_cast<uint64_t>(off) << 32) | stride);
    }
    return Orig<decltype(&GlitchSetStreamSource)>(kDev, kSetStreamSource)(d, n, vb, off, stride);
}
HRESULT STDMETHODCALLTYPE GlitchSetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) {
    glitch::NoteTexture(s, t);
    HRESULT hr = Orig<decltype(&GlitchSetTexture)>(kDev, kSetTexture)(d, s, t);
    resolve::OnSetTexture(d, s, t);
    return hr;
}
HRESULT STDMETHODCALLTYPE GlitchSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    if (i == 0) glitch::NoteRenderTarget(d, s);
    return Orig<decltype(&GlitchSetRenderTarget)>(kDev, 37)(d, i, s);
}
HRESULT STDMETHODCALLTYPE GlitchSetDepthStencil(IDirect3DDevice9* d, IDirect3DSurface9* s) {
    glitch::NoteDepthStencil(s);
    return Orig<decltype(&GlitchSetDepthStencil)>(kDev, 39)(d, s);
}
HRESULT STDMETHODCALLTYPE GlitchSetVSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    glitch::NoteConstants(false, start, data, count);
    return Orig<decltype(&GlitchSetVSConstF)>(kDev, 94)(d, start, data, count);
}
HRESULT STDMETHODCALLTYPE GlitchSetPSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    postfx::OnPixelConstants(start, data, count);
    glitch::NoteConstants(true, start, data, count);
    return Orig<decltype(&GlitchSetPSConstF)>(kDev, 109)(d, start, data, count);
}
HRESULT STDMETHODCALLTYPE GlitchSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
    v = postfx::BlendValue(st, v);
    glitch::NoteRenderState(static_cast<DWORD>(st), v);
    if (st == D3DRS_SLOPESCALEDEPTHBIAS) glitch::NoteSlopeSetter(v, _ReturnAddress());
    return Orig<decltype(&GlitchSetRenderState)>(kDev, kSetRenderState)(d, st, v);
}
HRESULT STDMETHODCALLTYPE GlitchSetSamplerState(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE ty, DWORD v) {
    if (ty == D3DSAMP_MAXANISOTROPY) v = resolve::Anisotropy(v);
    glitch::NoteSamplerState(s, static_cast<DWORD>(ty), v);
    return Orig<decltype(&GlitchSetSamplerState)>(kDev, kSetSamplerState)(d, s, ty, v);
}
HRESULT STDMETHODCALLTYPE GlitchSetStreamFreq(IDirect3DDevice9* d, UINT n, UINT setting) {
    if (n < 4) glitch::NoteBinding(static_cast<int>(7 + n), setting);
    return Orig<decltype(&GlitchSetStreamFreq)>(kDev, 102)(d, n, setting);
}
HRESULT STDMETHODCALLTYPE GlitchSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) {
    glitch::NoteViewport(vp);
    return Orig<decltype(&GlitchSetViewport)>(kDev, 47)(d, vp);
}
HRESULT STDMETHODCALLTYPE GlitchDrawIndexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts,
                                            UINT start, UINT prims) {
    postfx::OnDraw(d);
    if (glitch::CensusOnly()) {  // the device's own value, against the value the hooks followed
        DWORD real = 0;
        if (SUCCEEDED(d->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &real))) glitch::NoteRealSlope(real);
    }
    glitch::NoteDraw(base, start, prims);
    bool pick = glitch::PickBegin(d);
    HRESULT hr = Orig<decltype(&GlitchDrawIndexed)>(kDev, 82)(d, t, base, minIndex, verts, start, prims);
    if (pick) glitch::PickEnd(d);
    return hr;
}
// (pixel pick) the non-indexed draws too: full-screen passes and light volumes.
HRESULT STDMETHODCALLTYPE GlitchDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT prims) {
    postfx::OnDraw(d);
    glitch::NoteDraw(0, start, prims);
    bool pick = glitch::PickBegin(d);
    HRESULT hr = Orig<decltype(&GlitchDraw)>(kDev, 81)(d, t, start, prims);
    if (pick) glitch::PickEnd(d);
    return hr;
}
#undef TSCFIX_PTR_SETTER

HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* p) {
    Count(kDev, kSetVertexDeclaration);
    if (Filtering()) {
        if (Known(g_s.declKnown) && g_s.decl == p) return Skipped(kSetVertexDeclaration), D3D_OK;
        HRESULT hr = Orig<decltype(&SetVertexDeclaration)>(kDev, kSetVertexDeclaration)(d, p);
        if (SUCCEEDED(hr)) g_s.decl = p, g_s.declKnown = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetVertexDeclaration)>(kDev, kSetVertexDeclaration)(d, p);
}

HRESULT STDMETHODCALLTYPE SetFVF(IDirect3DDevice9* d, DWORD fvf) {
    Count(kDev, kSetFVF);
    g_s.declKnown = 0;  // SetFVF replaces the vertex declaration
    return Orig<decltype(&SetFVF)>(kDev, kSetFVF)(d, fvf);
}

HRESULT STDMETHODCALLTYPE SetStreamSource(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    Count(kDev, kSetStreamSource);
    if (Filtering() && n < 16) {
        auto& s = g_s.stream[n];
        if (Known(g_s.streamKnown[n]) && s.vb == vb && s.off == off && s.stride == stride)
            return Skipped(kSetStreamSource), D3D_OK;
        HRESULT hr = Orig<decltype(&SetStreamSource)>(kDev, kSetStreamSource)(d, n, vb, off, stride);
        if (SUCCEEDED(hr)) s = {vb, off, stride}, g_s.streamKnown[n] = g_s.gen;
        return hr;
    }
    return Orig<decltype(&SetStreamSource)>(kDev, kSetStreamSource)(d, n, vb, off, stride);
}

// Shader constants: skip a call only if every register it writes already holds the value.
template <int N>
bool ConstantsEqual(float (&shadow)[N][4], Gen (&known)[N], UINT start, const float* data, UINT count) {
    if (start + count > N) return false;
    for (UINT r = 0; r < count; ++r)
        if (!Known(known[start + r]) || memcmp(shadow[start + r], data + 4 * r, 16) != 0) return false;
    return true;
}
template <int N>
void ConstantsStore(float (&shadow)[N][4], Gen (&known)[N], UINT start, const float* data, UINT count) {
    if (start + count > N) return;
    memcpy(shadow[start], data, 16 * count);
    for (UINT r = 0; r < count; ++r) known[start + r] = g_s.gen;
}

HRESULT STDMETHODCALLTYPE SetVSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    Count(kDev, kSetVSConstF);
    if (Filtering() && data) {
        if (ConstantsEqual(g_s.vsc, g_s.vscKnown, start, data, count)) return Skipped(kSetVSConstF), D3D_OK;
        HRESULT hr = Orig<decltype(&SetVSConstF)>(kDev, kSetVSConstF)(d, start, data, count);
        if (SUCCEEDED(hr)) ConstantsStore(g_s.vsc, g_s.vscKnown, start, data, count);
        return hr;
    }
    return Orig<decltype(&SetVSConstF)>(kDev, kSetVSConstF)(d, start, data, count);
}

HRESULT STDMETHODCALLTYPE SetPSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    Count(kDev, kSetPSConstF);
    postfx::OnPixelConstants(start, data, count);  // (before the filter: a skipped call holds the same values)
    if (Filtering() && data) {
        if (ConstantsEqual(g_s.psc, g_s.pscKnown, start, data, count)) return Skipped(kSetPSConstF), D3D_OK;
        HRESULT hr = Orig<decltype(&SetPSConstF)>(kDev, kSetPSConstF)(d, start, data, count);
        if (SUCCEEDED(hr)) ConstantsStore(g_s.psc, g_s.pscKnown, start, data, count);
        return hr;
    }
    return Orig<decltype(&SetPSConstF)>(kDev, kSetPSConstF)(d, start, data, count);
}

HRESULT STDMETHODCALLTYPE BeginStateBlock(IDirect3DDevice9* d) {
    Count(kDev, kBeginStateBlock);
    g_s.recording = true;
    return Orig<decltype(&BeginStateBlock)>(kDev, kBeginStateBlock)(d);
}

HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DDevice9* d, IDirect3DStateBlock9** sb);

// State blocks change device state without going through the device interface.
using SBApplyFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
SBApplyFn g_sbApply = nullptr, g_sbCapture = nullptr;

// With a render thread, Capture/Apply are recorded like device calls: their place in
// the command stream is what makes them observe and change the right device state.
struct SBCall { IDirect3DStateBlock9* sb; SBApplyFn fn; };
void RecordStateBlockCall(IDirect3DStateBlock9* sb, SBApplyFn fn) {
    rt::KeepAlive(sb);
    SBCall call{sb, fn};
    rt::Enqueue([](const uint8_t* p) {
        auto& c = *reinterpret_cast<const SBCall*>(p);
        c.fn(c.sb);
    }, &call, sizeof call);
}

HRESULT STDMETHODCALLTYPE StateBlockApply(IDirect3DStateBlock9* sb) {
    Invalidate();
    if (g_opt.renderThread >= 2) {
        if (rt::Recording()) {
            rt::StateBlockApplied(sb);
            RecordStateBlockCall(sb, g_sbApply);
            return D3D_OK;
        }
        rt::InvalidateShadow();
        rt::SyncForDirectAccess(rt::kSyncStateBlock);
    }
    return g_sbApply(sb);
}
HRESULT STDMETHODCALLTYPE StateBlockCapture(IDirect3DStateBlock9* sb) {
    if (g_opt.renderThread >= 2) {
        if (rt::Recording()) {
            rt::StateBlockCaptured(sb);
            RecordStateBlockCall(sb, g_sbCapture);
            return D3D_OK;
        }
        rt::SyncForDirectAccess(rt::kSyncStateBlock);
    }
    return g_sbCapture(sb);
}

void HookStateBlock(IDirect3DStateBlock9* sb) {
    if (!sb) return;
    void** vt = *reinterpret_cast<void***>(sb);
    constexpr int kCapture = 4;
    if (vt[kCapture] != reinterpret_cast<void*>(&StateBlockCapture)) {
        if (!g_sbCapture) g_sbCapture = reinterpret_cast<SBApplyFn>(vt[kCapture]);
        if (reinterpret_cast<void*>(g_sbCapture) == vt[kCapture])
            WriteSlot(&vt[kCapture], reinterpret_cast<void*>(&StateBlockCapture));
    }
    constexpr int kApply = 5;  // QueryInterface, AddRef, Release, GetDevice, Capture, Apply
    if (vt[kApply] == reinterpret_cast<void*>(&StateBlockApply)) return;
    if (!g_sbApply) g_sbApply = reinterpret_cast<SBApplyFn>(vt[kApply]);
    if (reinterpret_cast<void*>(g_sbApply) == vt[kApply]) {
        WriteSlot(&vt[kApply], reinterpret_cast<void*>(&StateBlockApply));
    } else {
        // Unknown Apply implementation: we cannot track it, so stop filtering for good.
        g_opt.stateFilter = false;
        Log("d3d: unexpected state block implementation; state filter disabled");
    }
}

HRESULT STDMETHODCALLTYPE CreateStateBlock(IDirect3DDevice9* d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** sb) {
    Count(kDev, kCreateStateBlock);
    HRESULT hr = Orig<decltype(&CreateStateBlock)>(kDev, kCreateStateBlock)(d, t, sb);
    if (SUCCEEDED(hr) && sb) HookStateBlock(*sb);
    return hr;
}

HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DDevice9* d, IDirect3DStateBlock9** sb) {
    Count(kDev, kEndStateBlock);
    g_s.recording = false;
    Invalidate();
    HRESULT hr = Orig<decltype(&EndStateBlock)>(kDev, kEndStateBlock)(d, sb);
    if (SUCCEEDED(hr) && sb) HookStateBlock(*sb);
    return hr;
}

HRESULT STDMETHODCALLTYPE Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    Count(kDev, kReset);
    Invalidate();
    resolve::OnReset();
    postfx::OnReset();
    flickercap::OnReset();
    return Orig<decltype(&Reset)>(kDev, kReset)(d, pp);
}

void Rearm(IDirect3DDevice9* d);

// LOD fade: the game's shader-model-3 pixel shaders get the screen-door discard (psfade.h). A
// shader that the runtime refuses in its new form is created as the game gave it.
volatile long g_fadePatched = 0, g_fadeOther = 0, g_fadeRefused = 0;
HRESULT STDMETHODCALLTYPE CreatePixelShaderFade(IDirect3DDevice9* d, const DWORD* code, IDirect3DPixelShader9** out) {
    auto create = Orig<decltype(&CreatePixelShaderFade)>(kDev, 106);
    if (!code || !out) return create(d, code, out);
    if (resolve::Enabled() && resolve::IsShrinkShader(code)) {  // the supersampling shrink step: kept as it is, replaced when set
        HRESULT hr = create(d, code, out);
        if (SUCCEEDED(hr)) resolve::NoteShrinkShader(*out);
        return hr;
    }
    if (!g_opt.lodFade) return create(d, code, out);
    std::vector<uint32_t> patched;
    if (psfade::Patch(reinterpret_cast<const uint32_t*>(code), &patched) != psfade::Result::Patched) {
        InterlockedIncrement(&g_fadeOther);
        return create(d, code, out);
    }
    HRESULT hr = create(d, reinterpret_cast<const DWORD*>(patched.data()), out);
    if (SUCCEEDED(hr)) {
        long n = InterlockedIncrement(&g_fadePatched);
        if (n == 1 || n % 100 == 0) Log("lod fade: %ld pixel shaders with the fade discard, %ld left as they are, %ld refused", n, g_fadeOther, g_fadeRefused);
        return hr;
    }
    InterlockedIncrement(&g_fadeRefused);
    return create(d, code, out);
}

// (measurement, with resolve on) Stretch copies between surfaces of different sizes: one line per size pair.
HRESULT STDMETHODCALLTYPE StretchRectProbe(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                                           D3DTEXTUREFILTERTYPE filter) {
    static uint64_t seen[16];
    static volatile LONG count = 0;
    D3DSURFACE_DESC a{}, b{};
    if (src && dst && SUCCEEDED(src->GetDesc(&a)) && SUCCEEDED(dst->GetDesc(&b))) {
        uint64_t key = (uint64_t{a.Width} << 48) | (uint64_t{a.Height} << 32) | (uint64_t{b.Width} << 16) | b.Height;
        bool known = false;
        LONG n = count;
        for (LONG i = 0; i < n && i < 16; ++i) known |= seen[i] == key;
        if (!known && n < 16) {
            seen[n] = key, count = n + 1;
            Log("resolve probe: StretchRect %ux%u (fmt %d, multisample %d) to %ux%u (fmt %d), filter %d, rects %d/%d", a.Width, a.Height, a.Format,
                a.MultiSampleType, b.Width, b.Height, b.Format, filter, sr != nullptr, dr != nullptr);
        }
    }
    return Orig<decltype(&StretchRectProbe)>(kDev, 34)(d, src, sr, dst, dr, filter);
}

// (measurement, [experiments] sampler_probe=1) Which filter settings the game puts on its texture
// samplers: calls per (setting, value) over 100 frames, logged once.
volatile LONG g_samplerProbe = 0;  // 0 off, 1 counting
volatile LONG g_samplerCounts[5][20];
HRESULT STDMETHODCALLTYPE SetSamplerStateProbe(IDirect3DDevice9* d, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    if (g_samplerProbe == 1) {
        int row = type == D3DSAMP_MAGFILTER ? 0 : type == D3DSAMP_MINFILTER ? 1 : type == D3DSAMP_MIPFILTER ? 2 : type == D3DSAMP_MAXANISOTROPY ? 3 : -1;
        if (row >= 0) InterlockedIncrement(&g_samplerCounts[row][value < 19 ? value : 19]);
        if (type == D3DSAMP_MIPMAPLODBIAS) InterlockedIncrement(&g_samplerCounts[4][value == 0 ? 0 : 1]);
    }
    if (type == D3DSAMP_MAXANISOTROPY) value = resolve::Anisotropy(value);
    return Orig<decltype(&SetSamplerStateProbe)>(kDev, kSetSamplerState)(d, sampler, type, value);
}
HRESULT STDMETHODCALLTYPE SetRenderStatePostfx(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD value) {
    return Orig<decltype(&SetRenderStatePostfx)>(kDev, kSetRenderState)(d, state, postfx::BlendValue(state, value));
}
// (blend over time) The camera of each frame is read from the constants of the game's sun light shader.
HRESULT STDMETHODCALLTYPE SetPSConstFPostfx(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    postfx::OnPixelConstants(start, data, count);
    return Orig<decltype(&SetPSConstFPostfx)>(kDev, kSetPSConstF)(d, start, data, count);
}
// (blend over time) The draw calls: the first one of the last pass reads the target of the pass
// (postfx::OnDraw). A full-screen pass can use any of the four calls, so all four are hooked.
HRESULT STDMETHODCALLTYPE DrawPostfx(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT prims) {
    Count(kDev, 81);
    postfx::OnDraw(d);
    return Orig<decltype(&DrawPostfx)>(kDev, 81)(d, t, start, prims);
}
HRESULT STDMETHODCALLTYPE DrawIndexedPostfx(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT prims) {
    Count(kDev, 82);
    postfx::OnDraw(d);
    return Orig<decltype(&DrawIndexedPostfx)>(kDev, 82)(d, t, base, minIndex, verts, start, prims);
}
HRESULT STDMETHODCALLTYPE DrawUPPostfx(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride) {
    Count(kDev, 83);
    postfx::OnDraw(d);
    return Orig<decltype(&DrawUPPostfx)>(kDev, 83)(d, t, prims, data, stride);
}
HRESULT STDMETHODCALLTYPE DrawIndexedUPPostfx(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minIndex, UINT verts, UINT prims, const void* indices, D3DFORMAT format,
                                              const void* data, UINT stride) {
    Count(kDev, 84);
    postfx::OnDraw(d);
    return Orig<decltype(&DrawIndexedUPPostfx)>(kDev, 84)(d, t, minIndex, verts, prims, indices, format, data, stride);
}
void SamplerProbeFrame(unsigned frame) {
    if (frame == 500) g_samplerProbe = 1;
    if (frame != 600 || g_samplerProbe != 1) return;
    g_samplerProbe = 2;
    static const char* rows[] = {"mag filter", "min filter", "mip filter", "max anisotropy", "mip bias (0 = none, 1 = other)"};
    for (int r = 0; r < 5; ++r) {
        char text[400];
        int n = 0;
        for (int v = 0; v < 20; ++v)
            if (g_samplerCounts[r][v]) n += sprintf_s(text + n, sizeof text - n, " %d: %ld", v, g_samplerCounts[r][v]);
        Log("sampler probe: %s, value: calls in 100 frames:%s", rows[r], n ? text : " none");
    }
}

HRESULT STDMETHODCALLTYPE SetTextureResolve(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* texture) {
    HRESULT hr = Orig<decltype(&SetTextureResolve)>(kDev, 65)(d, stage, texture);
    resolve::OnSetTexture(d, stage, texture);
    return hr;
}

HRESULT STDMETHODCALLTYPE SetPixelShaderResolve(IDirect3DDevice9* d, IDirect3DPixelShader9* shader) {
    return Orig<decltype(&SetPixelShaderResolve)>(kDev, 107)(d, postfx::Substitute(d, resolve::Substitute(d, shader)));
}

// Frame times for the health line in the log (see main.cpp): frames, frames over 50 and 100 ms,
// and the longest frame, since the last read.
volatile LONG g_healthFrames = 0, g_healthOver50 = 0, g_healthOver100 = 0, g_healthWorstMs = 0;
volatile LONG g_healthActiveMs = 0;  // time in frames of at most one second (a loading screen is not play)
void NoteFrameTime() {
    static LARGE_INTEGER last{}, freq{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (last.QuadPart) {
        LONG ms = static_cast<LONG>((now.QuadPart - last.QuadPart) * 1000 / freq.QuadPart);
        if (ms > 1000) {
            last = now;
            return;
        }
        InterlockedIncrement(&g_healthFrames);
        InterlockedExchangeAdd(&g_healthActiveMs, ms);
        if (ms > 50) InterlockedIncrement(&g_healthOver50);
        if (ms > 100) InterlockedIncrement(&g_healthOver100);
        if (ms > g_healthWorstMs) g_healthWorstMs = ms;
    }
    last = now;
}

void LensPresent();
HRESULT STDMETHODCALLTYPE Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    Count(kDev, kPresent);
    NoteFrameTime();
    resolve::OnPresent(d);
    postfx::OnPresent(d);
    LensPresent();  // before the capture, so the log line of a frame comes before the line of a capture that ends with it
    flickercap::OnPresent(d);
    static unsigned frames = 0, failures = 0;
    if (g_opt.samplerProbe) SamplerProbeFrame(frames + 1);
    if ((++frames & 63) == 1) Rearm(d);
    if (glitch::Enabled()) glitch::OnPresent(d);
    if (g_opt.lodFadeTest > 0) {  // test: every patched shader discards this share of its pixels
        const float fade[4] = {-g_opt.lodFadeTest / 100.0f, 1.0f, 0.0f, 0.0f};
        d->SetPixelShaderConstantF(psfade::kFadeRegister, fade, 1);
    }
    if (g_opt.lodFade && frames == 600)
        Log("lod fade: %ld pixel shaders with the fade discard, %ld left as they are, %ld refused", g_fadePatched, g_fadeOther, g_fadeRefused);
    TSCFIX_TRACE(trace::kDevice, trace::kPresent);
    HRESULT hr = Orig<decltype(&Present)>(kDev, kPresent)(d, a, b, w, r);
    if (FAILED(hr) && failures++ < 20) Log("d3d: Present failed 0x%08lx (frame %u)", hr, frames);
    if (frames == 300) Log("d3d: 300 frames presented, %u failures", failures);
    return hr;
}

// --- resource locks (census) -------------------------------------------------------------
enum LockKind { kVB, kIB, kTex, kLockKinds };
volatile uint64_t g_locks[kLockKinds][4];
void CountLock(LockKind k, DWORD flags) {
    if (!g_opt.census) return;
    int c = (flags & D3DLOCK_DISCARD) ? 0 : (flags & D3DLOCK_NOOVERWRITE) ? 1 : (flags & D3DLOCK_READONLY) ? 2 : 3;
    ++g_locks[k][c];
}
// Resources of the same interface come in several runtime classes (managed, default-pool,
// dynamic ...), each with its own vtable and implementation. Every class gets the hook;
// the original is looked up through the object's vtable.
// Original methods per hooked vtable. Read by any thread without a lock: an entry is written
// completely before the count that makes it visible is published (release / acquire).
struct Originals {
    void* vt[32];
    void* fn[32];
    std::atomic<int> n{0};
    void* For(const void* obj) const {
        void* v = *static_cast<void* const*>(obj);
        int count = n.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
            if (vt[i] == v) return fn[i];
        return nullptr;
    }
};
SRWLOCK g_hookLock = SRWLOCK_INIT;  // serializes HookSlot (resources are created on several threads)
Originals g_vbLock, g_ibLock, g_texLock, g_cubeLock, g_surfLock, g_surfLevel, g_queryIssue, g_queryGetData;

using BufLockFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, void**, DWORD);
using TexLockFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using CubeLockFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfLockFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfLevelFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, IDirect3DSurface9**);

// With a render thread, earlier recorded draws may still use the resource: drain first.
inline void BeforeLock(int kind, DWORD f) {
    int cls = (f & D3DLOCK_DISCARD) ? 3 : (f & D3DLOCK_NOOVERWRITE) ? 2 : (f & D3DLOCK_READONLY) ? 1 : 0;
    if (g_opt.renderThread >= 2) rt::SyncForDirectAccess(rt::kSyncLockBase + kind * 4 + cls);
}

// Deferred buffer locks (render thread): a write-only vertex or index buffer cannot be read
// back (nor can a discarding lock's old contents), so Lock hands out a staging copy and Unlock records "lock the real buffer with the
// same flags, copy, unlock", which runs in stream order on the render thread. No drain.
Originals g_vbUnlock, g_ibUnlock;
using BufUnlockFn = HRESULT(STDMETHODCALLTYPE*)(void*);
volatile bool g_deferLocks = false;  // A/B "deferred_locks"; off by default
struct PendingLock {
    void* buffer;
    uint8_t* data;
    UINT offset, size;
    DWORD flags;
    BufLockFn lock;
    BufUnlockFn unlock;
};
thread_local PendingLock t_pending[8];
thread_local int t_pendingCount = 0;
volatile long g_deferredLocks = 0;
volatile bool g_directNoOverwrite = false;
// The overlap's lock paths (deferred / direct) only pay while the overlap is on; off, locks go
// the plain way as in the stock configuration.
volatile bool g_lockPathsActive = true;

// Buffers with a recorded (deferred) lock the render thread may not have run yet, and the queue
// position after it (kUnknown while it sits in an overlap side buffer).
constexpr uint64_t kUnknown = ~uint64_t{0};
struct InFlightBuffer {
    void* buffer;
    uint64_t pos;
};
InFlightBuffer g_inFlight[64];
int g_inFlightCount = 0;
SRWLOCK g_inFlightLock = SRWLOCK_INIT;

void DropReachedLocked() {
    for (int i = 0; i < g_inFlightCount;)
        if (g_inFlight[i].pos != kUnknown && rt::QueueReached(g_inFlight[i].pos))
            g_inFlight[i] = g_inFlight[--g_inFlightCount];
        else
            ++i;
}

bool InFlight(void* s) {
    if (!g_inFlightCount) return false;
    AcquireSRWLockExclusive(&g_inFlightLock);
    DropReachedLocked();
    bool found = false;
    for (int i = 0; i < g_inFlightCount && !found; ++i) found = g_inFlight[i].buffer == s;
    ReleaseSRWLockExclusive(&g_inFlightLock);
    return found;
}

// Room to track one more buffer (or it is tracked already).
bool HasRoom(void* s) {
    AcquireSRWLockExclusive(&g_inFlightLock);
    bool ok = g_inFlightCount < 64;
    if (!ok) DropReachedLocked(), ok = g_inFlightCount < 64;
    for (int i = 0; i < g_inFlightCount && !ok; ++i) ok = g_inFlight[i].buffer == s;
    ReleaseSRWLockExclusive(&g_inFlightLock);
    return ok;
}

void MarkInFlight(void* s, uint64_t pos) {
    AcquireSRWLockExclusive(&g_inFlightLock);
    bool found = false;
    for (int i = 0; i < g_inFlightCount && !found; ++i)
        if (g_inFlight[i].buffer == s) g_inFlight[i].pos = pos, found = true;
    if (!found && g_inFlightCount < 64) g_inFlight[g_inFlightCount++] = {s, pos};
    ReleaseSRWLockExclusive(&g_inFlightLock);
}

void OnSideAppended(uint64_t head) {
    AcquireSRWLockExclusive(&g_inFlightLock);
    for (int i = 0; i < g_inFlightCount; ++i)
        if (g_inFlight[i].pos == kUnknown) g_inFlight[i].pos = head;
    ReleaseSRWLockExclusive(&g_inFlightLock);
}

// Lock call sites whose code was read (module + offset of the return address), registered by
// the research code:
//  - write-only range: writes its whole locked range, never reads it -> deferred like DISCARD;
//  - CPU-only texture: a managed or dynamic texture only the CPU writes, used by draws recorded
//    after the lock -> locked directly, no drain.
struct VerifiedSite {
    char module[64];
    uintptr_t ret;
    LockSiteKind kind;
    uintptr_t base;
    sites::Fingerprint fp;  // fp.length != 0: ret is relative to the fingerprinted function
    bool dead;              // fingerprint not found in the loaded module: never matches
};
VerifiedSite g_sites[16];
int g_siteCount = 0;

bool CallerIs(void* ret, LockSiteKind kind) {
    auto r = reinterpret_cast<uintptr_t>(ret);
    for (int i = 0; i < g_siteCount; ++i) {
        VerifiedSite& v = g_sites[i];
        if (v.kind != kind) continue;
        if (v.dead) continue;
        if (!v.base) {
            HMODULE m = GetModuleHandleA(v.module);
            if (!m) continue;
            if (v.fp.length) {  // resolved once, when the module is first seen
                void* fn = sites::Find(m, v.fp);
                if (!fn) {
                    v.dead = true;
                    Log("d3d: lock site in %s not found (module version changed?); not verified", v.module);
                    continue;
                }
                v.ret += reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(m);
                v.fp.length = 0;
                Log("d3d: lock site in %s located by fingerprint", v.module);
            }
            v.base = reinterpret_cast<uintptr_t>(m);
        }
        if (r == v.base + v.ret) return true;
    }
    return false;
}
// 1: a write-only range site (needs an explicit range), 2: a fill-only caller of a lock helper
// (whole buffer allowed), 0: neither.
int WriteOnlyCaller(void* ret) {
    if (CallerIs(ret, LockSiteKind::WriteOnlyRange)) return 1;
    if (!CallerIs(ret, LockSiteKind::LockWrapper)) return 0;
    // A generic helper: the decision is its caller's (the frame above the lock call's caller).
    void* frames[3] = {};
    USHORT got = RtlCaptureStackBackTrace(1, 3, frames, nullptr);
    for (USHORT i = 0; i + 1 < got; ++i)
        if (frames[i] == ret) return CallerIs(frames[i + 1], LockSiteKind::WriteOnlyViaWrapper) ? 2 : 0;
    return 0;
}

constexpr size_t kStagingSlack = 4096;
constexpr uint8_t kGuardByte = 0xA5;
volatile long g_overrunsLogged = 0;

// (measurement) A buffer lock on a side recorder that is neither deferred nor direct: it will be
// a direct call out of queue order. Logs each distinct call site once.
void NoteSideLock(void* ret, bool vertex, DWORD f) {
    if (!rt::Recording() || rt::OwnsQueue()) return;
    static void* seen[32];
    static volatile long count = 0;
    for (long i = 0; i < count && i < 32; ++i)
        if (seen[i] == ret) return;
    long k = InterlockedIncrement(&count) - 1;
    if (k >= 32) return;
    seen[k] = ret;
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(ret), &m);
    char name[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    // The engine frames above it (rvas), to find the caller that decides what is written.
    void* f8[10] = {};
    USHORT got = RtlCaptureStackBackTrace(1, 10, f8, nullptr);
    char chain[200];
    int n = 0;
    for (USHORT i = 0; i < got && n < 180; ++i) {
        auto v = reinterpret_cast<uint8_t*>(f8[i]);
        if (m && v >= reinterpret_cast<uint8_t*>(m) && v < reinterpret_cast<uint8_t*>(m) + 0x2000000)
            n += sprintf_s(chain + n, sizeof chain - n, " %llx", static_cast<unsigned long long>(v - reinterpret_cast<uint8_t*>(m)));
    }
    chain[n] = 0;
    Log("d3d: side-recorder %s lock flags %lx not deferred, call site %s+0x%llx; engine frames%s", vertex ? "VB" : "IB", f,
        base ? base + 1 : name,
        static_cast<unsigned long long>(static_cast<uint8_t*>(ret) - reinterpret_cast<uint8_t*>(m)), chain);
}

bool DeferLock(void* s, UINT o, UINT n, void** p, DWORD f, bool vertex, void* ret = nullptr) {
    if (!g_deferLocks || !g_lockPathsActive || g_opt.renderThread < 2 || !rt::Recording() || (f & D3DLOCK_READONLY) || !p)
        return false;
    UINT size = 0;
    DWORD usage = 0;
    if (vertex) {
        D3DVERTEXBUFFER_DESC d{};
        if (FAILED(static_cast<IDirect3DVertexBuffer9*>(s)->GetDesc(&d))) return false;
        size = d.Size, usage = d.Usage;
    } else {
        D3DINDEXBUFFER_DESC d{};
        if (FAILED(static_cast<IDirect3DIndexBuffer9*>(s)->GetDesc(&d))) return false;
        size = d.Size, usage = d.Usage;
    }
    // Only a discarding lock may hand out uninitialised memory: the staging copy is written back
    // whole, so any other lock (NOOVERWRITE appends, partial writes) would clobber live data.
    (void)usage;
    // A NOOVERWRITE lock with an explicit range that follows a deferred lock of the same buffer
    // is deferred too (it must not reach the storage that lock is about to replace); the app
    // writes its whole range.
    bool follow = (f & D3DLOCK_NOOVERWRITE) && n && g_directNoOverwrite && InFlight(s);
    // (n == 0, the whole buffer, only for a fill-only caller of a lock helper.)
    int site = !(f & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD)) && ret ? WriteOnlyCaller(ret) : 0;
    bool writeOnly = (site == 1 && n) || site == 2;
    if (!((f & D3DLOCK_DISCARD) || follow || writeOnly) || o >= size || t_pendingCount == 8 || !HasRoom(s)) return false;
    for (int i = 0; i < t_pendingCount; ++i)
        if (t_pending[i].buffer == s) return false;
    UINT len = n ? n : size - o;
    if (len > size - o) len = size - o;
    // Slack after the range: code that writes past what it locked (harmless on a real lock, which
    // maps the whole buffer) must not corrupt the heap. The guard pattern at its end tells.
    auto* data = static_cast<uint8_t*>(malloc(len + kStagingSlack));
    if (!data) return false;
    memset(data + len, kGuardByte, kStagingSlack);
    Originals& lockOrig = vertex ? g_vbLock : g_ibLock;
    Originals& unlockOrig = vertex ? g_vbUnlock : g_ibUnlock;
    auto unlock = reinterpret_cast<BufUnlockFn>(unlockOrig.For(s));
    if (!unlock) {
        free(data);
        return false;
    }
    t_pending[t_pendingCount++] = {s, data, o, len, f, reinterpret_cast<BufLockFn>(lockOrig.For(s)), unlock};
    *p = data;
    InterlockedIncrement(&g_deferredLocks);
    return true;
}

HRESULT BufUnlock(void* s, Originals& orig) {
    for (int i = 0; i < t_pendingCount; ++i) {
        if (t_pending[i].buffer != s) continue;
        PendingLock pl = t_pending[i];
        t_pending[i] = t_pending[--t_pendingCount];
        size_t over = 0;
        for (size_t k = 0; k < kStagingSlack; ++k)
            if (pl.data[pl.size + k] != kGuardByte) over = k + 1;
        if (over && InterlockedIncrement(&g_overrunsLogged) <= 10)
            Log("d3d: deferred lock written %zu byte(s) past its range (buffer %p, offset %u, size %u, flags %lx)", over,
                s, pl.offset, pl.size, pl.flags);
        rt::KeepAlive(static_cast<IUnknown*>(s));
        rt::Enqueue([](const uint8_t* b) {
            auto& q = *reinterpret_cast<const PendingLock*>(b);
            void* dst = nullptr;
            if (SUCCEEDED(q.lock(q.buffer, q.offset, q.size, &dst, q.flags)) && dst) {
                memcpy(dst, q.data, q.size);
                q.unlock(q.buffer);
            }
            free(q.data);
        }, &pl, sizeof pl);
        MarkInFlight(s, rt::OwnsQueue() ? rt::QueueHead() : kUnknown);
        return D3D_OK;
    }
    return reinterpret_cast<BufUnlockFn>(orig.For(s))(s);
}

volatile long g_directLocks = 0;

// A NOOVERWRITE lock writes only where no queued draw reads: no drain needed, as long as no
// recorded (deferred) lock of the buffer is still queued and the caller records into the queue
// itself (not an overlap side buffer, which joins first).
bool LockDirect(void* s, DWORD f) {
    if (!g_directNoOverwrite || !g_lockPathsActive || g_opt.renderThread < 2) return false;
    if (!(f & D3DLOCK_NOOVERWRITE) || (f & D3DLOCK_DISCARD) || !rt::OwnsQueue() || InFlight(s)) return false;
    InterlockedIncrement(&g_directLocks);
    return true;
}

HRESULT STDMETHODCALLTYPE VBLock(void* s, UINT o, UINT n, void** p, DWORD f) {
    CountLock(kVB, f);
    if (LockDirect(s, f)) return reinterpret_cast<BufLockFn>(g_vbLock.For(s))(s, o, n, p, f);
    if (DeferLock(s, o, n, p, f, true, _ReturnAddress())) return D3D_OK;
    NoteSideLock(_ReturnAddress(), true, f);
    BeforeLock(0, f);
    return reinterpret_cast<BufLockFn>(g_vbLock.For(s))(s, o, n, p, f);
}
HRESULT STDMETHODCALLTYPE IBLock(void* s, UINT o, UINT n, void** p, DWORD f) {
    CountLock(kIB, f);
    if (LockDirect(s, f)) return reinterpret_cast<BufLockFn>(g_ibLock.For(s))(s, o, n, p, f);
    if (DeferLock(s, o, n, p, f, false, _ReturnAddress())) return D3D_OK;
    NoteSideLock(_ReturnAddress(), false, f);
    BeforeLock(1, f); return reinterpret_cast<BufLockFn>(g_ibLock.For(s))(s, o, n, p, f);
}
HRESULT STDMETHODCALLTYPE VBUnlock(void* s) { return BufUnlock(s, g_vbUnlock); }
HRESULT STDMETHODCALLTYPE IBUnlock(void* s) { return BufUnlock(s, g_ibUnlock); }
HRESULT STDMETHODCALLTYPE TexLock(void* s, UINT l, D3DLOCKED_RECT* r, const RECT* rc, DWORD f) {
    CountLock(kTex, f); BeforeLock(2, f); return reinterpret_cast<TexLockFn>(g_texLock.For(s))(s, l, r, rc, f);
}
HRESULT STDMETHODCALLTYPE CubeLock(void* s, D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT* r, const RECT* rc, DWORD f) {
    CountLock(kTex, f); BeforeLock(2, f); return reinterpret_cast<CubeLockFn>(g_cubeLock.For(s))(s, face, l, r, rc, f);
}
HRESULT STDMETHODCALLTYPE SurfLock(void* s, D3DLOCKED_RECT* r, const RECT* rc, DWORD f) {
    CountLock(kTex, f);
    if (g_opt.renderThread >= 2 && CallerIs(_ReturnAddress(), LockSiteKind::CpuOnlyTexture)) {
        D3DSURFACE_DESC d{};
        // Managed (the CPU copy is written) or dynamic (the driver syncs CPU access with the GPU).
        if (SUCCEEDED(static_cast<IDirect3DSurface9*>(s)->GetDesc(&d)) &&
            (d.Pool == D3DPOOL_MANAGED || (d.Usage & D3DUSAGE_DYNAMIC)))
            return reinterpret_cast<SurfLockFn>(g_surfLock.For(s))(s, r, rc, f);
    }
    BeforeLock(3, f); return reinterpret_cast<SurfLockFn>(g_surfLock.For(s))(s, r, rc, f);
}

template <typename Fn>
void HookSlot(void* object, int slot, Originals& originals, Fn hook) {
    if (!(g_opt.census || g_opt.renderThread >= 2) || !object) return;
    void** vt = *static_cast<void***>(object);
    if (vt[slot] == reinterpret_cast<void*>(hook) || originals.For(object)) return;
    AcquireSRWLockExclusive(&g_hookLock);
    if (!originals.For(object) && vt[slot] != reinterpret_cast<void*>(hook)) {
        int i = originals.n.load(std::memory_order_relaxed);
        if (i == 32) {
            Log("d3d: too many resource classes; lock hook missing");
        } else {
            originals.vt[i] = vt;
            originals.fn[i] = vt[slot];
            originals.n.store(i + 1, std::memory_order_release);  // entry complete before it is visible
            WriteSlot(&vt[slot], reinterpret_cast<void*>(hook));
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

// Surfaces handed out by textures get their LockRect hooked too.
HRESULT STDMETHODCALLTYPE GetSurfaceLevel(void* tex, UINT level, IDirect3DSurface9** out) {
    HRESULT hr = reinterpret_cast<SurfLevelFn>(g_surfLevel.For(tex))(tex, level, out);
    if (SUCCEEDED(hr) && out && *out) HookSlot(*out, 13, g_surfLock, &SurfLock);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f,
                                        D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) {
    Count(kDev, kCreateTexture);
    HRESULT hr = Orig<decltype(&CreateTexture)>(kDev, kCreateTexture)(d, w, h, l, u, f, p, t, sh);
    if (SUCCEEDED(hr) && (u & D3DUSAGE_RENDERTARGET)) resolve::NoteRenderTarget(w, h);
    if (SUCCEEDED(hr)) {
        HookSlot(*t, 19, g_texLock, &TexLock);
        HookSlot(*t, 18, g_surfLevel, &GetSurfaceLevel);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateCubeTexture(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
                                            IDirect3DCubeTexture9** t, HANDLE* sh) {
    Count(kDev, kCreateCubeTexture);
    HRESULT hr = Orig<decltype(&CreateCubeTexture)>(kDev, kCreateCubeTexture)(d, e, l, u, f, p, t, sh);
    if (SUCCEEDED(hr)) HookSlot(*t, 19, g_cubeLock, &CubeLock);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p,
                                                      IDirect3DSurface9** s, HANDLE* sh) {
    Count(kDev, kCreateOffscreen);
    HRESULT hr = Orig<decltype(&CreateOffscreenPlainSurface)>(kDev, kCreateOffscreen)(d, w, h, f, p, s, sh);
    if (SUCCEEDED(hr)) HookSlot(*s, 13, g_surfLock, &SurfLock);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateVB(IDirect3DDevice9* d, UINT n, DWORD u, DWORD fvf, D3DPOOL p,
                                   IDirect3DVertexBuffer9** vb, HANDLE* sh) {
    Count(kDev, kCreateVB);
    HRESULT hr = Orig<decltype(&CreateVB)>(kDev, kCreateVB)(d, n, u, fvf, p, vb, sh);
    if (SUCCEEDED(hr)) {
        HookSlot(*vb, 11, g_vbLock, &VBLock);
        HookSlot(*vb, 12, g_vbUnlock, &VBUnlock);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateIB(IDirect3DDevice9* d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p,
                                   IDirect3DIndexBuffer9** ib, HANDLE* sh) {
    Count(kDev, kCreateIB);
    HRESULT hr = Orig<decltype(&CreateIB)>(kDev, kCreateIB)(d, n, u, f, p, ib, sh);
    if (SUCCEEDED(hr)) {
        HookSlot(*ib, 11, g_ibLock, &IBLock);
        HookSlot(*ib, 12, g_ibUnlock, &IBUnlock);
    }
    return hr;
}

// --- GPU queries ---------------------------------------------------------------------------------
// The engine uses queries directly; with a render thread, Issue must follow the recorded
// draws it measures, and GetData must not race the render thread inside the runtime.
using QueryIssueFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, DWORD);
using QueryGetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);
// Issue is recorded; while an Issue is still queued, GetData answers S_FALSE ("not ready
// yet"), which D3D9 allows at any time, so the game thread never waits for the queue.
std::atomic<int>* PendingIssues(IDirect3DQuery9* q) {
    static std::unordered_map<IDirect3DQuery9*, std::atomic<int>*> pending;  // game thread only
    auto& slot = pending[q];
    if (!slot) slot = new std::atomic<int>(0);  // tiny, lives as long as the process
    return slot;
}

HRESULT STDMETHODCALLTYPE QueryIssue(IDirect3DQuery9* q, DWORD f) {
    auto issue = reinterpret_cast<QueryIssueFn>(g_queryIssue.For(q));
    if (rt::Recording()) {
        rt::KeepAlive(q);
        struct Call { IDirect3DQuery9* q; DWORD f; QueryIssueFn fn; std::atomic<int>* pending; };
        Call call{q, f, issue, PendingIssues(q)};
        call.pending->fetch_add(1);
        rt::Enqueue([](const uint8_t* p) {
            auto& c = *reinterpret_cast<const Call*>(p);
            c.fn(c.q, c.f);
            c.pending->fetch_sub(1);
        }, &call, sizeof call);
        return D3D_OK;
    }
    rt::SyncForDirectAccess(rt::kSyncQuery);
    return issue(q, f);
}
// (glitch detection) occlusion-count reads: a 0 between two visible results makes whatever the
// query gates (a lamp's flare) vanish for a frame. Counted per query, any thread.
struct QueryTrack { DWORD last = ~0u, before = ~0u; uint32_t flips = 0, reads = 0; DWORD lo = ~0u, hi = 0; };
SRWLOCK g_queryTrackLock = SRWLOCK_INIT;
volatile LONG64 g_queryReads = 0, g_queryNotReady = 0, g_queryBlips = 0;
void TrackQuery(IDirect3DQuery9* q, HRESULT hr, const void* data, DWORD size) {
    if (q->GetType() != D3DQUERYTYPE_OCCLUSION) return;
    InterlockedIncrement64(&g_queryReads);
    if (hr == S_FALSE) return void(InterlockedIncrement64(&g_queryNotReady));
    if (hr != S_OK || size != 4 || !data) return;
    DWORD v = *static_cast<const DWORD*>(data);
    static std::unordered_map<IDirect3DQuery9*, QueryTrack> tracks;
    AcquireSRWLockExclusive(&g_queryTrackLock);
    QueryTrack& t = tracks[q];
    if (t.before != ~0u && t.before > 0 && t.last == 0 && v > 0) InterlockedIncrement64(&g_queryBlips);
    // A-B-A: the count swings back and forth (a gated lamp glow that alternates).
    if (t.before != ~0u && v == t.before && v != t.last && (v > t.last ? v - t.last : t.last - v) * 4 > (v > t.last ? v : t.last)) ++t.flips;
    ++t.reads, t.lo = v < t.lo ? v : t.lo, t.hi = v > t.hi ? v : t.hi;
    t.before = t.last, t.last = v;
    LONG64 reads = g_queryReads;
    size_t queries = tracks.size();
    ReleaseSRWLockExclusive(&g_queryTrackLock);
    if (reads % 20000 == 0) {
        Log("glitch: occlusion queries: %lld reads, %lld not ready, %lld one-read drops to zero (%zu queries)", reads,
            static_cast<long long>(g_queryNotReady), static_cast<long long>(g_queryBlips), queries);
        AcquireSRWLockExclusive(&g_queryTrackLock);
        int logged = 0;
        for (auto& [query, tr] : tracks)
            if (tr.flips * 4 > tr.reads && tr.reads > 20 && logged++ < 10)
                Log("glitch: query %p swings: %u of %u reads, counts %lu..%lu", static_cast<void*>(query), tr.flips, tr.reads, tr.lo, tr.hi);
        for (auto& [query, tr] : tracks) tr.flips = tr.reads = 0, tr.lo = ~0u, tr.hi = 0;
        ReleaseSRWLockExclusive(&g_queryTrackLock);
    }
}

HRESULT STDMETHODCALLTYPE QueryGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD f) {
    auto getData = reinterpret_cast<QueryGetDataFn>(g_queryGetData.For(q));
    HRESULT hr;
    if (rt::Recording()) {
        if (PendingIssues(q)->load() > 0) hr = S_FALSE;
        else hr = getData(q, data, size, f);  // runtime lock (MULTITHREADED) serializes with the render thread
    } else {
        rt::SyncForDirectAccess(rt::kSyncQuery);
        hr = getData(q, data, size, f);
    }
    if (glitch::Enabled() || g_opt.queryTrack) TrackQuery(q, hr, data, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateQuery(IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** q) {
    Count(kDev, kCreateQuery);
    HRESULT hr = Orig<decltype(&CreateQuery)>(kDev, kCreateQuery)(d, t, q);
    if (SUCCEEDED(hr) && q && *q) {
        HookSlot(*q, 6, g_queryIssue, &QueryIssue);      // Issue
        HookSlot(*q, 7, g_queryGetData, &QueryGetData);  // GetData
    }
    return hr;
}

IDirect3DDevice9* g_device = nullptr;
IDirect3DDevice9* g_gameDevice = nullptr;  // the device the engine got (the render thread's wrapper or the real one)

// --- lens watch (diagnosis of the signal lens flicker at Haarlem, 2026-11-20) -----------------
// The lens part of that signal head (234 triangles) and its body (12,048 triangles) use the same
// shader and vertex buffer. At lens 2 three surfaces lie at the same place: two layers of the lens
// part (inside one draw) and a group of body triangles. Which one wins a sample depends on the
// rounding of the depth value, and that changes when the camera matrix changes in its last
// digits, as it does while the train stands (see work-items/doing/zfight-wobble-haarlem.md).
// [experiments] lens_watch=1: per frame, the samples that passed the depth test for each G-buffer
// draw of lens_watch_tris_a / _b triangles (234 / 12048), with hashes of the second kind's vertex
// shader constants. The device waits for each of those draws.
// [experiments] lens_pick_key=VK with lens_pick_x/y/w/h (back buffer pixels) and lens_pick_frames:
// a region pick (see LensPick). lens_pick_bias=N: in the second half of a pick the small part is
// drawn with a depth bias of N steps.
// [graphics] small_part_depth_bias (depth steps of a 24-bit buffer, default 0 = off),
// small_part_slope_bias (x100), small_part_ratio (default 8), small_part_toggle_key: a test, not
// a fix. In the G-buffer pass of the object shaders (Train*.fx, technique DEFERRED*), a draw with
// at most 1/small_part_ratio of the triangles of the largest draw from the same vertex buffer
// gets a depth bias towards the camera. With 32 steps and slope 1 the lens flicker stayed (two
// of the surfaces are in one draw), a third of all object draws were biased, and the unlit lens
// covered the lit lamp. Keep it off.
int g_lensInit = 0, g_lensToggleKey = 0, g_lensWatch = 0;
UINT g_lensTrisA = 0, g_lensTrisB = 0, g_lensRatio = 8;
float g_lensBias = 0, g_lensSlope = 0;
volatile bool g_lensOn = true;
volatile LONG g_lensGen = 1;  // counts effect creations (the per-thread technique cache follows it)
SRWLOCK g_lensLock = SRWLOCK_INIT;
std::unordered_map<void*, bool> g_lensObject;  // effect -> an object shader (under g_lensLock)
struct LensThread {
    bool active = false;  // inside Begin/End of an object shader's G-buffer technique
    LONG gen = 0;
    struct Technique { bool active; const char* name; };
    std::unordered_map<uint64_t, Technique> techniques;  // effect and technique -> active, name
    void* fx = nullptr;
    const char* technique = nullptr;
    std::unordered_map<void*, UINT> largest[2];         // vertex buffer -> most triangles in one draw (this period, the one before)
    unsigned draws = 0;
};
thread_local LensThread t_lens;
struct LensDraw { UINT prims; DWORD px; bool biased; uint32_t hash[4]; };
std::vector<LensDraw> g_lensDraws;
DWORD g_lensThread = 0;
unsigned g_lensActiveDraws = 0, g_lensBiasedDraws = 0;
IDirect3DQuery9* g_lensQuery = nullptr;
int g_lensPickKey = 0, g_lensPickPerPress = 0, g_lensPickFrames = 0, g_lensPickBias = 0;
RECT g_lensPickRect{};
void LensInit() {
    if (g_lensInit) return;
    g_lensInit = 1;
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
    g_lensBias = -static_cast<int>(GetPrivateProfileIntA("graphics", "small_part_depth_bias", 0, ini)) / 16777216.0f;
    g_lensSlope = -static_cast<int>(GetPrivateProfileIntA("graphics", "small_part_slope_bias", 0, ini)) / 100.0f;
    g_lensRatio = GetPrivateProfileIntA("graphics", "small_part_ratio", 8, ini);
    if (g_lensRatio < 2) g_lensRatio = 2;
    g_lensToggleKey = GetPrivateProfileIntA("graphics", "small_part_toggle_key", 0, ini);
    if (!g_lensToggleKey) g_lensToggleKey = GetPrivateProfileIntA("experiments", "small_part_toggle_key", 0, ini);  // the same switch for test runs
    g_lensWatch = GetPrivateProfileIntA("experiments", "lens_watch", 0, ini);
    g_lensTrisA = GetPrivateProfileIntA("experiments", "lens_watch_tris_a", 234, ini);
    g_lensTrisB = GetPrivateProfileIntA("experiments", "lens_watch_tris_b", 12048, ini);
    g_lensPickKey = GetPrivateProfileIntA("experiments", "lens_pick_key", 0, ini);
    g_lensPickPerPress = GetPrivateProfileIntA("experiments", "lens_pick_frames", 30, ini);
    g_lensPickBias = GetPrivateProfileIntA("experiments", "lens_pick_bias", 0, ini);
    g_lensPickRect.left = GetPrivateProfileIntA("experiments", "lens_pick_x", 0, ini);
    g_lensPickRect.top = GetPrivateProfileIntA("experiments", "lens_pick_y", 0, ini);
    g_lensPickRect.right = g_lensPickRect.left + GetPrivateProfileIntA("experiments", "lens_pick_w", 1, ini);
    g_lensPickRect.bottom = g_lensPickRect.top + GetPrivateProfileIntA("experiments", "lens_pick_h", 1, ini);
    Log("small parts: depth bias %g, slope %g, for draws of at most 1/%u of their model's largest; toggle key %d, watch %d", g_lensBias,
        g_lensSlope, g_lensRatio, g_lensToggleKey, g_lensWatch);
}
bool LensWanted() {
    LensInit();
    return g_lensBias != 0 || g_lensSlope != 0 || g_lensWatch;
}
// Each effect file as it is created.
void LensNoteEffect(void* fx, const char* file) {
    if (!LensWanted()) return;
    bool object = !_strnicmp(file, "Train", 5) && !strstr(file, "Flora") && !strstr(file, "ViewFacing") && !strstr(file, "SkyDome") &&
                  !strstr(file, "Particle") && !strstr(file, "post") && !strstr(file, "Shadow");
    AcquireSRWLockExclusive(&g_lensLock);
    g_lensObject[fx] = object;
    ReleaseSRWLockExclusive(&g_lensLock);
    InterlockedIncrement(&g_lensGen);
}
struct LensTechniqueDesc { const char* name; UINT passes, annotations; };
// After an effect's Begin (on the thread that drives the device).
void LensBegin(void* fx) {
    LensThread& t = t_lens;
    if (t.gen != g_lensGen) t.gen = g_lensGen, t.techniques.clear();
    void** vt = *static_cast<void***>(fx);
    const char* h = reinterpret_cast<const char* (STDMETHODCALLTYPE*)(void*)>(vt[59])(fx);  // GetCurrentTechnique
    uint64_t key = reinterpret_cast<uint64_t>(fx) * 31 ^ reinterpret_cast<uint64_t>(h);
    auto it = t.techniques.find(key);
    if (it == t.techniques.end()) {
        AcquireSRWLockShared(&g_lensLock);
        auto o = g_lensObject.find(fx);
        bool object = o != g_lensObject.end() && o->second;
        ReleaseSRWLockShared(&g_lensLock);
        LensTechniqueDesc desc{};
        bool named = SUCCEEDED(reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(void*, const char*, LensTechniqueDesc*)>(vt[5])(fx, h, &desc)) && desc.name;  // GetTechniqueDesc
        if (!named) desc.name = nullptr;
        bool active = object && named && !strncmp(desc.name, "DEFERRED", 8);
        it = t.techniques.emplace(key, LensThread::Technique{active, desc.name}).first;
    }
    t.active = it->second.active, t.fx = fx, t.technique = it->second.name;
}
uint32_t LensHash(const float* p, int floats) {
    uint32_t x = 2166136261u;
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    for (int i = 0; i < floats * 4; ++i) x = (x ^ b[i]) * 16777619u;
    return x;
}
// (lens watch) Region pick: for lens_pick_frames frames after the pick key, every indexed draw runs
// under a scissor of the region lens_pick_x/y/w/h (back buffer pixels) inside an occlusion query;
// each draw that puts samples there is logged in order, with its shader, depth states and transform.
UINT g_lensBackW = 0;
unsigned g_lensPickDraw = 0, g_lensFrame = 0;
const char* FxFileName(void* fx);
HRESULT LensPick(IDirect3DDevice9* d, D3DPRIMITIVETYPE ty, INT base, UINT minIndex, UINT verts, UINT start, UINT prims) {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    Fn orig = Orig<Fn>(kDev, 82);
    ++g_lensPickDraw;
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) rt->GetDesc(&desc), rt->Release();
    if (!g_lensBackW || desc.Width * 9 != desc.Height * 16 || desc.Width < g_lensBackW ||
        (!g_lensQuery && FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_lensQuery))))
        return orig(d, ty, base, minIndex, verts, start, prims);
    LONG k = static_cast<LONG>(desc.Width / g_lensBackW);
    RECT r{g_lensPickRect.left * k, g_lensPickRect.top * k, g_lensPickRect.right * k, g_lensPickRect.bottom * k}, old{};
    DWORD enable = FALSE;
    d->GetRenderState(D3DRS_SCISSORTESTENABLE, &enable);
    d->GetScissorRect(&old);
    d->SetScissorRect(&r);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    HRESULT hr = D3D_OK;
    DWORD px = 0;
    if ((prims == g_lensTrisA || prims == g_lensTrisB) && (ty == D3DPT_TRIANGLELIST || ty == D3DPT_TRIANGLESTRIP)) {
        // The two parts triangle by triangle (the same result as one draw): which triangles put samples in the region.
        // In the second half of the pick the small part is drawn with the depth bias lens_pick_bias (steps).
        bool biased = prims == g_lensTrisA && g_lensPickBias && g_lensPickFrames <= g_lensPickPerPress / 2;
        DWORD oldBias = 0;
        float bias = -g_lensPickBias / 16777216.0f;
        if (biased) d->GetRenderState(D3DRS_DEPTHBIAS, &oldBias), d->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<DWORD*>(&bias));
        static char line[60000];
        int n = sprintf_s(line, "lenstris: f %u #%u prims %u type %d bias %d |", g_lensFrame, g_lensPickDraw, prims, static_cast<int>(ty), biased ? g_lensPickBias : 0);
        DWORD cull = D3DCULL_NONE;
        d->GetRenderState(D3DRS_CULLMODE, &cull);
        bool strip = ty == D3DPT_TRIANGLESTRIP;
        for (UINT i = 0; i < prims; ++i) {
            // (in a strip every second triangle has the other winding)
            if (strip && cull != D3DCULL_NONE) d->SetRenderState(D3DRS_CULLMODE, (i & 1) ? (cull == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW) : cull);
            g_lensQuery->Issue(D3DISSUE_BEGIN);
            hr = orig(d, D3DPT_TRIANGLELIST, base, minIndex, verts, strip ? start + i : start + 3 * i, 1);
            g_lensQuery->Issue(D3DISSUE_END);
            DWORD one = 0;
            for (int k2 = 0; k2 < 100000 && g_lensQuery->GetData(&one, sizeof one, D3DGETDATA_FLUSH) == S_FALSE; ++k2) {}
            if (one && n < 59900) n += sprintf_s(line + n, sizeof line - n, " %u:%lu", i, one);
            px += one;
        }
        if (strip) d->SetRenderState(D3DRS_CULLMODE, cull);
        if (biased) d->SetRenderState(D3DRS_DEPTHBIAS, oldBias);
        Log("%s", line);
    } else {
        g_lensQuery->Issue(D3DISSUE_BEGIN);
        hr = orig(d, ty, base, minIndex, verts, start, prims);
        g_lensQuery->Issue(D3DISSUE_END);
        for (int i = 0; i < 100000 && g_lensQuery->GetData(&px, sizeof px, D3DGETDATA_FLUSH) == S_FALSE; ++i) {}
    }
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, enable);
    d->SetScissorRect(&old);
    if (px) {
        DWORD zf = 0, zw = 0, ze = 0, bias = 0, cull = 0, ate = 0;
        d->GetRenderState(D3DRS_ZFUNC, &zf), d->GetRenderState(D3DRS_ZWRITEENABLE, &zw), d->GetRenderState(D3DRS_ZENABLE, &ze);
        d->GetRenderState(D3DRS_DEPTHBIAS, &bias), d->GetRenderState(D3DRS_CULLMODE, &cull), d->GetRenderState(D3DRS_ALPHATESTENABLE, &ate);
        float c[16] = {};
        d->GetVertexShaderConstantF(0, c, 4);
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT offset = 0, stride = 0;
        if (SUCCEEDED(d->GetStreamSource(0, &vb, &offset, &stride)) && vb) vb->Release();
        IDirect3DBaseTexture9* tex = nullptr;
        if (SUCCEEDED(d->GetTexture(0, &tex)) && tex) tex->Release();
        const LensThread& t = t_lens;
        Log("lenspick: f %u #%u type %d px %lu rt %u fx %s tech %s prims %u start %u vb %p tex %p z %lu/%lu/%lu bias %08lx cull %lu atest %lu | %.7g %.7g %.7g %.7g | %.7g %.7g %.7g %.7g | "
            "%.7g %.7g %.7g %.7g | %.7g %.7g %.7g %.7g",
            g_lensFrame, g_lensPickDraw, static_cast<int>(ty), px, desc.Width, t.fx ? FxFileName(t.fx) : "-", t.fx && t.technique ? t.technique : "-", prims, start,
            static_cast<void*>(vb), static_cast<void*>(tex), ze, zf, zw, bias, cull, ate, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        if (prims == g_lensTrisA || prims == g_lensTrisB) {  // the whole input of the two parts, to compare frame against frame
            static char line[6000];
            float v[64] = {}, q[32] = {};
            d->GetVertexShaderConstantF(0, v, 16);
            d->GetPixelShaderConstantF(0, q, 8);
            int n = sprintf_s(line, "lensstate: f %u prims %u px %lu vs", g_lensFrame, prims, px);
            for (int i = 0; i < 64; ++i) n += sprintf_s(line + n, sizeof line - n, " %.9g", v[i]);
            n += sprintf_s(line + n, sizeof line - n, " | ps");
            for (int i = 0; i < 32; ++i) n += sprintf_s(line + n, sizeof line - n, " %.9g", q[i]);
            uint32_t h = 2166136261u;
            n += sprintf_s(line + n, sizeof line - n, " | rs");
            for (int rs = 7; rs < 210; ++rs) {
                DWORD x = 0;
                if (FAILED(d->GetRenderState(D3DRENDERSTATETYPE(rs), &x))) x = 0;
                h = (h ^ x) * 16777619u;
                if (rs == 15 || rs == 24 || rs == 25 || rs == 27 || rs == 19 || rs == 20) n += sprintf_s(line + n, sizeof line - n, " %d=%lx", rs, x);
            }
            n += sprintf_s(line + n, sizeof line - n, " all=%08x | samplers", h);
            for (DWORD st = 0; st < 4; ++st) {
                IDirect3DBaseTexture9* t2 = nullptr;
                if (SUCCEEDED(d->GetTexture(st, &t2)) && t2) t2->Release();
                n += sprintf_s(line + n, sizeof line - n, " s%lu %p", st, static_cast<void*>(t2));
                if (t2 && t2->GetType() == D3DRTYPE_TEXTURE) {  // the detail level a managed texture is held at (the engine streams textures)
                    D3DSURFACE_DESC td{};
                    static_cast<IDirect3DTexture9*>(t2)->GetLevelDesc(0, &td);
                    n += sprintf_s(line + n, sizeof line - n, " lod %lu of %lu %ux%u fmt %d pool %d", t2->GetLOD(), t2->GetLevelCount(), td.Width, td.Height,
                                   static_cast<int>(td.Format), static_cast<int>(td.Pool));
                }
                for (int y = 1; y <= 13; ++y) {
                    DWORD x = 0;
                    d->GetSamplerState(st, D3DSAMPLERSTATETYPE(y), &x);
                    n += sprintf_s(line + n, sizeof line - n, " %lx", x);
                }
            }
            IDirect3DVertexShader9* vsh = nullptr;
            IDirect3DPixelShader9* psh = nullptr;
            if (SUCCEEDED(d->GetVertexShader(&vsh)) && vsh) vsh->Release();
            if (SUCCEEDED(d->GetPixelShader(&psh)) && psh) psh->Release();
            sprintf_s(line + n, sizeof line - n, " | shaders %p %p base %d min %u verts %u", static_cast<void*>(vsh), static_cast<void*>(psh), base, minIndex, verts);
            Log("%s", line);
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE LensDrawIndexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE ty, INT base, UINT minIndex, UINT verts, UINT start, UINT prims) {
    LensThread& t = t_lens;
    postfx::OnDraw(d);
    if (g_lensPickFrames > 0 && GetCurrentThreadId() == g_lensThread) return LensPick(d, ty, base, minIndex, verts, start, prims);
    if (!t.active) return Orig<decltype(&LensDrawIndexed)>(kDev, 82)(d, ty, base, minIndex, verts, start, prims);
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (SUCCEEDED(d->GetStreamSource(0, &vb, &offset, &stride)) && vb) vb->Release();
    if ((++t.draws & 0xfffff) == 0) t.largest[1].swap(t.largest[0]), t.largest[0].clear();  // forget buffers that are gone
    UINT& now = t.largest[0][vb];
    if (prims > now) now = prims;
    UINT most = now;
    auto before = t.largest[1].find(vb);
    if (before != t.largest[1].end() && before->second > most) most = before->second;
    bool bias = g_lensOn && vb && static_cast<uint64_t>(prims) * g_lensRatio <= most && (g_lensBias != 0 || g_lensSlope != 0);
    bool watch = g_lensWatch && GetCurrentThreadId() == g_lensThread;
    bool query = watch && (prims == g_lensTrisA || prims == g_lensTrisB) && g_lensDraws.size() < 64;
    if (query && !g_lensQuery && FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_lensQuery))) query = false;
    if (watch) ++g_lensActiveDraws, g_lensBiasedDraws += bias;
    DWORD oldBias = 0, oldSlope = 0;
    if (bias) {
        if (FAILED(d->GetRenderState(D3DRS_DEPTHBIAS, &oldBias))) oldBias = 0;
        if (FAILED(d->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &oldSlope))) oldSlope = 0;
        d->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<DWORD*>(&g_lensBias));
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<DWORD*>(&g_lensSlope));
    }
    if (query) g_lensQuery->Issue(D3DISSUE_BEGIN);
    HRESULT hr = Orig<decltype(&LensDrawIndexed)>(kDev, 82)(d, ty, base, minIndex, verts, start, prims);
    if (query) {
        g_lensQuery->Issue(D3DISSUE_END);
        LensDraw x{prims, 0, bias, {}};
        for (int i = 0; i < 100000 && g_lensQuery->GetData(&x.px, sizeof x.px, D3DGETDATA_FLUSH) == S_FALSE; ++i) {}
        if (prims == g_lensTrisB) {
            float c[64] = {};
            d->GetVertexShaderConstantF(0, c, 16);
            for (int g = 0; g < 4; ++g) x.hash[g] = LensHash(c + g * 16, 16);
            static std::unordered_map<uint64_t, int> seen;
            uint64_t all = (static_cast<uint64_t>(LensHash(c, 32)) << 32) | LensHash(c + 32, 32);
            if (seen.size() < 12 && seen.emplace(all, 0).second) {
                char line[1600];
                int n = sprintf_s(line, "small parts: watch constants %08x %08x %08x %08x:", x.hash[0], x.hash[1], x.hash[2], x.hash[3]);
                for (int i = 0; i < 64; ++i) n += sprintf_s(line + n, sizeof line - n, " %.9g", c[i]);
                Log("%s", line);
            }
        }
        g_lensDraws.push_back(x);
    }
    if (bias) {
        d->SetRenderState(D3DRS_DEPTHBIAS, oldBias);
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, oldSlope);
    }
    return hr;
}
void LensPresent() {
    if (!LensWanted()) return;
    static unsigned frame = 0;
    static bool down = false;
    ++frame;
    if (g_lensToggleKey) {
        bool now = (GetAsyncKeyState(g_lensToggleKey) & 0x8000) != 0;
        if (now && !down) {
            g_lensOn = !g_lensOn;
            Log("small parts: depth bias %s (key), frame %u", g_lensOn ? "on" : "off", frame);
        }
        down = now;
    }
    if (!g_lensWatch) return;
    g_lensThread = GetCurrentThreadId();
    g_lensFrame = frame;
    if (g_lensPickFrames > 0 && --g_lensPickFrames == 0) Log("lenspick: done at frame %u", frame);
    g_lensPickDraw = 0;
    if (g_lensPickKey && g_lensPickFrames == 0 && (GetAsyncKeyState(g_lensPickKey) & 0x8000)) {
        IDirect3DSurface9* back = nullptr;
        if (g_device && SUCCEEDED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
            D3DSURFACE_DESC bd{};
            back->GetDesc(&bd);
            back->Release();
            g_lensBackW = bd.Width;
        }
        g_lensPickFrames = g_lensPickPerPress;
        Log("lenspick: key, %d frames from frame %u, back buffer width %u", g_lensPickFrames, frame + 1, g_lensBackW);
    }
    static char line[8192];
    int n = sprintf_s(line, "lens: f %u bias %d draws %u biased %u |", frame, g_lensOn ? 1 : 0, g_lensActiveDraws, g_lensBiasedDraws);
    for (const LensDraw& x : g_lensDraws) {
        n += sprintf_s(line + n, sizeof line - n, " %u/%lu%s", x.prims, x.px, x.biased ? "b" : "");
        if (x.prims == g_lensTrisB) n += sprintf_s(line + n, sizeof line - n, "/%08x.%08x.%08x.%08x", x.hash[0], x.hash[1], x.hash[2], x.hash[3]);
    }
    if (g_lensActiveDraws) Log("%s", line);
    g_lensDraws.clear();
    g_lensActiveDraws = g_lensBiasedDraws = 0;
}

// --- device instrumentation ----------------------------------------------------------------

void PlanDeviceSlots() {
    auto want = [](int slot, void* fn) { g_devWant[slot] = fn; };
    want(kPresent, reinterpret_cast<void*>(&Present));  // frame counter and periodic re-arm
    if (g_opt.lodFade || resolve::Enabled()) want(106, reinterpret_cast<void*>(&CreatePixelShaderFade));  // CreatePixelShader
    if (resolve::Enabled()) want(107, reinterpret_cast<void*>(&SetPixelShaderResolve));                   // SetPixelShader
    if (resolve::Enabled()) want(65, reinterpret_cast<void*>(&SetTextureResolve));                        // SetTexture
    // (The hooks below for the same slots take these over when they are on; they do the same substitutions.)
    if (g_opt.samplerProbe || resolve::Enabled()) want(kSetSamplerState, reinterpret_cast<void*>(&SetSamplerStateProbe));
    if (postfx::Enabled()) want(kSetRenderState, reinterpret_cast<void*>(&SetRenderStatePostfx));
    if (postfx::CameraWanted()) want(kSetPSConstF, reinterpret_cast<void*>(&SetPSConstFPostfx));
    if (LensWanted()) want(82, reinterpret_cast<void*>(&LensDrawIndexed));  // DrawIndexedPrimitive
    want(kReset, reinterpret_cast<void*>(&Reset));
    want(kCreateStateBlock, reinterpret_cast<void*>(&CreateStateBlock));
    want(kBeginStateBlock, reinterpret_cast<void*>(&BeginStateBlock));
    want(kEndStateBlock, reinterpret_cast<void*>(&EndStateBlock));
    if (g_opt.effectNoSaveState) want(kSetSamplerState, reinterpret_cast<void*>(&SetSamplerState));
    if (g_opt.stateFilter) {
        want(kSetRenderState, reinterpret_cast<void*>(&SetRenderState));
        want(kSetSamplerState, reinterpret_cast<void*>(&SetSamplerState));
        want(kSetTextureStageState, reinterpret_cast<void*>(&SetTextureStageState));
        want(kSetTexture, reinterpret_cast<void*>(&SetTexture));
        want(kSetVertexShader, reinterpret_cast<void*>(&SetVertexShader));
        want(kSetPixelShader, reinterpret_cast<void*>(&SetPixelShader));
        want(kSetVertexDeclaration, reinterpret_cast<void*>(&SetVertexDeclaration));
        want(kSetFVF, reinterpret_cast<void*>(&SetFVF));
        want(kSetIndices, reinterpret_cast<void*>(&SetIndices));
        want(kSetStreamSource, reinterpret_cast<void*>(&SetStreamSource));
        want(kSetVSConstF, reinterpret_cast<void*>(&SetVSConstF));
        want(kSetPSConstF, reinterpret_cast<void*>(&SetPSConstF));
    }
    if (glitch::Enabled() && !g_opt.stateFilter) {
        want(kSetVertexShader, reinterpret_cast<void*>(&GlitchSetVS));
        want(kSetPixelShader, reinterpret_cast<void*>(&GlitchSetPS));
        want(kSetIndices, reinterpret_cast<void*>(&GlitchSetIndices));
        want(kSetStreamSource, reinterpret_cast<void*>(&GlitchSetStreamSource));
        want(kSetTexture, reinterpret_cast<void*>(&GlitchSetTexture));
        want(82, reinterpret_cast<void*>(&GlitchDrawIndexed));  // DrawIndexedPrimitive
        if (glitch::PickWanted()) want(81, reinterpret_cast<void*>(&GlitchDraw));  // DrawPrimitive
        want(37, reinterpret_cast<void*>(&GlitchSetRenderTarget));
        want(47, reinterpret_cast<void*>(&GlitchSetViewport));  // SetViewport
        want(94, reinterpret_cast<void*>(&GlitchSetVSConstF));   // SetVertexShaderConstantF
        want(109, reinterpret_cast<void*>(&GlitchSetPSConstF));  // SetPixelShaderConstantF
        want(39, reinterpret_cast<void*>(&GlitchSetDepthStencil));
        want(kSetRenderState, reinterpret_cast<void*>(&GlitchSetRenderState));
        want(kSetSamplerState, reinterpret_cast<void*>(&GlitchSetSamplerState));
        want(102, reinterpret_cast<void*>(&GlitchSetStreamFreq));  // SetStreamSourceFreq
    }
    if (g_opt.census || g_opt.renderThread >= 2 || resolve::Enabled()) {  // resource creation hooks the Lock methods
        want(kCreateTexture, reinterpret_cast<void*>(&CreateTexture));
        want(kCreateVB, reinterpret_cast<void*>(&CreateVB));
        want(kCreateIB, reinterpret_cast<void*>(&CreateIB));
        want(kCreateCubeTexture, reinterpret_cast<void*>(&CreateCubeTexture));
        want(kCreateOffscreen, reinterpret_cast<void*>(&CreateOffscreenPlainSurface));
    }
    if (g_opt.renderThread >= 2) want(kCreateQuery, reinterpret_cast<void*>(&CreateQuery));
    if (postfx::DrawWanted()) {  // (the lens and glitch hooks of slots 81 and 82 call postfx::OnDraw themselves)
        if (!g_devWant[81]) want(81, reinterpret_cast<void*>(&DrawPostfx));           // DrawPrimitive
        if (!g_devWant[82]) want(82, reinterpret_cast<void*>(&DrawIndexedPostfx));    // DrawIndexedPrimitive
        if (!g_devWant[83]) want(83, reinterpret_cast<void*>(&DrawUPPostfx));         // DrawPrimitiveUP
        if (!g_devWant[84]) want(84, reinterpret_cast<void*>(&DrawIndexedUPPostfx));  // DrawIndexedPrimitiveUP
    }
    if (g_opt.census) {
        for (int i = 0; i < kDevSlots; ++i)
            if (!g_devWant[i]) g_devWant[i] = EmitThunk(kDev, i);
    }
}

// The runtime keeps a per-device vtable copy and may rewrite entries after creation
// (fast-path variants). Point every planned slot back at us, adopting its new original.
//
// Calls made while a slot was overwritten bypassed us, so the shadow state may be
// stale: invalidate it. If EndStateBlock was bypassed we may have missed the end of
// a recording, so leave recording mode as well.
void Rearm(IDirect3DDevice9* d) {
    void** vt = *reinterpret_cast<void***>(d);
    bool missed = false;
    for (int i = 0; i < kDevSlots; ++i) {
        if (!g_devWant[i] || vt[i] == g_devWant[i]) continue;
        g_b->original[kDev][i] = vt[i];
        WriteSlot(&vt[i], g_devWant[i]);
        missed = true;
        if (i == kEndStateBlock) g_s.recording = false;
    }
    if (missed) {
        Invalidate();
        Log("d3d: re-armed device slots overwritten by the runtime");
    }
}

// Diagnostics for the DX12 build, which talks to D3D12 through IDirect3DDevice9On12.
// Interface layout (d3d9on12.h): IUnknown, GetD3D12Device, UnwrapUnderlyingResource, ReturnUnderlyingResource.
constexpr GUID kIidDevice9On12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
using QIFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using GetD3D12DeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using UnwrapFn = HRESULT(STDMETHODCALLTYPE*)(void*, IDirect3DResource9*, void* queue, REFIID, void**);
using ReturnFn = HRESULT(STDMETHODCALLTYPE*)(void*, IDirect3DResource9*, UINT n, UINT64* values, void** fences);
QIFn g_devQI = nullptr;
GetD3D12DeviceFn g_get12 = nullptr;
UnwrapFn g_unwrap = nullptr;
ReturnFn g_return = nullptr;
std::atomic<int> g_on12Calls[4];

bool LogOn12(int which) { return ++g_on12Calls[which] <= 20 || (g_on12Calls[which] & 1023) == 0; }

const char* ResourceDesc(IDirect3DResource9* r, char* buf, size_t n) {
    snprintf(buf, n, "%p type=%d", r, r ? r->GetType() : -1);
    IDirect3DSurface9* s = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(IDirect3DSurface9), reinterpret_cast<void**>(&s))) && s) {
        D3DSURFACE_DESC d{};
        s->GetDesc(&d);
        size_t used = strlen(buf);
        snprintf(buf + used, n - used, " surf %ux%u fmt=%d usage=0x%lx pool=%d", d.Width, d.Height, d.Format, d.Usage, d.Pool);
        s->Release();
    }
    return buf;
}

HRESULT STDMETHODCALLTYPE Get12(void* self, REFIID riid, void** out) {
    HRESULT hr = g_get12(self, riid, out);
    if (LogOn12(1)) Log("d3d: 9on12 GetD3D12Device #%d -> 0x%08lx %p", g_on12Calls[1].load(), hr, out ? *out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Unwrap(void* self, IDirect3DResource9* r, void* queue, REFIID riid, void** out) {
    HRESULT hr = g_unwrap(self, r, queue, riid, out);
    if (LogOn12(2)) {
        char buf[160];
        Log("d3d: 9on12 Unwrap #%d %s queue=%p -> 0x%08lx %p", g_on12Calls[2].load(), ResourceDesc(r, buf, sizeof buf),
            queue, hr, out ? *out : nullptr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Return(void* self, IDirect3DResource9* r, UINT n, UINT64* values, void** fences) {
    HRESULT hr = g_return(self, r, n, values, fences);
    if (LogOn12(3))
        Log("d3d: 9on12 Return #%d %p fences=%u value0=%llu -> 0x%08lx", g_on12Calls[3].load(), r, n,
            n && values ? values[0] : 0ull, hr);
    return hr;
}

template <typename Fn>
void Detour(void* target, Fn hook, Fn* original) {
    if (*original) return;
    if (MH_CreateHook(target, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(original)) == MH_OK)
        MH_EnableHook(target);
}

HRESULT STDMETHODCALLTYPE DevQI(void* self, REFIID riid, void** out) {
    HRESULT hr = g_devQI(self, riid, out);
    if (riid == kIidDevice9On12) {
        if (LogOn12(0)) Log("d3d: QueryInterface(IDirect3DDevice9On12) #%d -> 0x%08lx %p", g_on12Calls[0].load(), hr, out ? *out : nullptr);
        if (SUCCEEDED(hr) && out && *out) {
            void** vt = *static_cast<void***>(*out);
            Detour(vt[3], &Get12, &g_get12);
            Detour(vt[4], &Unwrap, &g_unwrap);
            Detour(vt[5], &Return, &g_return);
        }
    }
    return hr;
}

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDeviceFn g_createDevice = nullptr;

HRESULT STDMETHODCALLTYPE CreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD flags,
                                       D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev) {
    // A render thread and the game thread both touch the runtime (resource locks,
    // object lifetimes), so the runtime must serialize internally.
    if (g_opt.renderThread >= 2 && g_opt.runtimeLock) flags |= D3DCREATE_MULTITHREADED;
    if (g_opt.swapEffect && pp) {
        Log("d3d: swap effect %d -> %d (diagnostic)", pp->SwapEffect, g_opt.swapEffect);
        pp->SwapEffect = static_cast<D3DSWAPEFFECT>(g_opt.swapEffect);
        if (pp->SwapEffect != D3DSWAPEFFECT_DISCARD && pp->BackBufferCount < 2 && pp->SwapEffect != D3DSWAPEFFECT_COPY)
            pp->BackBufferCount = 2;
    }
    HRESULT hr = g_createDevice(d3d, a, t, w, flags, pp, dev);
    Log("d3d: CreateDevice #%s on IDirect3D9 %p hwnd=%p focus=%p -> 0x%08lx device %p", g_device ? "2+" : "1", d3d,
        pp ? pp->hDeviceWindow : nullptr, w, hr, SUCCEEDED(hr) && dev ? *dev : nullptr);
    if (pp)
        Log("d3d:   present params: %ux%u fmt=%d count=%u msaa=%d/%lu swap=%d windowed=%d autodepth=%d depthfmt=%d "
            "flags=0x%lx refresh=%u interval=0x%x", pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
            pp->BackBufferCount, pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, pp->Windowed,
            pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
            pp->PresentationInterval);
    if (SUCCEEDED(hr) && !g_device) {
        g_device = *dev;
        Log("d3d: CreateDevice flags=0x%lx %ux%u windowed=%d", flags, pp ? pp->BackBufferWidth : 0,
            pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
        if (g_opt.census) Detour((*reinterpret_cast<void***>(g_device))[0], &DevQI, &g_devQI);
        PlanDeviceSlots();
        Rearm(g_device);
        std::string fixes;
        if (g_opt.effectNoSaveState) fixes += "effect_nosavestate ";
        if (g_opt.stateFilter) fixes += "state_filter ";
        if (g_opt.census) fixes += "d3d census ";
        if (!fixes.empty()) status::Set("fixes", fixes.c_str());
        // The engine gets the wrapper; census and filter keep working on the real device below it.
        *dev = rt::Wrap(*dev, static_cast<rt::Mode>(g_opt.renderThread));
        g_gameDevice = *dev;
    }
    return hr;
}

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
Create9Fn g_create9 = nullptr;

// IDirect3D9Ex::CreateDeviceEx (slot 20): logged so we can see devices created this way.
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
CreateDeviceExFn g_createDeviceEx = nullptr;
HRESULT STDMETHODCALLTYPE CreateDeviceEx(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD flags,
                                         D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** dev) {
    HRESULT hr = g_createDeviceEx(d3d, a, t, w, flags, pp, mode, dev);
    Log("d3d: CreateDeviceEx on %p hwnd=%p focus=%p flags=0x%lx %ux%u windowed=%d swap=%d -> 0x%08lx device %p", d3d,
        pp ? pp->hDeviceWindow : nullptr, w, flags, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        pp ? pp->Windowed : -1, pp ? pp->SwapEffect : -1, hr, SUCCEEDED(hr) && dev ? *dev : nullptr);
    return hr;
}

// Diagnostics: capability checks the engine makes before CreateDevice (logged with results).
using GetModeFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
using CheckTypeFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL);
using CheckFormatFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
using CheckMsFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD*);
using CheckDsFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT);
GetModeFn g_getMode = nullptr;
CheckTypeFn g_checkType = nullptr;
CheckFormatFn g_checkFormat = nullptr;
CheckMsFn g_checkMs = nullptr;
CheckDsFn g_checkDs = nullptr;
std::atomic<int> g_capLogs{0};
bool LogCaps() { return ++g_capLogs <= 200; }

HRESULT STDMETHODCALLTYPE GetMode(IDirect3D9* d, UINT a, D3DDISPLAYMODE* m) {
    HRESULT hr = g_getMode(d, a, m);
    if (LogCaps()) Log("d3d: GetAdapterDisplayMode(%u) -> 0x%08lx %ux%u fmt=%d", a, hr, m ? m->Width : 0, m ? m->Height : 0, m ? m->Format : 0);
    return hr;
}
HRESULT STDMETHODCALLTYPE CheckType(IDirect3D9* d, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT b, BOOL w) {
    HRESULT hr = g_checkType(d, a, t, f, b, w);
    if (LogCaps()) Log("d3d: CheckDeviceType(%u, %d, %d, %d, windowed=%d) -> 0x%08lx", a, t, f, b, w, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE CheckFormat(IDirect3D9* d, UINT a, D3DDEVTYPE t, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) {
    HRESULT hr = g_checkFormat(d, a, t, f, u, r, c);
    if (LogCaps()) Log("d3d: CheckDeviceFormat(%u, %d, adapter fmt %d, usage 0x%lx, rtype %d, fmt %d) -> 0x%08lx", a, t, f, u, r, c, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE CheckMs(IDirect3D9* d, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) {
    HRESULT hr = g_checkMs(d, a, t, f, w, m, q);
    if (LogCaps()) Log("d3d: CheckDeviceMultiSampleType(%u, %d, fmt %d, windowed=%d, ms %d) -> 0x%08lx", a, t, f, w, m, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE CheckDs(IDirect3D9* d, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT rt, D3DFORMAT ds) {
    HRESULT hr = g_checkDs(d, a, t, f, rt, ds);
    if (LogCaps()) Log("d3d: CheckDepthStencilMatch(%u, %d, %d, rt %d, ds %d) -> 0x%08lx", a, t, f, rt, ds, hr);
    return hr;
}

template <typename Fn>
void PatchSlot(void** vt, int slot, Fn hook, Fn* original) {
    if (vt[slot] == reinterpret_cast<void*>(hook)) return;
    if (!*original) *original = reinterpret_cast<Fn>(vt[slot]);
    WriteSlot(&vt[slot], reinterpret_cast<void*>(hook));
}

void PatchDirect3D(IDirect3D9* d3d) {
    if (!d3d) return;
    if (g_opt.crashLog) {
        void** cvt = *reinterpret_cast<void***>(d3d);
        PatchSlot(cvt, 8, &GetMode, &g_getMode);
        PatchSlot(cvt, 9, &CheckType, &g_checkType);
        PatchSlot(cvt, 10, &CheckFormat, &g_checkFormat);
        PatchSlot(cvt, 11, &CheckMs, &g_checkMs);
        PatchSlot(cvt, 12, &CheckDs, &g_checkDs);
    }
    IDirect3D9Ex* ex = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&ex))) && ex) {
        void** xvt = *reinterpret_cast<void***>(ex);
        if (xvt[20] != reinterpret_cast<void*>(&CreateDeviceEx)) {
            if (!g_createDeviceEx) g_createDeviceEx = reinterpret_cast<CreateDeviceExFn>(xvt[20]);
            if (reinterpret_cast<void*>(g_createDeviceEx) == xvt[20]) WriteSlot(&xvt[20], reinterpret_cast<void*>(&CreateDeviceEx));
        }
        ex->Release();
    }
    void** vt = *reinterpret_cast<void***>(d3d);
    if (vt[16] == reinterpret_cast<void*>(&CreateDevice)) return;  // this vtable is already patched
    if (g_createDevice && reinterpret_cast<void*>(g_createDevice) != vt[16]) {
        Log("d3d: second IDirect3D9 implementation; its devices are not instrumented");
        return;
    }
    g_createDevice = reinterpret_cast<CreateDeviceFn>(vt[16]);
    WriteSlot(&vt[16], reinterpret_cast<void*>(&CreateDevice));
}

using Create9On12Fn = IDirect3D9*(WINAPI*)(UINT, void* args, UINT count);
Create9On12Fn g_create9On12 = nullptr;

IDirect3D9* WINAPI Create9(UINT sdk) {
    IDirect3D9* d3d = nullptr;
    if (g_opt.force9On12) {  // diagnostic: route the DX9 build through D3D9On12
        auto real9On12 = g_create9On12 ? g_create9On12
                       : reinterpret_cast<Create9On12Fn>(GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9On12"));
        if (real9On12) d3d = real9On12(sdk, nullptr, 0);
        Log("d3d: forced D3D9On12 -> %p", d3d);
    }
    if (!d3d) d3d = g_create9(sdk);
    Log("d3d: Direct3DCreate9(sdk=%u) -> %p", sdk, d3d);
    PatchDirect3D(d3d);
    return d3d;
}

// The DX12 build creates its device through D3D9On12 instead.
// D3D9ON12_ARGS from the Windows SDK's d3d9.h (not exposed in every header configuration).
struct NineOn12Args {
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[2];
    UINT NumQueues;
    UINT NodeMask;
};

IDirect3D9* WINAPI Create9On12(UINT sdk, void* args, UINT count) {
    auto* a = static_cast<NineOn12Args*>(args);
    for (UINT i = 0; a && i < count; ++i)
        Log("d3d: 9On12 args[%u]: enable=%d device=%p queues=%u (%p, %p) nodemask=0x%x", i, a[i].Enable9On12,
            a[i].pD3D12Device, a[i].NumQueues, a[i].ppD3D12Queues[0], a[i].ppD3D12Queues[1], a[i].NodeMask);
    if (g_opt.default9On12Args && count) {
        Log("d3d: ignoring the caller's 9On12 overrides (diagnostic)");
        args = nullptr;
        count = 0;
    }
    IDirect3D9* d3d = g_create9On12(sdk, args, count);
    Log("d3d: Direct3DCreate9On12(sdk=%u, overrides=%u) -> %p", sdk, count, d3d);
    PatchDirect3D(d3d);
    return d3d;
}

// --- D3DX effects --------------------------------------------------------------------------
volatile uint64_t g_fxBeginFlags[8];
extern IDirect3DDevice9* g_device;

// --- effect state leak probe ([experiments] fx_leak / fx_restore; direct device only) -------
// With DONOTSAVESTATE an effect's End leaves the device as its passes set it; the game's own
// Begin(0) puts everything back. The probe reads the device state at Begin and at End, counts per
// effect file what End would have put back, and with fx_restore puts it back itself
// (bits: 1 sampler states, 2 render states, 4 texture stage states). Slow: a diagnostic.
int g_fxLeak = -1, g_fxRestore = 0;
bool g_fxKeepSamplers = true;  // [fixes] effect_keep_samplers: End puts sampler states back (see FxSamplers)
std::unordered_map<void*, std::string> g_fxFile;
const char* FxFileName(void* fx) {
    auto it = g_fxFile.find(fx);
    return it == g_fxFile.end() ? "?" : it->second.c_str();
}
std::unordered_map<std::string, unsigned> g_fxLeaks;
ULONGLONG g_fxLeakDump = 0;
struct FxState { DWORD ss[16][14]; DWORD rs[210]; DWORD tss[8][33]; };
FxState g_fxBefore[4];
int g_fxDepth = 0;

bool FxProbe() {
    if (g_fxLeak < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, 0x5c)) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "tscfix.ini");
        g_fxRestore = GetPrivateProfileIntA("experiments", "fx_restore", 0, ini);
        g_fxKeepSamplers = GetPrivateProfileIntA("fixes", "effect_keep_samplers", 1, ini) != 0;
        Log("d3d: effects put sampler states back at End: %d", g_fxKeepSamplers);
        g_fxLeak = (GetPrivateProfileIntA("experiments", "fx_leak", 0, ini) || g_fxRestore) && g_opt.renderThread == 0;
        if (g_fxLeak) Log("d3d: effect state leak probe on (restore mask %d)", g_fxRestore);
    }
    return g_fxLeak > 0 && g_device;
}

void FxRead(FxState& st) {
    for (DWORD i = 0; i < 16; ++i)
        for (int t = 1; t <= 13; ++t) g_device->GetSamplerState(i, D3DSAMPLERSTATETYPE(t), &st.ss[i][t]);
    for (int r = 7; r < 210; ++r)
        if (FAILED(g_device->GetRenderState(D3DRENDERSTATETYPE(r), &st.rs[r]))) st.rs[r] = 0;
    for (DWORD i = 0; i < 8; ++i)
        for (int t = 1; t <= 32; ++t)
            if (FAILED(g_device->GetTextureStageState(i, D3DTEXTURESTAGESTATETYPE(t), &st.tss[i][t]))) st.tss[i][t] = 0;
}

void FxNote(void* fx, const char* kind, int index, int type, DWORD before, DWORD after) {
    auto it = g_fxFile.find(fx);
    char key[400];
    sprintf_s(key, "%s %s%d.%d %lu->%lu", it == g_fxFile.end() ? "?" : it->second.c_str(), kind, index, type,
              static_cast<unsigned long>(before), static_cast<unsigned long>(after));
    ++g_fxLeaks[key];
}

void FxCompare(void* fx) {
    static FxState now;
    const FxState& was = g_fxBefore[g_fxDepth];
    FxRead(now);
    for (DWORD i = 0; i < 16; ++i)
        for (int t = 1; t <= 13; ++t)
            if (now.ss[i][t] != was.ss[i][t]) {
                FxNote(fx, "sampler", i, t, was.ss[i][t], now.ss[i][t]);
                if (g_fxRestore & 1) g_device->SetSamplerState(i, D3DSAMPLERSTATETYPE(t), was.ss[i][t]);
            }
    for (int r = 7; r < 210; ++r)
        if (now.rs[r] != was.rs[r]) {
            FxNote(fx, "render", 0, r, was.rs[r], now.rs[r]);
            if (g_fxRestore & 2) g_device->SetRenderState(D3DRENDERSTATETYPE(r), was.rs[r]);
        }
    for (DWORD i = 0; i < 8; ++i)
        for (int t = 1; t <= 32; ++t)
            if (now.tss[i][t] != was.tss[i][t]) {
                FxNote(fx, "stage", i, t, was.tss[i][t], now.tss[i][t]);
                if (g_fxRestore & 4) g_device->SetTextureStageState(i, D3DTEXTURESTAGESTATETYPE(t), was.tss[i][t]);
            }
    ULONGLONG t = GetTickCount64();
    if (!g_fxLeakDump) g_fxLeakDump = t;
    if (t - g_fxLeakDump > 30000) {
        g_fxLeakDump = t;
        Log("fxleak: %zu kinds of state left changed by an effect's End (count since start)", g_fxLeaks.size());
        for (const auto& e : g_fxLeaks) Log("fxleak: %8u  %s", e.second, e.first.c_str());
    }
}

HRESULT STDMETHODCALLTYPE FxBegin(void* fx, UINT* passes, DWORD flags) {
    Count(kFx, kFxBegin);
    bool probe = FxProbe();
    if (probe && g_fxDepth < 4) FxRead(g_fxBefore[g_fxDepth]);
    if (g_fxLeak > 0) ++g_fxDepth;
    if (NoSaveState() && g_fxKeepSamplers && t_fx.depth++ == 0) ++t_fx.epoch, t_fx.n = 0;
    // The effect vtable is static, so this hook always runs; use it to re-arm the
    // device slots the runtime rewrites (including Present itself).
    static unsigned begins = 0;
    if (g_device && (++begins & 511) == 1) Rearm(g_device);
    if (g_opt.census) ++g_fxBeginFlags[flags & 7];
    if (NoSaveState()) flags |= 1;  // D3DXFX_DONOTSAVESTATE
    HRESULT hr = Orig<decltype(&FxBegin)>(kFx, kFxBegin)(fx, passes, flags);
    if (SUCCEEDED(hr) && g_lensInit && LensWanted()) LensBegin(fx);
    return hr;
}

HRESULT STDMETHODCALLTYPE FxEnd(void* fx) {
    Count(kFx, kFxEnd);
    HRESULT hr = Orig<decltype(&FxEnd)>(kFx, kFxEnd)(fx);
    t_lens.active = false, t_lens.fx = nullptr;
    if (t_fx.depth && --t_fx.depth == 0) {
        FxSamplers& f = t_fx;
        for (int k = 0; k < f.n; ++k)
            if (f.e[k].now != f.e[k].before) SetSamplerState(f.dev, f.e[k].sampler, f.e[k].type, f.e[k].before);
    }
    if (g_fxLeak > 0 && g_fxDepth > 0 && --g_fxDepth < 4 && g_device) FxCompare(fx);
    // Without DONOTSAVESTATE, End restores device state through a state block,
    // which bypasses our device overrides.
    if (!NoSaveState()) Invalidate();
    return hr;
}

void InstrumentEffect(void* effect) {
    static bool done = false;
    if (done || !effect) return;
    done = true;
    void** vt = *static_cast<void***>(effect);
    for (int i = 0; i < kFxSlots; ++i) {
        g_b->original[kFx][i] = vt[i];
        void* want = i == kFxBegin ? reinterpret_cast<void*>(&FxBegin)
                   : i == kFxEnd   ? reinterpret_cast<void*>(&FxEnd)
                   : g_opt.census  ? EmitThunk(kFx, i)
                                   : nullptr;
        if (want) WriteSlot(&vt[i], want);
    }
    Log("d3d: effect interface instrumented");
}

using CreateEffectFromFileAFn = HRESULT(WINAPI*)(void*, const char*, const void*, void*, DWORD, void*, void**, void**);
CreateEffectFromFileAFn g_createEffectFromFileA = nullptr;

HRESULT WINAPI CreateEffectFromFileA(void* dev, const char* file, const void* defs, void* inc, DWORD flags,
                                     void* pool, void** fx, void** errors) {
    // Beside the recorder (pipelined update, its loading fibers) the effect pool is shared with
    // effects the render thread is replaying: park it for the creation.
    bool pause = rt::DirectCaller();
    if (pause) rt::PauseRenderThread();
    HRESULT hr = g_createEffectFromFileA(dev, file, defs, inc, flags, pool, fx, errors);
    if (pause) rt::ResumeRenderThread();
    if (SUCCEEDED(hr) && fx) {
        if (file) g_fxFile[*fx] = strrchr(file, 0x5c) ? strrchr(file, 0x5c) + 1 : file;
        if (file) LensNoteEffect(*fx, strrchr(file, 0x5c) ? strrchr(file, 0x5c) + 1 : file);
        InstrumentEffect(*fx);
        if (g_opt.renderThread >= 2) *fx = rt::WrapEffect(*fx);
    }
    return hr;
}

}  // namespace

void HookSurface(IDirect3DSurface9* surface) { HookSlot(surface, 13, g_surfLock, &SurfLock); }

bool Install(const Options& options) {
    g_opt = options;
    if (g_opt.renderThread >= 2 && !g_opt.effectNoSaveState) {
        // Saving state means a state block Capture/Apply per effect pass, which would
        // force the render thread to drain thousands of times per frame.
        g_opt.effectNoSaveState = true;
        Log("d3d: render thread forces effect_nosavestate");
    }
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    if (!d3d9 || !d3dx) {
        Log("d3d: d3d9/d3dx9 not loaded");
        return false;
    }
    g_b = static_cast<Block*>(VirtualAlloc(nullptr, sizeof(Block), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    MH_Initialize();
    void* create9 = GetProcAddress(d3d9, "Direct3DCreate9");
    void* cef = GetProcAddress(d3dx, "D3DXCreateEffectFromFileA");
    bool ok = create9 && cef &&
              MH_CreateHook(create9, reinterpret_cast<void*>(&Create9), reinterpret_cast<void**>(&g_create9)) == MH_OK &&
              MH_CreateHook(cef, reinterpret_cast<void*>(&CreateEffectFromFileA),
                            reinterpret_cast<void**>(&g_createEffectFromFileA)) == MH_OK &&
              MH_EnableHook(create9) == MH_OK && MH_EnableHook(cef) == MH_OK;
    if (void* on12 = GetProcAddress(d3d9, "Direct3DCreate9On12")) {
        if (MH_CreateHook(on12, reinterpret_cast<void*>(&Create9On12), reinterpret_cast<void**>(&g_create9On12)) == MH_OK)
            MH_EnableHook(on12);
    }
    Log("d3d: %s (census=%d nosavestate=%d statefilter=%d renderthread=%d)", ok ? "installed" : "install failed",
        g_opt.census, g_opt.effectNoSaveState, g_opt.stateFilter, g_opt.renderThread);
    return ok;
}

void ResetCensus() {
    if (!g_b) return;
    for (auto& i : g_b->count)
        for (auto& c : i) c = 0;
    for (auto& c : g_b->filtered) c = 0;
    for (auto& k : g_locks)
        for (auto& c : k) c = 0;
    for (auto& c : g_fxBeginFlags) c = 0;
}

void WriteCensus(const char* path) {
    if (!g_b || !g_opt.census) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    double frames = static_cast<double>(g_b->count[kDev][kPresent]);
    if (frames < 1) frames = 1;
    uint64_t dev = 0, fx = 0, skipped = 0;
    for (int s = 0; s < kDevSlots; ++s) dev += g_b->count[kDev][s], skipped += g_b->filtered[s];
    for (int s = 0; s < kFxSlots; ++s) fx += g_b->count[kFx][s];
    fprintf(f, "# frames=%.0f device calls/frame=%.0f (filtered %.0f) effect calls/frame=%.0f\n", frames,
            dev / frames, skipped / frames, fx / frames);
    const char* lk[] = {"vertex buffer", "index buffer", "texture"};
    const char* lc[] = {"discard", "nooverwrite", "readonly", "plain"};
    for (int k = 0; k < kLockKinds; ++k)
        for (int c = 0; c < 4; ++c)
            if (g_locks[k][c]) fprintf(f, "lock\t%s\t%s\t%.1f/frame\n", lk[k], lc[c], g_locks[k][c] / frames);
    for (int i = 0; i < 8; ++i)
        if (g_fxBeginFlags[i]) fprintf(f, "effect_begin\tflags=%d\t%.1f/frame\n", i, g_fxBeginFlags[i] / frames);
    for (int s = 0; s < kDevSlots; ++s)
        if (g_b->count[kDev][s])
            fprintf(f, "device\t%s\t%.1f/frame\tfiltered %.1f/frame\n", d3dnames::kDevice9[s],
                    g_b->count[kDev][s] / frames, g_b->filtered[s] / frames);
    for (int s = 0; s < kFxSlots; ++s)
        if (g_b->count[kFx][s])
            fprintf(f, "effect\t%s\t%.1f/frame\n", d3dnames::kEffect[s], g_b->count[kFx][s] / frames);
    fclose(f);
}

// A/B switches, called on the game thread at Present (no effect pass spans a frame).
void SetFilterActive(bool on) {
    if (on && !g_filterActive) Invalidate();  // calls passed through unrecorded while paused
    g_filterActive = on;
}
void SetNoSaveStateActive(bool on) { g_noSaveActive = on; }

void SetDeferredLocks(bool on) {
    g_deferLocks = on;
    rt::SetSideAppendedHook(&OnSideAppended);
}
void SetDirectNoOverwrite(bool on) { g_directNoOverwrite = on; }
void SetLockPathsActive(bool on) { g_lockPathsActive = on; }

void AddVerifiedLockSite(const char* module, uintptr_t returnOffset, LockSiteKind kind) {
    if (g_siteCount == 16) return;
    VerifiedSite& v = g_sites[g_siteCount];
    strncpy_s(v.module, module, _TRUNCATE);
    v.ret = returnOffset;
    v.kind = kind;
    v.base = 0;
    v.fp = {};
    v.dead = false;
    ++g_siteCount;
}
void AddVerifiedLockSite(const char* module, const sites::Fingerprint& function, uintptr_t returnOffset,
                         LockSiteKind kind) {
    if (g_siteCount == 16) return;
    AddVerifiedLockSite(module, returnOffset, kind);
    g_sites[g_siteCount - 1].fp = function;
}

IDirect3DDevice9* GameDevice() { return g_gameDevice; }

void ReadFrameHealth(long* frames, long* over50, long* over100, long* worstMs, long* activeMs) {
    *activeMs = InterlockedExchange(&g_healthActiveMs, 0);
    *frames = InterlockedExchange(&g_healthFrames, 0), *over50 = InterlockedExchange(&g_healthOver50, 0);
    *over100 = InterlockedExchange(&g_healthOver100, 0), *worstMs = InterlockedExchange(&g_healthWorstMs, 0);
}

}  // namespace tscfix::d3d
