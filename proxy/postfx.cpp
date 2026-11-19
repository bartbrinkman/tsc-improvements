#include "postfx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "resolve.h"

// The game's picture passes after the scene is drawn (read from the game's shader sources):
//   FXAA            PostProcessFXAA.fx: constants fxaaQualitySubpix ..., sampler LumaSampler
//   depth of field  PostDepthOfField.fx: constants gBlurDiscCoords ..., sampler BBTexSamp
// A pass is recognised by the names in its shader's constant table, the first time the shader is
// set. A pass that is switched off gets a shader that copies its input picture.
namespace tscfix::postfx {
namespace {

Options g_opt;
enum Kind : uint8_t { kUnseen = 0, kOther = 1, kFxaa = 2, kDof = 3, kBloom = 4, kBright = 5, kGlare = 6, kLamp = 7 };
struct Classed {
    IDirect3DPixelShader9* shader;
    uint8_t kind;
    uint8_t sampler;  // the register of the pass's input picture
    uint8_t bloomSampler, lumSampler;  // (bloom pass) the blurred bright picture and the scene brightness
    uint32_t size;
    bool glow = false;  // (lamp) the glow pass, not a point or spot light
};
constexpr size_t kSlots = 16384;  // power of two
Classed g_classed[kSlots];
IDirect3DPixelShader9* g_copy[16] = {};  // a copy shader per sampler register
int g_fxaaOff = -1, g_dofOff = -1;       // compare mode: -1 use the options, 0 pass on, 1 pass off
volatile LONG g_replaced[4] = {};

// The register of the constant `name` in a shader's constant table, or -1.
int ConstantRegister(const uint8_t* code, size_t size, const char* name) {
    for (size_t i = 0; i + 40 < size; i += 4) {
        if (memcmp(code + i, "CTAB", 4) != 0) continue;
        const uint8_t* base = code + i + 4;
        uint32_t count, info;
        memcpy(&count, base + 12, 4);
        memcpy(&info, base + 16, 4);
        for (uint32_t k = 0; k < count && k < 64; ++k) {
            const uint8_t* e = base + info + 20 * k;
            if (e + 20 > code + size) return -1;
            uint32_t at;
            uint16_t reg;
            memcpy(&at, e, 4);
            memcpy(&reg, e + 6, 2);
            if (base + at + strlen(name) < code + size && strcmp(reinterpret_cast<const char*>(base + at), name) == 0) return reg;
        }
        return -1;
    }
    return -1;
}

Classed& Class(IDirect3DPixelShader9* shader) {
    size_t at = (reinterpret_cast<uintptr_t>(shader) >> 4) * 0x9E3779B97F4A7C15ull >> 50;
    Classed& c = g_classed[at & (kSlots - 1)];
    UINT size = 0;
    if (c.shader == shader && (c.kind == kOther || (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size == c.size))) return c;
    c = {shader, kOther, 0, 0, 0, 0};
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return c;
    std::vector<uint8_t> code(size + 8);
    if (FAILED(shader->GetFunction(code.data(), &size))) return c;
    c.size = size;
    int s;
    bool glow = false;
    if (ConstantRegister(code.data(), size, "fxaaQualitySubpix") >= 0 && (s = ConstantRegister(code.data(), size, "LumaSampler")) >= 0)
        c.kind = kFxaa, c.sampler = static_cast<uint8_t>(s);
    else if (ConstantRegister(code.data(), size, "gBlurDiscCoords") >= 0 && (s = ConstantRegister(code.data(), size, "BBTexSamp")) >= 0)
        c.kind = kDof, c.sampler = static_cast<uint8_t>(s);
    else if ((s = ConstantRegister(code.data(), size, "frameSamp")) >= 0 && ConstantRegister(code.data(), size, "Blur1ImgSamp") >= 0 &&
             ConstantRegister(code.data(), size, "AvgLumFinalSamp") >= 0) {
        c.kind = kBloom, c.sampler = static_cast<uint8_t>(s);
        c.bloomSampler = static_cast<uint8_t>(ConstantRegister(code.data(), size, "Blur1ImgSamp"));
        c.lumSampler = static_cast<uint8_t>(ConstantRegister(code.data(), size, "AvgLumFinalSamp"));
    }
    else if ((s = ConstantRegister(code.data(), size, "frameSamp")) >= 0 && ConstantRegister(code.data(), size, "BloomThreshold") >= 0)
        c.kind = kBright, c.sampler = static_cast<uint8_t>(s);  // the bright parts for the bloom
    else if (ConstantRegister(code.data(), size, "AvgLum2x2ImgSamp") >= 0)
        c.kind = kGlare;  // the scene brightness (drawn into a 1 x 1 target)
    else if (ConstantRegister(code.data(), size, "ViewSpaceNormalSampler") >= 0 && ConstantRegister(code.data(), size, "specMax") >= 0 &&
             ConstantRegister(code.data(), size, "AmbientLightColour") < 0)
        c.kind = kLamp;  // point and spot lights (DeferredPoint.fx, DeferredSpot.fx)
    // The glow around train and signal lamps (PostTrainLightGlow.fx): a mask of the lamp pixels, blurred
    // and added to the picture. It is treated as lamp light: it fades as the scene gets bright.
    else if (ConstantRegister(code.data(), size, "Frame1Sampler") >= 0 &&
             (ConstantRegister(code.data(), size, "BlurWeights_HIGH") >= 0 || ConstantRegister(code.data(), size, "BlurWeights_MEDIUM") >= 0 ||
              ConstantRegister(code.data(), size, "BlurWeights_LOW") >= 0))
        c.kind = kLamp, glow = true;
    else if (ConstantRegister(code.data(), size, "ColourSampler") >= 0 && ConstantRegister(code.data(), size, "AuxSampler") >= 0 &&
             ConstantRegister(code.data(), size, "ViewSpaceNormalSampler") < 0 && size < 1200)
        c.kind = kLamp, glow = true;
    c.glow = glow;
    if (glow) {
        static volatile LONG toldGlow = 0;
        if (InterlockedIncrement(&toldGlow) == 1) Log("postfx: the game's lamp glow shader is in use");
    } else if (c.kind == kBright || c.kind == kGlare || c.kind == kLamp) {
        static volatile LONG told[8];
        if (InterlockedIncrement(&told[c.kind]) == 1)
            Log("postfx: the game's %s shader is in use", c.kind == kBright ? "bloom bright-parts" : c.kind == kGlare ? "scene-brightness" : "lamp light");
    } else if (c.kind != kOther)
        Log("postfx: the game's %s pass is in use (input picture at sampler %d)", c.kind == kFxaa ? "FXAA" : c.kind == kDof ? "depth-of-field" : "bloom and tone",
            c.sampler);
    return c;
}

struct Blob : IUnknown {
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
using CompileFn = HRESULT(WINAPI*)(const void* src, SIZE_T size, const char* name, const void* defines, void* include, const char* entry,
                                   const char* target, UINT flags1, UINT flags2, Blob** code, Blob** errors);

IDirect3DPixelShader9* CopyShader(IDirect3DDevice9* device, int sampler) {
    if (sampler < 0 || sampler >= 16) return nullptr;
    static bool failed[16];
    if (g_copy[sampler] || failed[sampler]) return g_copy[sampler];
    failed[sampler] = true;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    char source[256];
    sprintf_s(source, "sampler2D s : register(s%d);\nfloat4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(s, uv).rgb, 1); }\n", sampler);
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "tscfix_copy", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !code) return nullptr;
    IDirect3DPixelShader9* shader = nullptr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    code->Release();
    if (FAILED(hr)) return nullptr;
    failed[sampler] = false;
    return g_copy[sampler] = shader;
}

// The mod's own last pass, in place of the game's "bloom and tone" pass (Train_post_bloom.fx,
// PSPresent). The game's pass removes 27% of the colour and adds bloom only in dark scenes. This
// one works in linear light: exposure by scene brightness, bloom in every scene, a contrast step
// around middle grey, a soft roll-off near white, full colour.
struct Film {
    const char* name;
    float exposureDay, exposureNight;  // linear gain in a bright and in a dark scene
    float bloomDay, bloomNight;        // weight of the blurred bright picture
    float contrast;                    // power around middle grey (1 = none)
    float shoulder;                    // the level (linear) from which bright values are pressed together; white ends below 1
    float saturation;                  // 1 = as rendered
};
const Film kFilms[] = {
    {"film", 0.90f, 1.05f, 0.05f, 0.60f, 1.08f, 0.55f, 1.15f},
    {"film-strong", 0.82f, 1.05f, 0.08f, 0.80f, 1.15f, 0.45f, 1.25f},
};
constexpr int kFilmCount = static_cast<int>(sizeof kFilms / sizeof kFilms[0]);
IDirect3DPixelShader9* g_film[kFilmCount] = {};
bool g_filmFailed[kFilmCount] = {};
int g_filmForced = -1;  // compare mode: -1 by the options, 0 the game's pass, 1.. a film (index + 1)

IDirect3DPixelShader9* FilmShader(IDirect3DDevice9* device, int index, const Classed& c) {
    if (index < 0 || index >= kFilmCount) return nullptr;
    if (g_film[index] || g_filmFailed[index]) return g_film[index];
    g_filmFailed[index] = true;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    const Film& f = kFilms[index];
    char source[2000];
    sprintf_s(source,
              "sampler2D frame : register(s%d);\nsampler2D bloom : register(s%d);\nsampler2D lum : register(s%d);\n"
              "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
              "  float3 c = tex2D(frame, uv).rgb;\n  float3 b = tex2D(bloom, uv).rgb;\n  float amt = saturate(tex2D(lum, uv).x);\n"
              "  float3 x = c * c * lerp(%.4f, %.4f, amt) + b * b * lerp(%.4f, %.4f, amt);\n"
              "  x = 0.18 * pow(max(x, 1e-5) / 0.18, %.4f);\n"
              "  x = lerp(x, %.4f + (1.0 - %.4f) * tanh((x - %.4f) / (1.0 - %.4f)), step(%.4f, x));\n"
              "  float l = dot(x, float3(0.2126, 0.7152, 0.0722));\n  x = lerp(l.xxx, x, %.4f);\n"
              "  return float4(sqrt(saturate(x)), 1);\n}\n",
              c.sampler, c.bloomSampler, c.lumSampler, f.exposureNight, f.exposureDay, f.bloomNight, f.bloomDay, f.contrast, f.shoulder, f.shoulder, f.shoulder, f.shoulder, f.shoulder,
              f.saturation);
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "tscfix_film", nullptr, nullptr, "main", "ps_3_0", 1u << 15, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        Log("postfx: the new last pass did not compile (0x%08lx): %.300s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    IDirect3DPixelShader9* shader = nullptr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    code->Release();
    Log("postfx: last pass \"%s\" created 0x%08lx", f.name, hr);
    if (FAILED(hr)) return nullptr;
    g_filmFailed[index] = false;
    return g_film[index] = shader;
}

// The bright parts for the bloom. The game keeps saturate(colour - 0.65) * 6, which turns every
// bright area into flat white. This one keeps the colour and comes in smoothly from 55% up.
IDirect3DPixelShader9* g_bright[16] = {};
IDirect3DPixelShader9* BrightShader(IDirect3DDevice9* device, int sampler) {
    if (sampler < 0 || sampler >= 16) return nullptr;
    static bool failed[16];
    if (g_bright[sampler] || failed[sampler]) return g_bright[sampler];
    failed[sampler] = true;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    char source[512];
    sprintf_s(source,
              "sampler2D s : register(s%d);\nfloat4 main(float2 uv : TEXCOORD0) : COLOR {\n  float3 c = tex2D(s, uv).rgb;\n"
              "  float m = max(c.r, max(c.g, c.b));\n  float w = smoothstep(0.55, 1.0, m);\n  return float4(saturate(c * w * 1.6), 1);\n}\n",
              sampler);
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "tscfix_bright", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !code) return nullptr;
    IDirect3DPixelShader9* shader = nullptr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    code->Release();
    if (FAILED(hr)) return nullptr;
    failed[sampler] = false;
    return g_bright[sampler] = shader;
}

// Lamp light in daylight. The game switches lamps by the clock; their light is added to the picture
// at full strength also when the scene is bright. The game computes the brightness of the whole
// scene for its bloom (a 1 x 1 picture). The mod reads that value twice per second and weakens the
// light of point and spot lamps as the scene gets bright: their draws add light with the blend
// "one"; while a lamp-light shader is set, the mod makes that "blend factor" with the factor below.
IDirect3DSurface9* g_lumTarget = nullptr;   // the game's 1 x 1 scene-brightness target
IDirect3DSurface9* g_lumCopy = nullptr;     // system-memory copy
volatile float g_sceneBrightness = -1.0f;   // 0 to 1, -1 not read yet
volatile bool g_lampBound = false, g_lampBlendChanged = false;
bool g_glareBound = false;
int g_lampForced = -1;  // compare mode: -1 by the options, 0 lamps as the game has them, 1 faded
bool LampFade() { return g_lampForced >= 0 ? g_lampForced == 1 : g_opt.lampFade; }
float LampFactor() {
    float b = g_sceneBrightness;
    if (b < 0) return 1.0f;
    float t = (b - g_opt.lampFadeFrom) / (g_opt.lampFadeTo - g_opt.lampFadeFrom);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    t = t * t * (3 - 2 * t);
    return 1.0f - 0.85f * t;  // a little lamp light stays in full daylight
}
DWORD FactorColour() {
    DWORD k = static_cast<DWORD>(LampFactor() * 255.0f + 0.5f);
    return 0xFF000000u | (k << 16) | (k << 8) | k;
}
void LampEnter(IDirect3DDevice9* device) {
    g_lampBound = true;
    DWORD src = 0;
    if (!LampFade() || FAILED(device->GetRenderState(D3DRS_SRCBLEND, &src)) || src != D3DBLEND_ONE) return;
    device->SetRenderState(D3DRS_BLENDFACTOR, FactorColour());
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
    g_lampBlendChanged = true;
}
void LampLeave(IDirect3DDevice9* device) {
    g_lampBound = false;
    if (!g_lampBlendChanged) return;
    g_lampBlendChanged = false;
    DWORD src = 0;
    if (SUCCEEDED(device->GetRenderState(D3DRS_SRCBLEND, &src)) && src == D3DBLEND_BLENDFACTOR) device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
}
void NoteLumTarget(IDirect3DDevice9* device) {
    if (g_lumTarget) return;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    if (desc.Width == 1 && desc.Height == 1 && SUCCEEDED(device->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &g_lumCopy, nullptr))) {
        g_lumTarget = target;  // (keeps the reference)
        Log("postfx: the game's scene-brightness target is known (format %d)", desc.Format);
        return;
    }
    static int told = 0;
    if (told++ < 4) Log("postfx: scene-brightness target not found yet: the target is %ux%u", desc.Width, desc.Height);
    target->Release();
}
void ReadSceneBrightness(IDirect3DDevice9* device) {
    if (!g_lumTarget || !g_lumCopy || FAILED(device->GetRenderTargetData(g_lumTarget, g_lumCopy))) return;
    D3DLOCKED_RECT lr{};
    if (FAILED(g_lumCopy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    const uint8_t* px = static_cast<const uint8_t*>(lr.pBits);
    g_sceneBrightness = px[2] / 255.0f;  // red of a B8G8R8A8 pixel
    g_lumCopy->UnlockRect();
}

}  // namespace

void Configure(const Options& options) {
    g_opt = options;
    if (options.noLampLight) g_lampForced = 2, Log("postfx: (diagnosis) no lamp light (%s)", options.noLampLight == 1 ? "all" : options.noLampLight == 2 ? "point and spot lights" : "glow pass");
}
void SetActive(bool on) {
    if (g_opt.compare) return;
    g_fxaaOff = g_dofOff = g_filmForced = g_lampForced = on ? -1 : 0;
    resolve::ForceVariant(on ? -1 : 0);
}
bool Enabled() { return g_opt.noLampLight || g_opt.fxaa != 1 || !g_opt.depthOfField || g_opt.compare || g_opt.film > 0 || g_opt.lampFade; }
DWORD BlendValue(D3DRENDERSTATETYPE state, DWORD value) {
    if (state != D3DRS_SRCBLEND || value != D3DBLEND_ONE || !g_lampBound || !LampFade()) return value;
    g_lampBlendChanged = true;  // (the factor itself was set when the lamp shader was set)
    return D3DBLEND_BLENDFACTOR;
}
// Supersampling already smooths the edges; FXAA after it only blurs (measured at Haarlem: the far
// track is 40% sharper without it, the catenary 22%).
bool FxaaWanted() { return g_opt.fxaa < 0 ? !resolve::ShrinkInUse() : g_opt.fxaa == 1; }

IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    if (!shader) {
        if (g_lampBound) LampLeave(device);
        return shader;
    }
    if (g_glareBound) {  // the shader after the scene-brightness shader: its target is still set
        g_glareBound = false;
        NoteLumTarget(device);
    }
    const Classed& c = Class(shader);
    if (c.kind == kLamp) {
        if (g_lampForced == 2 && (g_opt.noLampLight == 1 || (g_opt.noLampLight == 2) != c.glow)) {  // (compare, diagnosis) no lamp light: the lamp shader gives black
            static IDirect3DPixelShader9* black = nullptr;
            static bool failed = false;
            if (!black && !failed) {
                failed = true;
                HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
                auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
                const char* source = "float4 main() : COLOR { return float4(0, 0, 0, 0); }";
                Blob* code = nullptr;
                Blob* errors = nullptr;
                if (compile && SUCCEEDED(compile(source, strlen(source), "tscfix_black", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errors)) && code) {
                    if (SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &black))) failed = false;
                    code->Release();
                }
                if (errors) errors->Release();
            }
            if (black) return black;
        }
        if (!g_lampBound) LampEnter(device);
        return shader;
    }
    if (g_lampBound) LampLeave(device);
    if (c.kind == kOther) return shader;
    if (c.kind == kGlare) {
        NoteLumTarget(device);
        g_glareBound = !g_lumTarget;
        return shader;
    }
    if (c.kind == kBright) {
        int film = g_filmForced >= 0 ? g_filmForced : g_opt.film;
        IDirect3DPixelShader9* made = film > 0 ? BrightShader(device, c.sampler) : nullptr;
        return made ? made : shader;
    }
    if (c.kind == kBloom) {
        int film = g_filmForced >= 0 ? g_filmForced : g_opt.film;  // 0: the game's pass
        IDirect3DPixelShader9* made = film > 0 ? FilmShader(device, film - 1, c) : nullptr;
        return made ? made : shader;
    }
    bool off = c.kind == kFxaa ? (g_fxaaOff >= 0 ? g_fxaaOff == 1 : !FxaaWanted()) : (g_dofOff >= 0 ? g_dofOff == 1 : !g_opt.depthOfField);
    if (!off) return shader;
    IDirect3DPixelShader9* copy = CopyShader(device, c.sampler);
    if (!copy) return shader;
    if (InterlockedIncrement(&g_replaced[c.kind]) == 1) Log("postfx: the %s pass is switched off (it copies its input)", c.kind == kFxaa ? "FXAA" : "depth-of-field");
    return copy;
}

