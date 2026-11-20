#include "postfx.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "physicsfix.h"
#include "resolve.h"

// The game's picture passes after the scene is drawn (read from the game's shader sources):
//   FXAA            PostProcessFXAA.fx: constants fxaaQualitySubpix ..., sampler LumaSampler
//   depth of field  PostDepthOfField.fx: constants gBlurDiscCoords ..., sampler BBTexSamp
// A pass is recognised by the names in its shader's constant table, the first time the shader is
// set. A pass that is switched off gets a shader that copies its input picture.
namespace tscfix::postfx {
namespace {

Options g_opt;
enum Kind : uint8_t { kUnseen = 0, kOther = 1, kFxaa = 2, kDof = 3, kBloom = 4, kBright = 5, kGlare = 6, kLamp = 7, kSun = 8 };
struct Classed {
    IDirect3DPixelShader9* shader;
    uint8_t kind;
    uint8_t sampler;  // the register of the pass's input picture
    uint8_t bloomSampler, lumSampler;  // (bloom pass) the blurred bright picture and the scene brightness
    uint32_t size;
    bool glow = false;  // (lamp) the glow pass, not a point or spot light
    uint8_t invProj = 0, viewInverse = 0;  // (sun light) the first register of the inverse projection and of the inverse view matrix
    uint16_t samplers = 0;                 // (bloom pass, glow mask) one bit per sampler register that the game's shader reads
    // (steady glow) 1 the mask pass (PSMask), 2 the last pass of technique HIGH (PSComposite_HIGH). For both:
    // `sampler` is ColourSampler, `bloomSampler` is AuxSampler, `packed` is the register that holds
    // abs(PackedDepth) (c0: the preshader writes it, the constant table does not name it).
    uint8_t glowPass = 0, packed = 0;
    uint8_t sets = 0;  // (kind kOther) sets since the code size was last compared (see Class)
    // (glow gain) 1 to 4: a blur pass of the glow, the entry of kBlurs plus one. `sampler` is Frame1Sampler,
    // `bloomSampler` is AuxSampler, `packed` is the register of `power`.
    uint8_t blur = 0;
};
constexpr size_t kSlots = 16384;  // power of two
constexpr size_t kProbe = 4;      // places tried for one shader before another shader loses its place
Classed g_classed[kSlots];
unsigned g_classEvict = 0;        // which of the places is given up next
IDirect3DPixelShader9* g_copy[16] = {};  // a copy shader per sampler register
int g_fxaaOff = -1, g_dofOff = -1;       // compare mode: -1 use the options, 0 pass on, 1 pass off
volatile LONG g_replaced[4] = {};
bool g_cameraWanted = false;             // (blend over time) the sun light shader is classed and its matrices are read

// The register of the constant `name` in a shader's constant table, or -1. `registers` gets the
// number of registers that the constant takes.
int ConstantRegister(const uint8_t* code, size_t size, const char* name, int* registers = nullptr) {
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
            uint16_t reg, regs;
            memcpy(&at, e, 4);
            memcpy(&reg, e + 6, 2);
            memcpy(&regs, e + 8, 2);
            if (base + at + strlen(name) < code + size && strcmp(reinterpret_cast<const char*>(base + at), name) == 0) {
                if (registers) *registers = regs;
                return reg;
            }
        }
        return -1;
    }
    return -1;
}

// One bit per sampler register (0 to 15) that the shader's constant table names.
uint16_t SamplerMask(const uint8_t* code, size_t size) {
    uint16_t mask = 0;
    for (size_t i = 0; i + 40 < size; i += 4) {
        if (memcmp(code + i, "CTAB", 4) != 0) continue;
        const uint8_t* base = code + i + 4;
        uint32_t count, info;
        memcpy(&count, base + 12, 4);
        memcpy(&info, base + 16, 4);
        for (uint32_t k = 0; k < count && k < 64; ++k) {
            const uint8_t* e = base + info + 20 * k;
            if (e + 20 > code + size) break;
            uint16_t set, reg, regs;
            memcpy(&set, e + 4, 2);
            memcpy(&reg, e + 6, 2);
            memcpy(&regs, e + 8, 2);
            if (set != 3) continue;  // 3: the sampler registers
            for (unsigned r = reg; r < 16u && r < static_cast<unsigned>(reg) + regs; ++r) mask |= static_cast<uint16_t>(1u << r);
        }
        break;
    }
    return mask;
}

// A one-way value (FNV-1a, 64 bits) of a shader's instructions: the version, each instruction and
// the end. Comments (the constant table, the preshader) are left out.
uint64_t InstructionHash(const uint8_t* code, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto add = [&](size_t at, size_t bytes) {
        for (size_t k = at; k < at + bytes; ++k) h = (h ^ code[k]) * 0x100000001b3ull;
    };
    if (size < 8) return 0;
    add(0, 4);
    size_t i = 4;
    while (i + 4 <= size) {
        uint32_t t;
        memcpy(&t, code + i, 4);
        uint32_t op = t & 0xFFFFu;
        if (op == 0xFFFFu) {
            add(i, 4);
            return h;
        }
        if (op == 0xFFFEu) {
            i += 4 + 4 * static_cast<size_t>((t >> 16) & 0x7FFFu);
            continue;
        }
        size_t bytes = 4 + 4 * static_cast<size_t>((t >> 24) & 0xFu);
        if (i + bytes > size) return 0;
        add(i, bytes);
        i += bytes;
    }
    return 0;  // no end
}

// (glow gain) The four blur shaders of PostTrainLightGlow.fx that a technique uses (HIGH and MEDIUM;
// no technique uses the LOW blur). Each is known by the value of its instructions in the game's
// PostTrainLightGlow.fx.o. The game's file gives each shader a preshader. It computes two values
// from effect constants and puts them in registers that the constant table does not name:
//   c[weights]      abs(PackedDepth)        (how the depth is stored)
//   c[weights + 1]  width / ViewSize.x or width / ViewSize.y   (the step between two samples)
// The named constants are BlurWeights_* at c0 (13 or 9 registers) and `power` at c[weights + 2].
struct Blur {
    uint64_t hash;
    uint8_t weights;
    bool vertical;
};
const Blur kBlurs[] = {
    {0x411229174d13dc36ull, 13, false},
    {0x9928f0783e963abcull, 13, true},
    {0xd5c59e8ee060050cull, 9, false},
    {0xe8084ff24c90fc5bull, 9, true},
};

// (steady glow) The values of the instructions of PSMask and PSComposite_HIGH in the game's file.
// Both do:  g = abs(PackedDepth) == 0 ? material.g : material.a;  colour = g > 0 ? picture.rgb : 0;
// PSMask gives (colour x g, g). PSComposite_HIGH gives half of that.
constexpr uint64_t kGlowMaskHash = 0xd58c18658a7e4a83ull;
constexpr uint64_t kGlowCompositeHash = 0x10b0086b5cd55c3aull;

Classed& Class(IDirect3DPixelShader9* shader) {
    size_t at = (reinterpret_cast<uintptr_t>(shader) >> 4) * 0x9E3779B97F4A7C15ull >> 50;
    // A shader has kProbe places in a row. With one place, two shaders in use on the same place
    // put each other out at every set (a copy of the code and a parse each time).
    Classed* found = nullptr;
    Classed* empty = nullptr;
    for (size_t n = 0; n < kProbe && !found; ++n) {
        Classed& e = g_classed[(at + n) & (kSlots - 1)];
        if (e.shader == shader) found = &e;
        else if (!e.shader && !empty) empty = &e;
    }
    UINT size = 0;
    if (found) {
        // The address can belong to a new shader after a release. A pass is compared by its code
        // size at every set. A shader that is no pass is compared once per 256 sets: that keeps
        // the common case free of a runtime call, and a pass made later at the address of a
        // released shader is still found.
        if (found->kind == kOther && ++found->sets != 0) return *found;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size == found->size) return *found;
    }
    Classed& c = found ? *found : empty ? *empty : g_classed[(at + (g_classEvict++ & (kProbe - 1))) & (kSlots - 1)];
    c = {shader, kOther, 0, 0, 0, 0};
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return c;
    std::vector<uint8_t> code(size + 8);
    if (FAILED(shader->GetFunction(code.data(), &size))) return c;
    c.size = size;
    int s, v, projRegs = 0, viewRegs = 0;
    bool glow = false;
    // (blend over time) The game's sun light shader (DeferredDirectional.fx, two variants). It gets the
    // camera of the frame as constants: InvProj (4 registers) and WorldViewInverse (3 registers).
    if (g_cameraWanted && ConstantRegister(code.data(), size, "AmbientLightColour") >= 0 &&
        (s = ConstantRegister(code.data(), size, "InvProj", &projRegs)) >= 0 && projRegs >= 4 && s + 4 <= 224 &&
        (v = ConstantRegister(code.data(), size, "WorldViewInverse", &viewRegs)) >= 0 && viewRegs >= 3 && v + 3 <= 224)
        c.kind = kSun, c.invProj = static_cast<uint8_t>(s), c.viewInverse = static_cast<uint8_t>(v);
    else if (ConstantRegister(code.data(), size, "fxaaQualitySubpix") >= 0 && (s = ConstantRegister(code.data(), size, "LumaSampler")) >= 0)
        c.kind = kFxaa, c.sampler = static_cast<uint8_t>(s);
    else if (ConstantRegister(code.data(), size, "gBlurDiscCoords") >= 0 && (s = ConstantRegister(code.data(), size, "BBTexSamp")) >= 0)
        c.kind = kDof, c.sampler = static_cast<uint8_t>(s);
    else if ((s = ConstantRegister(code.data(), size, "frameSamp")) >= 0 && ConstantRegister(code.data(), size, "Blur1ImgSamp") >= 0 &&
             ConstantRegister(code.data(), size, "AvgLumFinalSamp") >= 0) {
        c.kind = kBloom, c.sampler = static_cast<uint8_t>(s);
        c.bloomSampler = static_cast<uint8_t>(ConstantRegister(code.data(), size, "Blur1ImgSamp"));
        c.lumSampler = static_cast<uint8_t>(ConstantRegister(code.data(), size, "AvgLumFinalSamp"));
        c.samplers = SamplerMask(code.data(), size);
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
              ConstantRegister(code.data(), size, "BlurWeights_LOW") >= 0)) {
        c.kind = kLamp, glow = true;
        // (glow gain) The replacement reads the registers of the game's shader. It is made only for a
        // shader with the known instructions and the register layout that goes with them.
        uint64_t hash = InstructionHash(code.data(), size);
        int known = -1;
        for (int k = 0; k < 4; ++k)
            if (kBlurs[k].hash == hash) known = k;
        int weightRegs = 0, powerRegs = 0;
        int frame = ConstantRegister(code.data(), size, "Frame1Sampler"), aux = ConstantRegister(code.data(), size, "AuxSampler");
        int weights = known < 0 ? -1 : ConstantRegister(code.data(), size, kBlurs[known].weights == 13 ? "BlurWeights_HIGH" : "BlurWeights_MEDIUM", &weightRegs);
        int power = ConstantRegister(code.data(), size, "power", &powerRegs);
        if (known >= 0 && frame >= 0 && frame < 16 && aux >= 0 && aux < 16 && weights == 0 && weightRegs == kBlurs[known].weights && power == weightRegs + 2 &&
            powerRegs == 1) {
            c.blur = static_cast<uint8_t>(known + 1);
            c.sampler = static_cast<uint8_t>(frame), c.bloomSampler = static_cast<uint8_t>(aux), c.packed = static_cast<uint8_t>(power);
        }
        static volatile LONG toldBlur = 0;
        if (g_opt.glowFarGain < 1.0f && InterlockedIncrement(&toldBlur) <= 6) {
            if (c.blur)
                Log("postfx: glow gain: the game's blur shader is known (%d weights, %s; picture at sampler %d, depth at sampler %d, weights at c0, power at c%d)",
                    kBlurs[known].weights, kBlurs[known].vertical ? "vertical" : "horizontal", frame, aux, power);
            else
                Log("postfx: glow gain: a blur shader of the game is not a known one (instructions %08lx%08lx, weights at c%d x %d, power at c%d): it stays as the game has it",
                    static_cast<unsigned long>(hash >> 32), static_cast<unsigned long>(hash & 0xFFFFFFFFu), weights, weightRegs, power);
        }
    }
    else if (ConstantRegister(code.data(), size, "ColourSampler") >= 0 && ConstantRegister(code.data(), size, "AuxSampler") >= 0 &&
             ConstantRegister(code.data(), size, "ViewSpaceNormalSampler") < 0 && size < 1200) {
        c.kind = kLamp, glow = true;
        // (steady glow) PSMask and PSComposite_HIGH, each known by the value of its instructions in the
        // game's PostTrainLightGlow.fx.o. The constant table of each names the two samplers only.
        // PackedDepth is an input of the preshader: it puts abs(PackedDepth) in c0, a register that
        // the table does not name. The instructions read c0, and c1 is a constant of the shader.
        int colour = ConstantRegister(code.data(), size, "ColourSampler"), aux = ConstantRegister(code.data(), size, "AuxSampler");
        uint64_t hash = InstructionHash(code.data(), size);
        int pass = hash == kGlowMaskHash ? 1 : hash == kGlowCompositeHash ? 2 : 0;
        if (pass && colour < 16 && aux < 16 && ConstantRegister(code.data(), size, "PackedDepth") < 0) {
            c.sampler = static_cast<uint8_t>(colour), c.bloomSampler = static_cast<uint8_t>(aux), c.packed = 0;
            c.samplers = SamplerMask(code.data(), size);
            c.glowPass = static_cast<uint8_t>(pass);
        }
        static volatile LONG toldMask = 0;
        if (g_opt.steadyGlow && InterlockedIncrement(&toldMask) <= 6) {
            if (c.glowPass)
                Log("postfx: steady glow: the game's %s shader is known (colour at sampler %d, material at sampler %d, abs(PackedDepth) at c0)",
                    pass == 1 ? "mask" : "HIGH composite", colour, aux);
            else
                Log("postfx: steady glow: a shader with ColourSampler and AuxSampler is not a known one (instructions %08lx%08lx, %u bytes): it stays as the game has it",
                    static_cast<unsigned long>(hash >> 32), static_cast<unsigned long>(hash & 0xFFFFFFFFu), static_cast<unsigned>(size));
        }
    }
    c.glow = glow;
    if (glow) {
        static volatile LONG toldGlow = 0;
        if (InterlockedIncrement(&toldGlow) == 1) Log("postfx: the game's lamp glow shader is in use");
    } else if (c.kind == kSun) {
        static volatile LONG toldSun = 0;
        if (InterlockedIncrement(&toldSun) <= 4)
            Log("postfx: the game's sun light shader is in use (inverse projection at c%d, inverse view at c%d)", c.invProj, c.viewInverse);
    } else if (c.kind == kBright || c.kind == kGlare || c.kind == kLamp) {
        static volatile LONG told[8];
        if (InterlockedIncrement(&told[c.kind]) == 1)
            Log("postfx: the game's %s shader is in use", c.kind == kBright ? "bloom bright-parts" : c.kind == kGlare ? "scene-brightness" : "lamp light");
    } else if (c.kind != kOther) {
        static volatile LONG toldPass[8];  // (once per kind: a slot shared by two shaders is classified again at every set)
        if (InterlockedIncrement(&toldPass[c.kind]) == 1)
            Log("postfx: the game's %s pass is in use (input picture at sampler %d)", c.kind == kFxaa ? "FXAA" : c.kind == kDof ? "depth-of-field" : "bloom and tone",
                c.sampler);
    }
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
// The made last passes. The text of a pass names the sampler registers of the game's shader it
// stands in for (FilmSource), so a made shader belongs to (film, the three registers), not to the
// film alone. `used` with no shader: it did not compile, and is not tried again before a device reset.
struct FilmMade {
    bool used;
    int index;
    uint8_t sampler, bloomSampler, lumSampler;
    IDirect3DPixelShader9* shader;
    // (blend over time) The variant for the same registers, and the stage of its kept picture.
    IDirect3DPixelShader9* blend;
    bool blendTried;
    int blendStage;
};
FilmMade g_film[8] = {};  // (the game has one last-pass shader per quality setting: a few register sets at most)
int g_filmForced = -1;  // compare mode: -1 by the options, 0 the game's pass, 1.. a film (index + 1)

// (blend over time) The variant of the last pass that also reads the kept picture of the frame before.
constexpr int kBlendRegister = 221;  // (weight of the frame before, -, 1 / width, 1 / height); c222 and c223 belong to the LOD fade (psfade.h)
int g_blendStage = -1;  // the sampler stage of the kept picture for the last pass in use: one that the pass does not use

// The text of the last pass. `historyStage` < 0: the pass as it is. Otherwise the variant that blends
// with the kept picture at that sampler stage.
void FilmSource(char* source, size_t size, const Film& f, const Classed& c, int historyStage) {
    int n = sprintf_s(source, size, "sampler2D frame : register(s%d);\nsampler2D bloom : register(s%d);\nsampler2D lum : register(s%d);\n", c.sampler, c.bloomSampler,
                      c.lumSampler);
    if (historyStage >= 0)
        n += sprintf_s(source + n, size - n,
                       "sampler2D history : register(s%d);\nfloat4 blend : register(c%d);\n"
                       "float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n",
                       historyStage, kBlendRegister);
    else
        n += sprintf_s(source + n, size - n, "float4 main(float2 uv : TEXCOORD0) : COLOR {\n");
    n += sprintf_s(source + n, size - n,
                   "  float3 c = tex2D(frame, uv).rgb;\n  float3 b = tex2D(bloom, uv).rgb;\n  float amt = saturate(tex2D(lum, uv).x);\n"
                   "  float3 x = c * c * lerp(%.4f, %.4f, amt) + b * b * lerp(%.4f, %.4f, amt);\n"
                   "  x = 0.18 * pow(max(x, 1e-5) / 0.18, %.4f);\n"
                   "  x = lerp(x, %.4f + (1.0 - %.4f) * tanh((x - %.4f) / (1.0 - %.4f)), step(%.4f, x));\n"
                   "  float l = dot(x, float3(0.2126, 0.7152, 0.0722));\n  x = lerp(l.xxx, x, %.4f);\n",
                   f.exposureNight, f.exposureDay, f.bloomNight, f.bloomDay, f.contrast, f.shoulder, f.shoulder, f.shoulder, f.shoulder, f.shoulder, f.saturation);
    if (historyStage < 0) {
        sprintf_s(source + n, size - n, "  return float4(sqrt(saturate(x)), 1);\n}\n");
        return;
    }
    // The blend is done in linear light, the space of the pass itself. The kept picture holds finished
    // values (the square root of linear light), so its square is linear light again. The kept picture
    // is read at the centre of the pixel that is drawn (VPOS), not at `uv`: a part of a pixel between
    // the two would add up to a moving blur, frame after frame.
    // TODO (stage 4, move the history): read the kept picture at the place where this pixel was in the
    // frame before. That needs the game's depth picture on one more sampler and the two cameras
    // (g_camSeen and g_camLast) as constants. Sky has no depth: it moves with the camera turn only.
    // TODO (stage 5, reject): make blend.x smaller for near pixels and where that place is off screen.
    sprintf_s(source + n, size - n,
              "  x = saturate(x);\n  float3 h = tex2D(history, (vpos + 0.5) * blend.zw).rgb;\n"
              "  x = lerp(x, h * h, blend.x);\n  return float4(sqrt(x), 1);\n}\n");
}