// Compare mode: every 900 frames, the variants below are drawn one frame each and saved as
// pictures in %TEMP%\tscfix-picture (same scene when the train stands).
struct Variant {
    const char* name;
    int kernel;  // shrink filter: 0 the game's, 1 Lanczos
    int fxaaOff, dofOff;
    int film;    // 0 the game's last pass, 1.. one of kFilms
    int lampFade;
};
const Variant kVariants[] = {
    {"0-game", 0, 0, 0, 0, 0},
    {"1-sharp", 1, 1, 0, 0, 0},
    {"2-sharp-film", 1, 1, 0, 1, 0},
    {"3-sharp-film-lamps", 1, 1, 0, 1, 1},  // the mod's default
    {"4-no-lamp-light", 1, 1, 0, 1, 2},  // diagnosis: which light in the picture is lamp light
};
void OnPresent(IDirect3DDevice9* device) {
    static unsigned frames = 0;
    if (++frames % 30 == 0 && (g_opt.lampFade || g_opt.compare)) {
        ReadSceneBrightness(device);
        if (frames % 1800 == 0 && g_sceneBrightness >= 0)
            Log("postfx: scene brightness %.2f, lamp light at %.0f%%", g_sceneBrightness, LampFactor() * 100.0f);
    }
    if (!g_opt.compare) return;
    static unsigned frame = 0, set = 0;
    static int saving = -1;  // the variant of the frame that is about to be presented
    const int count = static_cast<int>(sizeof kVariants / sizeof kVariants[0]);
    ++frame;
    if (saving >= 0) {
        using SaveFn = HRESULT(WINAPI*)(const char*, DWORD, IDirect3DSurface9*, const void*, const RECT*);
        static SaveFn save = nullptr;
        if (!save)
            if (HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll")) save = reinterpret_cast<SaveFn>(GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA"));
        IDirect3DSurface9* back = nullptr;
        if (save && SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
            char dir[MAX_PATH], path[MAX_PATH];
            GetTempPathA(MAX_PATH, dir);
            strcat_s(dir, "tscfix-picture");
            CreateDirectoryA(dir, nullptr);
            sprintf_s(path, "%s/set%02u-%s.png", dir, set, kVariants[saving].name);
            HRESULT hr = save(path, 3 /* PNG */, back, nullptr, nullptr);
            if (saving == 0) Log("postfx: compare set %u at frame %u (0x%08lx) in %s", set, frame, hr, dir);
            back->Release();
        }
        saving = saving + 1 < count ? saving + 1 : -1;
        if (saving < 0) ++set;
    } else if (frame % 900 == 600 && set < 8) {
        saving = 0;
    }
    if (saving >= 0) {
        g_fxaaOff = kVariants[saving].fxaaOff, g_dofOff = kVariants[saving].dofOff, g_filmForced = kVariants[saving].film;
        g_lampForced = kVariants[saving].lampFade;
        resolve::ForceVariant(kVariants[saving].kernel);
    } else {
        g_fxaaOff = g_dofOff = g_filmForced = g_lampForced = -1;
        resolve::ForceVariant(-1);
    }
}

void OnReset() {
    for (auto& s : g_copy)
        if (s) s->Release(), s = nullptr;
    for (auto& s : g_film)
        if (s) s->Release(), s = nullptr;
    for (auto& s : g_bright)
        if (s) s->Release(), s = nullptr;
    if (g_lumTarget) g_lumTarget->Release(), g_lumTarget = nullptr;
    if (g_lumCopy) g_lumCopy->Release(), g_lumCopy = nullptr;
    g_lampBound = g_lampBlendChanged = false;
}

}  // namespace tscfix::postfx