IDirect3DPixelShader9* CompileFilm(IDirect3DDevice9* device, const Film& f, const Classed& c, int historyStage) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    char source[2400];
    FilmSource(source, sizeof source, f, c, historyStage);
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
    if (historyStage >= 0) Log("postfx: last pass \"%s\" with the blend over time created 0x%08lx (kept picture at sampler %d)", f.name, hr, historyStage);
    else Log("postfx: last pass \"%s\" created 0x%08lx", f.name, hr);
    return SUCCEEDED(hr) ? shader : nullptr;
}

IDirect3DPixelShader9* FilmShader(IDirect3DDevice9* device, int index, const Classed& c) {
    if (index < 0 || index >= kFilmCount) return nullptr;
    FilmMade* free = nullptr;
    for (FilmMade& m : g_film) {
        if (!m.used) {
            if (!free) free = &m;
            continue;
        }
        if (m.index == index && m.sampler == c.sampler && m.bloomSampler == c.bloomSampler && m.lumSampler == c.lumSampler) return m.shader;
    }
    if (!free) {  // more register sets than places: the game's own pass stays (all places come free at a device reset)
        static bool told = false;
        if (!told) told = true, Log("postfx: no place for one more last pass (samplers %d, %d, %d); the game's pass is used", c.sampler, c.bloomSampler, c.lumSampler);
        return nullptr;
    }
    *free = {true, index, c.sampler, c.bloomSampler, c.lumSampler, CompileFilm(device, kFilms[index], c, -1)};
    return free->shader;
}

// Made per register set, as the plain pass (FilmShader makes the entry first).
IDirect3DPixelShader9* FilmBlendShader(IDirect3DDevice9* device, int index, const Classed& c) {
    g_blendStage = -1;
    if (index < 0 || index >= kFilmCount || !FilmShader(device, index, c)) return nullptr;
    for (FilmMade& m : g_film) {
        if (!m.used || m.index != index || m.sampler != c.sampler || m.bloomSampler != c.bloomSampler || m.lumSampler != c.lumSampler) continue;
        if (!m.blendTried) {
            m.blendTried = true, m.blendStage = -1;
            for (int s = 15; s >= 0 && m.blendStage < 0; --s)
                if (!(c.samplers & (1u << s)) && s != c.sampler && s != c.bloomSampler && s != c.lumSampler) m.blendStage = s;
            if (m.blendStage >= 0) m.blend = CompileFilm(device, kFilms[index], c, m.blendStage);
        }
        g_blendStage = m.blendStage;
        return m.blend;
    }
    return nullptr;
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
// light of point and spot lamps as the scene gets bright. That value changes with what the camera
// sees (sky or no sky), so the lamps follow a slow average of it (time constant about 30 s): a lamp
// keeps its strength while the view changes, and follows dusk and dawn. Their draws add light with the blend
// "one"; while a lamp-light shader is set, the mod makes that "blend factor" with the factor below.
IDirect3DSurface9* g_lumTarget = nullptr;   // the game's 1 x 1 scene-brightness target
IDirect3DSurface9* g_lumCopy = nullptr;     // system-memory copy
volatile float g_sceneBrightness = -1.0f;   // 0 to 1, -1 not read yet
volatile float g_lampBrightness = -1.0f;    // its slow average, which the lamps follow
volatile bool g_lampBound = false, g_lampBlendChanged = false;
bool g_glareBound = false;
int g_lampForced = -1;  // compare mode: -1 by the options, 0 lamps as the game has them, 1 faded
bool LampFade() { return g_lampForced >= 0 ? g_lampForced == 1 : g_opt.lampFade; }
float LampFactor() {
    float b = g_lampBrightness;
    if (b < 0) return 1.0f;
    float t = (b - g_opt.lampFadeFrom) / (g_opt.lampFadeTo - g_opt.lampFadeFrom);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    t = t * t * (3 - 2 * t);
    return 1.0f - 0.9f * t;  // a little lamp light stays in full daylight
}
DWORD FactorColour() {
    DWORD k = static_cast<DWORD>(LampFactor() * 255.0f + 0.5f);
    return 0xFF000000u | (k << 16) | (k << 8) | k;
}
void LampEnter(IDirect3DDevice9* device) {
    g_lampBound = true;
    if (!LampFade()) return;
    // The factor first, whatever the blend is now: the game can set the blend "one" after the
    // shader, and BlendValue then uses the factor without setting it.
    device->SetRenderState(D3DRS_BLENDFACTOR, FactorColour());
    DWORD src = 0;
    if (FAILED(device->GetRenderState(D3DRS_SRCBLEND, &src))) return;
    if (src == D3DBLEND_BLENDFACTOR) {
        // The mod's own blend is still on the device although nothing says so (an earlier lamp
        // draw did not put "one" back). It is the mod's again from here, so the way out puts it back.
        static volatile LONG told = 0;
        if (InterlockedIncrement(&told) <= 3) Log("postfx: lamp fade: the blend was still \"blend factor\" when a lamp shader was set");
        g_lampBlendChanged = true;
        return;
    }
    if (src != D3DBLEND_ONE) return;
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
    g_lampBlendChanged = true;
}
void LampLeave(IDirect3DDevice9* device) {
    g_lampBound = false;
    // One read of the blend per way out of a lamp shader (not per shader set), also when the flag
    // says the mod changed nothing while the fade is on: a "blend factor" left on the device would
    // weaken every later draw that adds light.
    bool changed = g_lampBlendChanged;
    if (!changed && !LampFade()) return;
    g_lampBlendChanged = false;
    DWORD src = 0;
    if (FAILED(device->GetRenderState(D3DRS_SRCBLEND, &src)) || src != D3DBLEND_BLENDFACTOR) return;
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    static volatile LONG told = 0;
    if (!changed && InterlockedIncrement(&told) <= 3) Log("postfx: lamp fade: \"blend factor\" was left on the device; \"one\" put back");
}
void NoteLumTarget(IDirect3DDevice9* device) {
    if (g_lumTarget) return;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    // (ReadSceneBrightness reads the red byte of a 32-bit pixel: only these two formats have it there.)
    bool bytes = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
    if (bytes && desc.Width == 1 && desc.Height == 1 && SUCCEEDED(device->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &g_lumCopy, nullptr))) {
        g_lumTarget = target;  // (keeps the reference)
        Log("postfx: the game's scene-brightness target is known (format %d)", desc.Format);
        return;
    }
    static int told = 0;
    if (told++ < 4) Log("postfx: scene-brightness target not found yet: the target is %ux%u, format %d", desc.Width, desc.Height, desc.Format);
    target->Release();
}
void ReadSceneBrightness(IDirect3DDevice9* device) {
    if (!g_lumTarget || !g_lumCopy || FAILED(device->GetRenderTargetData(g_lumTarget, g_lumCopy))) return;
    D3DLOCKED_RECT lr{};
    if (FAILED(g_lumCopy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    const uint8_t* px = static_cast<const uint8_t*>(lr.pBits);
    float b = px[2] / 255.0f;  // red of a B8G8R8A8 pixel (NoteLumTarget takes no other format)
    g_sceneBrightness = b;
    float avg = g_lampBrightness;
    g_lampBrightness = avg < 0 ? b : avg + (b - avg) / 60.0f;  // (two readings per second)
    g_lumCopy->UnlockRect();
}

// --- Blend over time (work-items: blend-over-time.md, stages 1 to 3) ---------------------------
// Far catenary wires are thinner than one rendered point: pieces of a wire show and vanish from
// frame to frame, also while the train stands. A pass after the frame can not bring back a piece
// that no point hit. A blend over time can: each pixel also takes a part of the frame before.
//   Stage 1  Does the camera stand? The camera rest of physicsfix.cpp says so (RestGate below).
//            The camera of each frame, read from the constants of the game's sun light shader,
//            can only forbid the blend.
//   Stage 2  A kept picture: the frame before, as the mod's last pass drew it.
//   Stage 3  The last pass blends the new frame with the kept picture, only while the camera stands.
// Stage 4 (move the kept picture to where each pixel was) and stage 5 (reject where that is wrong)
// are not built; see the TODO notes in FilmSource and FilmEnter.

// Stage 1. The pixel shader constants as the game set them (all hooks of SetPixelShaderConstantF
// call OnPixelConstants). The matrices are read when the sun light shader is replaced by the next
// shader: then they hold what its draw used, whether the game sets constants before or after the
// shader.
constexpr UINT kPsRegisters = 224;
float g_psConst[kPsRegisters][4];
unsigned g_psFrame[kPsRegisters];  // the frame in which each register was set last
struct Camera {
    bool valid = false;
    float invProj[16];      // InvProj, four registers
    float viewInverse[12];  // WorldViewInverse, three registers: xyz one column of the turn, w one coordinate of the position
};
// Checked in the game: WorldViewInverse is relative to the camera. Its position is 0 0 0 and the
// path is 0 m while the train drives, so these matrices show a turn of the camera only. A drive on
// straight track counts as standing here. For that reason they do not switch the blend on: the
// camera rest does (RestGate). They only forbid it (g_moved).
Camera g_camSeen;  // read in this frame (not valid when the sun light shader was not used)
Camera g_camLast;  // of the frame before
bool g_sunBound = false;
uint8_t g_sunInvProj = 0, g_sunViewInverse = 0;
bool g_judged = false;   // the camera of this frame is compared with the frame before
bool g_still = false;    // the matrices did not change since the frame before
bool g_moved = false;    // the matrices changed since the frame before: no blend
// The camera rest (physicsfix.cpp, [fixes] camera_rest) is the "camera stands" signal. The cab
// camera's update runs on the game thread, one or more frames before this thread draws that frame.
// So the blend needs kRestRow updates in a row at rest (the frame that is drawn now is then one of
// them), and one update that passed through stops the blend at once, also for frames that were
// built before it. No cab camera update for kRestGapMs (another camera, a menu) is no blend.
constexpr unsigned kRestRow = 4;
constexpr ULONGLONG kRestGapMs = 200;
enum RestGateState { kRestStands, kRestShortRow, kRestNoUpdate, kRestOff, kRestNotInstalled };
const char* const kRestGateText[] = {"at rest", "passed through or less than 4 updates at rest", "no cab camera update for 0.2 s", "switched off", "not installed"};
RestGateState g_restGate = kRestNotInstalled;  // of this frame
unsigned g_restRow = 0;                        // updates in a row at rest, as read in this frame
unsigned g_frame = 1;    // counts Present calls
int g_sunPasses = 0, g_filmPasses = 0;  // in this frame
// For the log line, since the line before.
struct BlendStats {
    unsigned frames, still, moved, noCamera, blended, jumps, differ;
    unsigned restStands, restShort, restNone;  // frames by the camera rest: stands, passed through, no signal
    unsigned restVeto;                         // frames where the camera rest stands and the matrices changed
    unsigned notBlended[8];                    // frames at rest that were not blended, per reason (NoBlendReason)
    unsigned notSet;              // sun passes whose matrices were not set in their frame
    double path;                  // metres that the camera went
    float stillMove, stillTurn;   // the largest change in a frame that counted as standing
} g_stats{};
ULONGLONG g_statsSince = 0;

// Why a frame at rest is not blended. Each reason is counted for the taa line and written once.
enum NoBlendReason { kNoSecondPass, kNoTarget, kNoPicture, kNoFresh, kNoMatrices, kNoSwitch, kNoShader, kNoStage, kNoBlendReasons };
const char* const kNoBlendText[kNoBlendReasons] = {
    "a second last pass in the frame", "the target of the last pass can not be read", "the kept picture was not made", "the kept picture is not of the frame before",
    "the sun pass matrices moved", "the blend is switched off (A/B off phase, taa_show_history or picture_compare)", "the blend variant of the last pass was not made",
    "the sampler stage of the kept picture can not be used"};
void NotBlended(NoBlendReason why, unsigned a = 0, unsigned b = 0) {
    if (g_restGate != kRestStands) return;
    ++g_stats.notBlended[why];
    static bool told[kNoBlendReasons] = {};
    if (told[why]) return;
    told[why] = true;
    Log("postfx: blend over time: a frame at rest is not blended: %s (frame %u, values %u and %u)", kNoBlendText[why], g_frame, a, b);
}

// How far two cameras are apart: the position in metres, and the largest change of one value of the
// turn or of the projection (the projection relative to its size).
void Difference(const Camera& a, const Camera& b, float* move, float* turn) {
    float m = 0, t = 0;
    for (int i = 0; i < 12; ++i) {
        float d = fabsf(a.viewInverse[i] - b.viewInverse[i]);
        if (i % 4 == 3) m += d * d;
        else if (d > t) t = d;
    }
    for (int i = 0; i < 16; ++i) {
        float size = fabsf(a.invProj[i]) > fabsf(b.invProj[i]) ? fabsf(a.invProj[i]) : fabsf(b.invProj[i]);
        float d = fabsf(a.invProj[i] - b.invProj[i]) / (size > 1.0f ? size : 1.0f);
        if (d > t) t = d;
    }
    *move = sqrtf(m), *turn = t;
}

void SunLeave() {
    g_sunBound = false;
    // The game sets the constants of a pass each time the pass begins. A matrix that was not set in
    // this frame is an old one (the hook can miss calls for some frames, see Rearm in d3dlayer.cpp):
    // then this frame has no camera, and no blend.
    if (g_psFrame[g_sunViewInverse] != g_frame || g_psFrame[g_sunInvProj] != g_frame) {
        ++g_stats.notSet;
        return;
    }
    Camera c;
    memcpy(c.invProj, g_psConst[g_sunInvProj], sizeof c.invProj);
    memcpy(c.viewInverse, g_psConst[g_sunViewInverse], sizeof c.viewInverse);
    float sum = 0;
    for (float v : c.viewInverse) sum += fabsf(v);
    if (!(sum > 0) || sum > 1e12f) return;  // never set, or not a number
    c.valid = true;
    if (g_sunPasses++ > 0 && g_camSeen.valid) {  // more than one sun pass in a frame: do they have one camera?
        float move, turn;
        Difference(c, g_camSeen, &move, &turn);
        if (move > 0.001f || turn > 0.0001f) ++g_stats.differ;
    }
    g_camSeen = c;  // (the last one of the frame counts)
}

RestGateState RestGate(unsigned* row) {
    physicsfix::CameraRestInfo r = physicsfix::CameraRestState();
    *row = r.restUpdates;
    if (!r.installed) return kRestNotInstalled;
    if (!r.active) return kRestOff;
    ULONGLONG now = GetTickCount64();
    if (!r.updateTick || now - r.updateTick > kRestGapMs) return kRestNoUpdate;
    return r.restUpdates >= kRestRow ? kRestStands : kRestShortRow;
}

// Once per frame, when the last pass is set (the sun pass is drawn before it) or at Present.
void JudgeCamera() {
    if (g_judged) return;
    g_judged = true;
    g_still = g_moved = false;
    ++g_stats.frames;
    g_restGate = RestGate(&g_restRow);
    if (g_restGate == kRestStands) ++g_stats.restStands;
    else if (g_restGate == kRestShortRow) ++g_stats.restShort;
    else ++g_stats.restNone;
    if (g_camSeen.valid && g_camLast.valid) {
        float move, turn;
        Difference(g_camSeen, g_camLast, &move, &turn);
        g_still = move <= g_opt.stillMove && turn <= g_opt.stillTurn;
        if (g_still) {
            ++g_stats.still;
            if (move > g_stats.stillMove) g_stats.stillMove = move;
            if (turn > g_stats.stillTurn) g_stats.stillTurn = turn;
        } else {
            g_moved = true;
            ++g_stats.moved;
            if (g_restGate == kRestStands) ++g_stats.restVeto;
        }
        if (move < 50.0f) g_stats.path += move;
        else ++g_stats.jumps;  // (a new origin of the coordinates, or another camera)
    } else {
        ++g_stats.noCamera;
    }
    g_camLast = g_camSeen;  // (not valid when this frame had no sun pass: the next frame does not blend either)
}

// Stage 2. The kept picture: a texture with the size and format of the target of the last pass (with
// supersampling that target is larger than the window: the shrink pass comes after the last pass),
// with the frame before, as the last pass drew it. It is copied from the target of the last pass when the next shader is set, so the game's 2D
// display, drawn after it, is not in it. (A copy of the back buffer at Present would hold the 2D
// display: each frame would then blend the display of the frame before under the new one.)
IDirect3DTexture9* g_history = nullptr;
IDirect3DSurface9* g_historySurface = nullptr;
IDirect3DTexture9* g_spare = nullptr;  // (taa_show_history) the second picture
IDirect3DSurface9* g_spareSurface = nullptr;
UINT g_historyWidth = 0, g_historyHeight = 0;  // of the kept picture, or of the size that could not be made
D3DFORMAT g_historyFormat = D3DFMT_UNKNOWN;
unsigned g_historyFrame = 0;  // the frame whose picture is kept (0: none)
volatile bool g_blendActive = true;  // in-session A/B

// (Compare mode draws another variant each frame: no blend between them. In the off phase of an
// A/B window no picture is kept either, so the off phase has none of the cost.)
bool HistoryWanted() { return ((g_opt.blendOverTime && g_blendActive) || g_opt.showHistory) && !g_opt.compare; }
bool BlendWanted() { return g_opt.blendOverTime && g_blendActive && !g_opt.showHistory && !g_opt.compare; }

bool MakePicture(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc, IDirect3DTexture9** texture, IDirect3DSurface9** surface) {
    if (FAILED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, texture, nullptr)) || !*texture) return false;
    if (SUCCEEDED((*texture)->GetSurfaceLevel(0, surface)) && *surface) return true;
    (*texture)->Release(), *texture = nullptr;
    return false;
}
// `desc`: the target of the last pass. The kept picture is made again when that target changes.
bool EnsureHistory(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
    if (desc.Width == g_historyWidth && desc.Height == g_historyHeight && desc.Format == g_historyFormat) return g_history != nullptr;
    if (g_historySurface) g_historySurface->Release(), g_historySurface = nullptr;
    if (g_history) g_history->Release(), g_history = nullptr;
    if (g_spareSurface) g_spareSurface->Release(), g_spareSurface = nullptr;
    if (g_spare) g_spare->Release(), g_spare = nullptr;
    g_historyFrame = 0;
    g_historyWidth = desc.Width, g_historyHeight = desc.Height, g_historyFormat = desc.Format;  // (also when it is not made: not tried again for this target)
    bool made = MakePicture(device, desc, &g_history, &g_historySurface);
    if (made && g_opt.showHistory && !MakePicture(device, desc, &g_spare, &g_spareSurface)) Log("postfx: blend over time: no second picture, taa_show_history does nothing");
    Log("postfx: blend over time: kept picture of %ux%u (format %d, the target of the last pass at its draw call) %s", desc.Width, desc.Height, desc.Format, made ? "made" : "not made: no blend");
    return made;
}

// Stage 3. While the blend variant of the last pass is set, the kept picture is on a sampler stage
// that the game's pass does not use. The stage gets its texture and settings back when the pass ends.
const D3DSAMPLERSTATETYPE kHistoryStates[] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};
const DWORD kHistoryValues[] = {D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE};
constexpr int kHistoryStateCount = static_cast<int>(sizeof kHistoryStates / sizeof kHistoryStates[0]);
// A sampler stage that holds a picture of the mod for the time of one pass, and what was on it before.
struct HeldStage {
    int stage = -1;                           // -1: nothing is held
    IDirect3DBaseTexture9* ours = nullptr;    // (not counted)
    IDirect3DBaseTexture9* before = nullptr;  // (counted)
    DWORD states[kHistoryStateCount];
};
HeldStage g_filmStage;
bool g_filmBound = false;
IDirect3DPixelShader9* g_filmFor = nullptr;    // the game's shader of the last pass that is set now
IDirect3DPixelShader9* g_filmGiven = nullptr;  // what the mod set in its place
IDirect3DSurface9* g_filmTarget = nullptr;     // where the last pass draws (reference kept until the pass ends)

// `filter`: D3DTEXF_POINT, or D3DTEXF_LINEAR for a picture that is read at another size.
bool BindStage(IDirect3DDevice9* device, HeldStage& h, int at, IDirect3DBaseTexture9* texture, DWORD filter, const char* what) {
    DWORD stage = static_cast<DWORD>(at);
    IDirect3DBaseTexture9* before = nullptr;
    bool known = SUCCEEDED(device->GetTexture(stage, &before));
    for (int i = 0; known && i < kHistoryStateCount; ++i) known = SUCCEEDED(device->GetSamplerState(stage, kHistoryStates[i], &h.states[i]));
    if (!known) {  // what is on the stage can not be put back: the kept picture is not used
        if (before) before->Release();
        static int told = 0;
        if (told++ < 2) Log("postfx: %s: the state of sampler %lu can not be read, the kept picture is not used", what, stage);
        return false;
    }
    device->SetTexture(stage, texture);
    for (int i = 0; i < kHistoryStateCount; ++i) {
        bool isFilter = kHistoryStates[i] == D3DSAMP_MAGFILTER || kHistoryStates[i] == D3DSAMP_MINFILTER;
        device->SetSamplerState(stage, kHistoryStates[i], isFilter ? filter : kHistoryValues[i]);
    }
    h.ours = texture, h.before = before;
    h.stage = at;
    return true;
}
void UnbindStage(IDirect3DDevice9* device, HeldStage& h) {
    if (h.stage < 0) return;
    DWORD stage = static_cast<DWORD>(h.stage);
    h.stage = -1;
    IDirect3DBaseTexture9* now = nullptr;
    bool ours = SUCCEEDED(device->GetTexture(stage, &now)) && now == h.ours;
    if (now) now->Release();
    if (ours) {  // (when the game, or a state block, put another texture there, the stage is left as it is)
        device->SetTexture(stage, h.before);
        for (int i = 0; i < kHistoryStateCount; ++i) device->SetSamplerState(stage, kHistoryStates[i], h.states[i]);
    }
    if (h.before) h.before->Release(), h.before = nullptr;
    h.ours = nullptr;
}
void DropStage(HeldStage& h) {  // (at a device reset: no call to the device)
    if (h.before) h.before->Release(), h.before = nullptr;
    h.ours = nullptr;
    h.stage = -1;
}
bool BindHistory(IDirect3DDevice9* device) { return BindStage(device, g_filmStage, g_blendStage, g_history, D3DTEXF_POINT, "blend over time"); }
void UnbindHistory(IDirect3DDevice9* device) { UnbindStage(device, g_filmStage); }

// The game's last pass is set and the mod's pass `plain` takes its place. Returns the shader to set:
// always `plain`. The blend variant is not chosen here: an effect pass of this engine sets its pixel
// shader before its other states, and its render target can be set after the shader. The target that
// GetRenderTarget gives at this moment can be the target of the pass before (it was the half-size
// glow target of 1024x576 in the game). The target is read at the first draw call of the pass (FilmDraw).
bool g_filmPending = false;  // the last pass is set and its first draw call did not come yet
int g_filmIndex = 0;         // the film of the pass that is set
Classed g_filmClass{};       // its class (a copy: the place in g_classed can get another shader)
IDirect3DPixelShader9* FilmEnter(IDirect3DDevice9*, IDirect3DPixelShader9* game, const Classed& c, int index, IDirect3DPixelShader9* plain) {
    JudgeCamera();
    g_filmBound = true;
    g_filmFor = game, g_filmGiven = plain;
    if (++g_filmPasses > 1) return NotBlended(kNoSecondPass), plain;  // a second picture in one frame: not blended and not kept
    g_filmPending = true;
    g_filmIndex = index, g_filmClass = c;
    return plain;
}

// The first draw call of the last pass, before it is drawn: all states of the pass are set now. The
// target is read here, the kept picture gets its size, and the blend variant is set when the frame
// can be blended.
void FilmDraw(IDirect3DDevice9* device) {
    g_filmPending = false;
    if (!g_filmBound || !g_filmFor) return;
    const Classed& c = g_filmClass;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return NotBlended(kNoTarget);
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    static UINT toldWidth = 0, toldHeight = 0;
    if (desc.Width != toldWidth || desc.Height != toldHeight) {  // (once per size) what the pass draws to and what it reads
        toldWidth = desc.Width, toldHeight = desc.Height;
        IDirect3DBaseTexture9* base = nullptr;
        IDirect3DTexture9* scene = nullptr;
        D3DSURFACE_DESC sd{};
        if (SUCCEEDED(device->GetTexture(static_cast<DWORD>(c.sampler), &base)) && base) {
            if (base->GetType() == D3DRTYPE_TEXTURE) scene = static_cast<IDirect3DTexture9*>(base), scene->GetLevelDesc(0, &sd);
            base->Release();
        }
        Log("postfx: blend over time: the last pass draws to a target of %ux%u (format %d), read at its first draw call; its scene picture at sampler %d is %ux%u (frame %u)",
            desc.Width, desc.Height, desc.Format, c.sampler, sd.Width, sd.Height, g_frame);
    }
    // The kept picture has the size of this target (with supersampling the target is larger than the back buffer).
    if (!EnsureHistory(device, desc)) {
        target->Release();
        return NotBlended(kNoPicture, desc.Width, desc.Height);
    }
    g_filmTarget = target;  // (FilmLeave copies it to the kept picture)
    // The weight of the frame before. None when the kept picture is not of the frame before (first
    // frame, after a device reset, after a menu), when the camera rest does not say that the cab
    // camera stands (RestGate), and when the matrices of the sun pass changed (g_moved).
    // TODO (stage 4): with the kept picture moved to where each pixel was, the blend can stay on while
    // the camera moves. The shader then needs the camera of this frame (g_camSeen) and of the frame
    // before as constants. JudgeCamera has put this frame in g_camLast already: keep the frame before
    // in one more variable there.
    // TODO (stage 5): things that move while the camera stands (a passing train, a signal that changes)
    // and wipers and rain leave a trail of about 10 frames at the default weight of 75% (about 20
    // frames at 85%). Stage 5 makes the weight smaller per pixel, by distance.
    bool fresh = g_historyFrame != 0 && g_historyFrame + 1 == g_frame;
    float weight = fresh && g_restGate == kRestStands && !g_moved && BlendWanted() ? g_opt.blendWeight : 0.0f;
    if (weight <= 0.0f) {
        if (!BlendWanted()) NotBlended(kNoSwitch);
        else if (!fresh) NotBlended(kNoFresh, g_historyFrame, g_frame);
        else if (g_moved) NotBlended(kNoMatrices);
        return;
    }
    IDirect3DPixelShader9* blend = FilmBlendShader(device, g_filmIndex, c);
    if (!blend) return NotBlended(kNoShader, static_cast<unsigned>(g_blendStage + 1), c.samplers);
    if (!BindHistory(device)) return NotBlended(kNoStage, static_cast<unsigned>(g_blendStage));
    const float k[4] = {weight, 0.0f, 1.0f / static_cast<float>(g_historyWidth), 1.0f / static_cast<float>(g_historyHeight)};
    device->SetPixelShaderConstantF(kBlendRegister, k, 1);
    // The game's shader is set once more through the device, so each hook of SetPixelShader (and the
    // state filter) sees it: Substitute gives the blend variant for it now.
    IDirect3DPixelShader9* plain = g_filmGiven;
    g_filmGiven = blend;
    if (FAILED(device->SetPixelShader(g_filmFor)) || !g_filmBound || g_filmGiven != blend) {
        // (not set: the plain pass draws, and the stage of the kept picture gets its texture back)
        if (g_filmBound) g_filmGiven = plain, UnbindHistory(device);
        return NotBlended(kNoShader, 0, 0);
    }
    ++g_stats.blended;
}

// The last pass ends (the next shader is set, or the frame is presented): the stage of the kept
// picture gets its texture back, and the picture that the pass drew is kept for the next frame.
void FilmLeave(IDirect3DDevice9* device) {
    g_filmBound = false;
    g_filmPending = false;
    g_filmFor = g_filmGiven = nullptr;
    UnbindHistory(device);
    if (!g_filmTarget) return;
    IDirect3DSurface9* target = g_filmTarget;
    g_filmTarget = nullptr;
    bool kept = false;
    if (g_opt.showHistory && g_spareSurface) {
        // (diagnosis) The new frame goes to the second picture, the frame before goes to the screen in
        // its place, and the two pictures change places. The screen then shows each frame one frame late.
        kept = SUCCEEDED(device->StretchRect(target, nullptr, g_spareSurface, nullptr, D3DTEXF_NONE));
        if (kept && g_historyFrame != 0 && g_historyFrame + 1 == g_frame) device->StretchRect(g_historySurface, nullptr, target, nullptr, D3DTEXF_NONE);
        if (kept) {
            IDirect3DTexture9* t = g_history;
            IDirect3DSurface9* s = g_historySurface;
            g_history = g_spare, g_historySurface = g_spareSurface;
            g_spare = t, g_spareSurface = s;
        }
    } else {
        kept = SUCCEEDED(device->StretchRect(target, nullptr, g_historySurface, nullptr, D3DTEXF_NONE));
    }
    g_historyFrame = kept ? g_frame : 0;
    target->Release();
}

void ReleaseHistory() {
    DropStage(g_filmStage);
    if (g_filmTarget) g_filmTarget->Release(), g_filmTarget = nullptr;
    if (g_historySurface) g_historySurface->Release(), g_historySurface = nullptr;
    if (g_history) g_history->Release(), g_history = nullptr;
    if (g_spareSurface) g_spareSurface->Release(), g_spareSurface = nullptr;
    if (g_spare) g_spare->Release(), g_spare = nullptr;
    g_historyWidth = g_historyHeight = 0;
    g_historyFormat = D3DFMT_UNKNOWN;
    g_historyFrame = 0;
    g_filmBound = false;
    g_filmPending = false;
    g_filmFor = g_filmGiven = nullptr;
}

// --- Steady glow ([graphics] steady_glow) --------------------------------------------------------
// The glow around lamps (PostTrainLightGlow.fx) can alternate from frame to frame while nothing
// moves: its mask of lamp pixels is not the same in each frame. The passes, from the game's source:
//   1  PSMask              draws the mask (colour x glow, glow) into the target Frame1 (half size, no blend)
//   2  PSBlurHorizontal_*  reads Frame1, adds to the picture (blend one + one)   } not in technique LOW
//   3  PSBlurVertical_*    reads Frame1, adds to the picture                     }
//   4  PSComposite         reads Frame1, adds half of it (MEDIUM and LOW), or
//      PSComposite_HIGH    computes the mask again from the G-buffer at full size and adds half of it
// The mod's mask shader computes the game's mask m(n) and writes two pictures at once:
//   kept(n)  = max(m(n), keep x kept(n-1) - 1.5/255)   to a picture of the mod (second render target)
//   shown(n) = max(m(n), kept(n-1))                    to Frame1, in place of m(n)
// A mask that alternates 1, 0, 1, 0 gives kept = 1, keep, 1, keep and shown = 1, 1, 1, 1: steady. A
// lamp that goes off gives shown = 1, keep, keep^2, ...: with keep = 0.6 it is below 8% after 5 frames.
// The same trail is behind a lamp when the camera moves. The small amount that is taken off each
// frame makes the kept value reach zero: with 8 bits, 0.6 x 1/255 would round to 1/255 for ever, and
// the blur passes multiply far lamps by up to 175.
// A copy of Frame1 as the picture of the frame before does not do this: Frame1 would then hold
// shown(n-1), and max(m, keep x shown(n-1)) gives 1, keep, 1, keep for the alternating mask.
// PSComposite_HIGH does not read Frame1. Its replacement adds half of max(m(n), kept(n-1)).
IDirect3DTexture9* g_glowTex[2] = {};  // the kept mask of this frame and of the frame before, in turn
IDirect3DSurface9* g_glowSurf[2] = {};
UINT g_glowWidth = 0, g_glowHeight = 0;
bool g_glowFailed = false;
int g_glowWrite = 0;         // the picture that the mask pass wrote last
unsigned g_glowFrame = 0;    // the frame in which it was written (0: none)
int g_glowBound = 0;         // 1 the mod's mask shader is set, 2 the mod's composite shader
bool g_glowTargetSet = false;
IDirect3DPixelShader9* g_glowFor = nullptr;    // the game's shader that is set now
IDirect3DPixelShader9* g_glowGiven = nullptr;  // what the mod set in its place
HeldStage g_glowStage;
volatile bool g_glowActive = true;     // in-session A/B of steady_glow
volatile bool g_pictureActive = true;  // in-session A/B "picture": off = the game's own passes
struct GlowShader {
    IDirect3DPixelShader9* shader = nullptr;
    bool tried = false;
    uint32_t registers = 0, size = 0;  // what the text was made for
} g_glowShader[2];  // the mask pass, the composite pass
struct GlowStats {
    unsigned masks, held, composites, notReplaced;
} g_glowStats{};
ULONGLONG g_glowStatsSince = 0;

// --- Glow gain ([graphics] glow_far_gain_percent) -------------------------------------------------
// The blur passes of the glow add  (power / widthDepthBias) x weight x mask  to the picture, with
// widthDepthBias = lerp(1.0, 0.01, depth). The gain is `power` (1.75) for a lamp at the camera and
// 100 x power (175) for a lamp at the far plane. The mod's blur shaders are the game's, with the gain
//   power / max(widthDepthBias, floor),   floor = 0.01 / share,   share = glow_far_gain_percent / 100.
// The largest gain is then power / floor = share x (power / 0.01): the given share of the game's
// largest gain. share = 1 gives floor = 0.01, the game's own shader (no replacement). share = 0.2
// gives floor = 0.05: gain 35 at most, and no change for lamps with widthDepthBias above 0.05. The
// width of the blur is not changed.
// Checked offline (fxc, the same compiler as d3dcompiler_47.dll): with "power / bias" this text gives
// the same instructions as each of the game's four blur shaders. With the floor, the gain has one
// more instruction (cmp).
volatile bool g_gainActive = true;  // in-session A/B "glow_gain"
struct GainShader {
    IDirect3DPixelShader9* shader = nullptr;
    bool tried = false;
    uint32_t registers = 0;  // what the text was made for
} g_gainShader[4];
volatile LONG g_gainSets = 0;

bool GainWanted() { return g_opt.glowFarGain < 1.0f && g_gainActive && g_pictureActive && !g_opt.compare; }

IDirect3DPixelShader9* GainShaderFor(IDirect3DDevice9* device, const Classed& c) {
    if (c.blur < 1 || c.blur > 4) return nullptr;
    GainShader& g = g_gainShader[c.blur - 1];
    const Blur& b = kBlurs[c.blur - 1];
    uint32_t registers = c.sampler | (c.bloomSampler << 8) | (c.packed << 16);
    if (g.tried && g.registers == registers) return g.shader;
    if (g.shader) g.shader->Release(), g.shader = nullptr;
    g.tried = true, g.registers = registers;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    int n = b.weights, half = n / 2;
    double floor = 0.01 / g_opt.glowFarGain;
    char source[1400];
    sprintf_s(source,
              "sampler2D src : register(s%d);\nsampler2D aux : register(s%d);\nfloat weights[%d] : register(c0);\nfloat packedAbs : register(c%d);\n"
              "float step : register(c%d);\nfloat power : register(c%d);\nfloat4 main(float2 uv : TEXCOORD0) : COLOR {\n  float4 colour = 0;\n"
              "  float3 a = tex2D(aux, uv).xyz;\n  float f = dot(a, float3(65536.0, 256.0, 1.0) / 65793.0);\n"
              "  f = (pow(2, f * log2(101.0)) - 1.0) / 100.0;\n  float depth = -packedAbs >= 0 ? a.r : f;\n  float bias = lerp(1.0, 0.01, depth);\n"
              "  float gain = power / max(bias, %.6f);\n  for (int i = -%d; i <= %d; i++) colour += gain * weights[i + %d] * tex2D(src, uv + float2(%s));\n"
              "  return colour;\n}\n",
              c.sampler, c.bloomSampler, n, n, n + 1, c.packed, floor, half, half, half, b.vertical ? "0.0, i * step * bias" : "i * step * bias, 0.0");
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "tscfix_glow_gain", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        Log("postfx: glow gain: the blur shader did not compile (0x%08lx): %.300s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g.shader);
    code->Release();
    Log("postfx: glow gain: blur shader (%d weights, %s) created 0x%08lx; floor %.4f, far gain at most %.0f%% of the game's", n, b.vertical ? "vertical" : "horizontal", hr,
        floor, g_opt.glowFarGain * 100.0f);
    if (FAILED(hr)) g.shader = nullptr;
    return g.shader;
}

void ReleaseGain() {
    for (auto& g : g_gainShader) {
        if (g.shader) g.shader->Release();
        g = {};
    }
}

bool SteadyWanted() { return g_opt.steadyGlow && g_glowActive && g_pictureActive && !g_opt.compare; }

// The text of the two shaders. mask() does the arithmetic of the instructions of the game's PSMask:
// the register that the preshader fills holds abs(PackedDepth), and zero selects the green channel.
IDirect3DPixelShader9* SteadyGlowShader(IDirect3DDevice9* device, const Classed& c, bool composite, int stage, UINT width, UINT height) {
    GlowShader& g = g_glowShader[composite ? 1 : 0];
    uint32_t registers = c.sampler | (c.bloomSampler << 8) | (c.packed << 16) | (static_cast<uint32_t>(stage) << 24);
    uint32_t size = composite ? 0 : (width | (height << 16));
    if (g.tried && g.registers == registers && g.size == size) return g.shader;
    if (g.shader) g.shader->Release(), g.shader = nullptr;
    g.tried = true, g.registers = registers, g.size = size;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    char source[1600];
    int n = sprintf_s(source,
                      "sampler2D colour : register(s%d);\nsampler2D aux : register(s%d);\nfloat packedAbs : register(c%d);\nsampler2D held : register(s%d);\n"
                      "float4 mask(float2 uv) {\n  float3 c = float3(0, 0, 0);\n  float4 a = tex2D(aux, uv);\n  float g = -packedAbs >= 0 ? a.g : a.a;\n"
                      "  if (g > 0) c = tex2D(colour, uv).rgb;\n  return float4(c * g, g);\n}\n",
                      c.sampler, c.bloomSampler, c.packed, stage);
    if (composite)
        sprintf_s(source + n, sizeof source - n, "float4 main(float2 uv : TEXCOORD0) : COLOR { return max(mask(uv), tex2D(held, uv)) * 0.5; }\n");
    else  // (the kept mask is read at the centre of the pixel that is drawn, as the kept picture of the blend over time)
        sprintf_s(source + n, sizeof source - n,
                  "struct Out { float4 shown : COLOR0; float4 kept : COLOR1; };\nOut main(float2 uv : TEXCOORD0, float2 vpos : VPOS) {\n"
                  "  float4 m = mask(uv);\n  float4 h = tex2D(held, (vpos + 0.5) * float2(%.9f, %.9f));\n  Out o;\n"
                  "  o.shown = max(m, h);\n  o.kept = max(m, h * %.4f - 0.006);\n  return o;\n}\n",
                  1.0 / width, 1.0 / height, g_opt.steadyGlowKeep);
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "tscfix_glow", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        Log("postfx: steady glow: the %s shader did not compile (0x%08lx): %.300s", composite ? "composite" : "mask", hr,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g.shader);
    code->Release();
    Log("postfx: steady glow: %s shader created 0x%08lx (colour at sampler %d, material at sampler %d, abs(PackedDepth) at c%d, kept mask at sampler %d)",
        composite ? "composite" : "mask", hr, c.sampler, c.bloomSampler, c.packed, stage);
    if (FAILED(hr)) g.shader = nullptr;
    return g.shader;
}

void ReleaseGlow() {
    for (int i = 0; i < 2; ++i) {
        if (g_glowSurf[i]) g_glowSurf[i]->Release(), g_glowSurf[i] = nullptr;
        if (g_glowTex[i]) g_glowTex[i]->Release(), g_glowTex[i] = nullptr;
        if (g_glowShader[i].shader) g_glowShader[i].shader->Release();
        g_glowShader[i] = {};
    }
    DropStage(g_glowStage);
    g_glowWidth = g_glowHeight = 0;
    g_glowFailed = false;
    g_glowFrame = 0;
    g_glowBound = 0;
    g_glowTargetSet = false;
    g_glowFor = g_glowGiven = nullptr;
}

// Two pictures of the size and format of the game's mask target.
bool EnsureGlowPictures(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
    if (g_glowTex[0] && desc.Width == g_glowWidth && desc.Height == g_glowHeight) return true;
    if (g_glowFailed && desc.Width == g_glowWidth && desc.Height == g_glowHeight) return false;
    for (int i = 0; i < 2; ++i) {
        if (g_glowSurf[i]) g_glowSurf[i]->Release(), g_glowSurf[i] = nullptr;
        if (g_glowTex[i]) g_glowTex[i]->Release(), g_glowTex[i] = nullptr;
    }
    g_glowFrame = 0;
    g_glowWidth = desc.Width, g_glowHeight = desc.Height;
    bool made = MakePicture(device, desc, &g_glowTex[0], &g_glowSurf[0]) && MakePicture(device, desc, &g_glowTex[1], &g_glowSurf[1]);
    if (!made && g_glowSurf[0]) {
        g_glowSurf[0]->Release(), g_glowSurf[0] = nullptr;
        g_glowTex[0]->Release(), g_glowTex[0] = nullptr;
    }
    g_glowFailed = !made;
    Log("postfx: steady glow: the game's mask target is %ux%u (format %d); two kept masks %s", desc.Width, desc.Height, desc.Format,
        made ? "made" : "not made: the glow stays as the game has it");
    return made;
}

// A glow shader of the game (the mask pass or the last pass of HIGH) is set. Returns the shader to
// set: the game's own, or the mod's.
IDirect3DPixelShader9* GlowEnter(IDirect3DDevice9* device, IDirect3DPixelShader9* game, const Classed& c) {
    int stage = -1;
    for (int s = 15; s >= 0 && stage < 0; --s)
        if (!(c.samplers & (1u << s)) && s != c.sampler && s != c.bloomSampler) stage = s;
    if (stage < 0) return game;
    if (c.glowPass == 2) {
        // The mask pass of this frame wrote g_glowTex[g_glowWrite]; the other one is of the frame before.
        if (g_opt.steadyGlow == 2 || g_glowFrame != g_frame) return game;
        IDirect3DPixelShader9* made = SteadyGlowShader(device, c, true, stage, 0, 0);
        if (!made || !BindStage(device, g_glowStage, stage, g_glowTex[1 - g_glowWrite], D3DTEXF_LINEAR, "steady glow")) return game;
        g_glowBound = 2;
        g_glowFor = game;
        ++g_glowStats.composites;
        return g_glowGiven = made;
    }
    ++g_glowStats.masks;
    ++g_glowStats.notReplaced;  // (taken back below)
    if (g_glowFrame == g_frame) return game;  // a second mask pass in one frame: as the game has it
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return game;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    target->Release();
    // The mask target is a texture of 8 bits per channel. Another target here means that the game set
    // the shader before the target, or that this is not the mask pass: then nothing is changed.
    if (desc.Format != D3DFMT_A8R8G8B8 || desc.MultiSampleType != D3DMULTISAMPLE_NONE || !(desc.Usage & D3DUSAGE_RENDERTARGET) || desc.Width > 65535 ||
        desc.Height > 65535) {
        static int told = 0;
        if (told++ < 3) Log("postfx: steady glow: the target of the mask pass is %ux%u, format %d: not the expected one, no change", desc.Width, desc.Height, desc.Format);
        return game;
    }
    IDirect3DSurface9* second = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(1, &second)) && second) {  // the game has a second target of its own here
        second->Release();
        static int told = 0;
        if (told++ < 3) Log("postfx: steady glow: a second render target is set in the mask pass, no change");
        return game;
    }
    if (!EnsureGlowPictures(device, desc)) return game;
    IDirect3DPixelShader9* made = SteadyGlowShader(device, c, false, stage, desc.Width, desc.Height);
    if (!made) return game;
    bool fresh = g_glowFrame != 0 && g_glowFrame + 1 == g_frame;
    int read = g_glowWrite, write = 1 - g_glowWrite;
    // No kept mask of the frame before (first frame, after a reset, after a menu, after an A/B window):
    // an empty one. The result is then the game's own mask.
    if (!fresh) device->ColorFill(g_glowSurf[read], nullptr, 0);
    if (!BindStage(device, g_glowStage, stage, g_glowTex[read], D3DTEXF_POINT, "steady glow")) return game;
    if (FAILED(device->SetRenderTarget(1, g_glowSurf[write]))) {
        UnbindStage(device, g_glowStage);
        return game;
    }
    g_glowTargetSet = true;
    g_glowWrite = write;
    g_glowFrame = g_frame;
    g_glowBound = 1;
    g_glowFor = game;
    --g_glowStats.notReplaced;
    if (fresh) ++g_glowStats.held;
    static bool told = false;
    if (!told) told = true, Log("postfx: steady glow is active (mask target %ux%u, %.0f%% of the kept mask stays per frame)", desc.Width, desc.Height, g_opt.steadyGlowKeep * 100.0f);
    return g_glowGiven = made;
}

// The pass ends (the next shader is set, or the frame is presented).
void GlowLeave(IDirect3DDevice9* device) {
    g_glowBound = 0;
    g_glowFor = g_glowGiven = nullptr;
    UnbindStage(device, g_glowStage);
    if (!g_glowTargetSet) return;
    g_glowTargetSet = false;
    IDirect3DSurface9* now = nullptr;
    bool ours = SUCCEEDED(device->GetRenderTarget(1, &now)) && now == g_glowSurf[g_glowWrite];
    if (now) now->Release();
    if (ours) device->SetRenderTarget(1, nullptr);  // (when the game put another target there, it stays)
}

void GlowOnPresent(IDirect3DDevice9* device) {
    if (g_glowBound) GlowLeave(device);
    if (!g_opt.glowLog) return;
    ULONGLONG now = GetTickCount64();
    if (!g_glowStatsSince) g_glowStatsSince = now;
    if (now - g_glowStatsSince < 30000) return;
    const GlowStats& s = g_glowStats;
    Log("glow: %.0f s: %u mask passes, %u with the kept mask of the frame before, %u left as the game has them; %u last passes of HIGH with the kept mask; steady glow %s",
        static_cast<double>(now - g_glowStatsSince) / 1000.0, s.masks, s.held, s.notReplaced, s.composites, SteadyWanted() ? "on" : "off");
    g_glowStats = {};
    g_glowStatsSince = now;
}

// At Present: the frame ends.
void BlendOnPresent(IDirect3DDevice9* device) {
    if (g_filmBound) FilmLeave(device);  // (no shader was set after the last pass)
    if (g_sunBound) SunLeave();
    if (g_cameraWanted) JudgeCamera();
    if (g_opt.blendLog) {
        ULONGLONG now = GetTickCount64();
        if (!g_statsSince) g_statsSince = now;
        if (now - g_statsSince >= 30000) {
            double seconds = static_cast<double>(now - g_statsSince) / 1000.0;
            const BlendStats& s = g_stats;
            char why[600] = "";
            for (int i = 0, n = 0; i < kNoBlendReasons; ++i)
                if (s.notBlended[i]) n += sprintf_s(why + n, sizeof why - n, "%s%u %s", n ? ", " : "", s.notBlended[i], kNoBlendText[i]);
            if (g_camLast.valid)
                Log("taa: camera at %.2f %.2f %.2f, %s in the last frame; %.0f s: %u frames, %u standing, %u moved, %u without camera, path %.2f m (%.1f km/h), %u jumps; "
                    "largest change in a standing frame %.5f m, turn %.6f; sun pass %d and last pass %d per frame, %u sun passes with another camera, %u with old matrices; "
                    "%u frames blended; camera rest: %s (%u updates in a row at rest), %u frames at rest, %u passed through, %u without signal, %u at rest but the matrices moved; "
                    "at rest and not blended: %s",
                    g_camLast.viewInverse[3], g_camLast.viewInverse[7], g_camLast.viewInverse[11], g_still ? "it did not move" : "it moved", seconds, s.frames, s.still, s.moved,
                    s.noCamera, s.path, s.path / seconds * 3.6, s.jumps, s.stillMove, s.stillTurn, g_sunPasses, g_filmPasses, s.differ, s.notSet, s.blended,
                    kRestGateText[g_restGate], g_restRow, s.restStands, s.restShort, s.restNone, s.restVeto, why[0] ? why : "none");
            else
                Log("taa: no camera in the last frame (the sun light shader was not used, or its matrices were not set); %.0f s: %u frames, %u standing, %u moved, "
                    "%u without camera; sun pass %d and last pass %d per frame, %u sun passes with old matrices; %u frames blended; "
                    "camera rest: %s (%u updates in a row at rest), %u frames at rest, %u passed through, %u without signal, %u at rest but the matrices moved; at rest and not blended: %s",
                    seconds, s.frames, s.still, s.moved, s.noCamera, g_sunPasses, g_filmPasses, s.notSet, s.blended,
                    kRestGateText[g_restGate], g_restRow, s.restStands, s.restShort, s.restNone, s.restVeto, why[0] ? why : "none");
            g_stats = {};
            g_statsSince = now;
        }
    }
    g_camSeen.valid = false;
    g_judged = false;
    g_sunPasses = g_filmPasses = 0;
    ++g_frame;
}

}  // namespace

void Configure(const Options& options) {
    g_opt = options;
    if (options.noLampLight) g_lampForced = 2, Log("postfx: (diagnosis) no lamp light (%s)", options.noLampLight == 1 ? "all" : options.noLampLight == 2 ? "point and spot lights" : "glow pass");
    if (g_opt.blendWeight < 0.0f || g_opt.blendWeight > 0.98f) g_opt.blendWeight = 0.75f;
    if (g_opt.steadyGlowKeep < 0.0f || g_opt.steadyGlowKeep > 0.95f) g_opt.steadyGlowKeep = 0.6f;
    if (!(g_opt.glowFarGain >= 0.01f) || g_opt.glowFarGain > 1.0f) g_opt.glowFarGain = 1.0f;
    if (g_opt.glowFarGain < 1.0f)
        Log("postfx: glow gain: the gain of the glow for far lamps is at most %.0f%% of the game's (floor %.4f)", g_opt.glowFarGain * 100.0f, 0.01f / g_opt.glowFarGain);
    if (options.steadyGlow || options.glowLog)
        Log("postfx: steady glow %s (%.0f%% of the kept mask stays per frame%s)%s", options.steadyGlow ? "on" : "off", g_opt.steadyGlowKeep * 100.0f,
            options.steadyGlow == 2 ? ", mask pass only" : "", options.glowLog ? ", log line per 30 s" : "");
    g_cameraWanted = options.blendOverTime || options.blendLog;
    if (options.blendOverTime || options.blendLog || options.showHistory)
        Log("postfx: blend over time %s (frame before at %.0f%%, only while the camera rest holds the cab camera for %u updates in a row; "
            "no blend when the sun pass matrices change more than %.2f mm or %.6f per frame)%s%s%s", options.blendOverTime ? "on" : "off",
            g_opt.blendWeight * 100.0f, kRestRow, g_opt.stillMove * 1000.0f, g_opt.stillTurn, options.blendLog ? ", log line per 30 s" : "",
            options.showHistory ? ", (diagnosis) the screen shows the kept picture" : "", options.film > 0 ? "" : "; it needs [graphics] film=1 or 2: no blend");
}
bool CameraWanted() { return g_cameraWanted; }
bool DrawWanted() { return g_opt.blendOverTime || g_opt.showHistory; }
bool FilmDrawPending() { return g_filmPending; }
void OnDrawSlow(IDirect3DDevice9* device) { FilmDraw(device); }
void OnPixelConstants(UINT start, const float* data, UINT count) {
    if (!g_cameraWanted || !data || start >= kPsRegisters) return;
    if (count > kPsRegisters - start) count = kPsRegisters - start;
    memcpy(g_psConst[start], data, static_cast<size_t>(count) * 16);
    for (UINT r = start; r < start + count; ++r) g_psFrame[r] = g_frame;
}
void SetBlendActive(bool on) { g_blendActive = on; }
void SetSteadyGlowActive(bool on) { g_glowActive = on; }
void SetGlowGainActive(bool on) { g_gainActive = on; }
void SetActive(bool on) {
    if (g_opt.compare) return;
    g_pictureActive = on;
    g_fxaaOff = g_dofOff = g_filmForced = g_lampForced = on ? -1 : 0;
    resolve::ForceVariant(on ? -1 : 0);
}
bool Enabled() {
    return g_opt.noLampLight || g_opt.fxaa != 1 || !g_opt.depthOfField || g_opt.compare || g_opt.film > 0 || g_opt.lampFade || g_opt.blendOverTime || g_opt.blendLog ||
           g_opt.showHistory || g_opt.steadyGlow || g_opt.glowLog || g_opt.glowFarGain < 1.0f;
}
DWORD BlendValue(D3DRENDERSTATETYPE state, DWORD value) {
    if (state != D3DRS_SRCBLEND || value != D3DBLEND_ONE || !g_lampBound || !LampFade()) return value;
    g_lampBlendChanged = true;  // (the factor itself was set when the lamp shader was set)
    return D3DBLEND_BLENDFACTOR;
}
// Supersampling already smooths the edges; FXAA after it only blurs (measured at Haarlem: the far
// track is 40% sharper without it, the catenary 22%).
bool FxaaWanted() { return g_opt.fxaa < 0 ? !resolve::ShrinkInUse() : g_opt.fxaa == 1; }

IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    if (g_filmBound) {  // (blend over time) the last pass was set
        if (shader && shader == g_filmFor) return g_filmGiven;  // the game sets it once more: nothing changes
        FilmLeave(device);
    }
    if (g_glowBound) {  // (steady glow) the mod's mask or composite shader was set
        if (shader && shader == g_glowFor) return g_glowGiven;
        GlowLeave(device);
    }
    if (g_sunBound) SunLeave();  // (blend over time) the sun pass ends: its constants are the camera of this frame
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
        else if (LampFade()) device->SetRenderState(D3DRS_BLENDFACTOR, FactorColour());  // (lamp after lamp: the factor may be old)
        if (c.blur && GainWanted()) {  // (glow gain) a blur pass of the glow: the mod's shader with the limited gain
            IDirect3DPixelShader9* made = GainShaderFor(device, c);
            if (made) {
                if (InterlockedIncrement(&g_gainSets) == 1) Log("postfx: glow gain is active (the mod's blur shader is set in place of the game's)");
                return made;
            }
        }
        if (c.glowPass && SteadyWanted()) return GlowEnter(device, shader, c);
        return shader;
    }
    if (g_lampBound) LampLeave(device);
    if (c.kind == kOther) return shader;
    if (c.kind == kSun) {
        g_sunBound = true;
        g_sunInvProj = c.invProj, g_sunViewInverse = c.viewInverse;
        return shader;
    }
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
        if (made && HistoryWanted()) return FilmEnter(device, shader, c, film - 1, made);
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
    GlowOnPresent(device);  // (before the frame count goes up)
    BlendOnPresent(device);
    static unsigned frames = 0;
    if (++frames % 30 == 0 && (g_opt.lampFade || g_opt.compare)) {
        ReadSceneBrightness(device);
        if (frames % 1800 == 0 && g_sceneBrightness >= 0)
            Log("postfx: scene brightness %.2f (average %.2f), lamp light at %.0f%%", g_sceneBrightness, g_lampBrightness, LampFactor() * 100.0f);
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
    for (FilmMade& m : g_film) {
        if (m.shader) m.shader->Release();
        if (m.blend) m.blend->Release();
        m = {};
    }
    g_blendStage = -1;
    for (auto& s : g_bright)
        if (s) s->Release(), s = nullptr;
    // (blend over time) The kept picture is lost with the reset: the first frames after it are not
    // blended (g_historyFrame = 0), and the camera of the frame before is not known.
    ReleaseHistory();
    ReleaseGain();  // (glow gain) the blur shaders are made again at the next use
    ReleaseGlow();  // (steady glow) the kept masks and the two shaders: the first frame after the reset has the game's own mask
    g_camSeen.valid = g_camLast.valid = false;
    g_sunBound = false;
    if (g_lumTarget) g_lumTarget->Release(), g_lumTarget = nullptr;
    if (g_lumCopy) g_lumCopy->Release(), g_lumCopy = nullptr;
    g_lampBound = g_lampBlendChanged = false;
}

}  // namespace tscfix::postfx
