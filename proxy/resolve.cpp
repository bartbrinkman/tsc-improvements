#include "resolve.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

// The game's shrink step (technique IntermediatetoBBSS of IntermediateToBB.fx, read from the
// game's own shader source): per screen pixel 8 point samples of the large picture, at fixed
// places inside the pixel, equal weights. At 3 x 3 a pixel covers 9 rendered points: some are read
// twice, some not at all, none from a neighbour pixel.
//
// The replacement is written here (HLSL text, compiled once per factor with the system's
// d3dcompiler_47.dll). Per screen pixel it reads every rendered point within the kernel radius at
// its centre and weights it by the kernel. The weights are constants in the shader text: for a
// whole-number factor the points sit at the same places in every pixel.
//
// The shader is recognised when the game creates it: shader model 3, one named constant
// ("IntermediateSampler"), a loop, and constant registers c0 to c16 in use.
namespace tscfix::resolve {
namespace {

constexpr int kRegister = 210;  // (1/source width, 1/source height, 0, 0); the game uses c0 to c19 here
constexpr int kKernels = 3;

Options g_opt;
volatile bool g_active = true;
IDirect3DPixelShader9* volatile g_originals[8] = {};  // the game makes the shader once per effect that has it
volatile LONG g_originalCount = 0, g_seenSets = 0, g_setCalls = 0;
volatile LONG g_rtWidth = 0, g_rtHeight = 0;       // the largest render-target texture seen
IDirect3DPixelShader9* g_made[kKernels + 1][4] = {};  // [kernel][factor]
bool g_failed[kKernels + 1][4] = {};
volatile LONG g_substituted = 0;
int g_compareKernel = -1;  // compare mode: the kernel of the frame that is drawn now (-1: not in a compare)
volatile int g_forced = -1;  // set by the picture compare of postfx.cpp

double Sinc(double x) {
    if (fabs(x) < 1e-9) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}
// The kernel weight at distance d (in screen pixels).
double Weight(int kernel, double d) {
    d = fabs(d);
    if (kernel == 1) return d < 2.0 ? Sinc(d) * Sinc(d / 2.0) : 0.0;
    if (kernel == 2) {  // Mitchell-Netravali, B = C = 1/3
        const double B = 1.0 / 3.0, C = 1.0 / 3.0;
        if (d < 1.0) return ((12 - 9 * B - 6 * C) * d * d * d + (-18 + 12 * B + 6 * C) * d * d + (6 - 2 * B)) / 6.0;
        if (d < 2.0) return ((-B - 6 * C) * d * d * d + (6 * B + 30 * C) * d * d + (-12 * B - 48 * C) * d + (8 * B + 24 * C)) / 6.0;
        return 0.0;
    }
    return d < 0.5 ? 1.0 : 0.0;  // the pixel's own points
}

std::string Source(int kernel, int factor) {
    // Points along one axis: offset o (in rendered points, from the pixel's first point) at
    // distance d = (o + 0.5 - factor / 2) / factor from the pixel centre.
    double radius = kernel == 3 ? 0.5 : 2.0;
    std::vector<int> offsets;
    std::vector<double> weights;
    double sum = 0;
    for (int o = -factor * 3; o < factor * 4; ++o) {
        double d = (o + 0.5 - factor / 2.0) / factor;
        if (fabs(d) >= radius) continue;
        double w = Weight(kernel, d);
        offsets.push_back(o), weights.push_back(w), sum += w;
    }
    int n = static_cast<int>(offsets.size());
    char text[256];
    std::string s = "sampler2D src : register(s0);\nfloat4 P : register(c210);\n";
    sprintf_s(text, "static const float W[%d] = {", n);
    s += text;
    for (int i = 0; i < n; ++i) {
        sprintf_s(text, "%s%.9g", i ? ", " : "", weights[i] / sum);
        s += text;
    }
    s += "};\nstruct Out { float4 c : COLOR0; float d : DEPTH; };\nOut main(float2 vpos : VPOS) {\n";
    sprintf_s(text, "  float2 first = (floor(vpos) * %d + (%d + 0.5)) * P.xy;\n", factor, offsets[0]);
    s += text;
    s += "  float2 lo = 0.5 * P.xy, hi = 1 - 0.5 * P.xy;\n  float3 sum = 0;\n";
    sprintf_s(text, "  [unroll] for (int j = 0; j < %d; j++) {\n    float3 row = 0;\n    [unroll] for (int i = 0; i < %d; i++) {\n", n, n);
    s += text;
    s += "      float2 uv = clamp(first + float2(i, j) * P.xy, lo, hi);\n";
    s += "      float3 c = tex2Dlod(src, float4(uv, 0, 0)).rgb;\n";
    s += g_opt.linear ? "      row += c * c * W[i];\n" : "      row += c * W[i];\n";
    s += "    }\n    sum += row * W[j];\n  }\n  Out o;\n";
    s += g_opt.linear ? "  o.c = float4(sqrt(saturate(sum)), 1);\n" : "  o.c = float4(saturate(sum), 1);\n";
    s += "  o.d = 1;\n  return o;\n}\n";
    return s;
}

struct Blob : IUnknown {
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
using CompileFn = HRESULT(WINAPI*)(const void* src, SIZE_T size, const char* name, const void* defines, void* include, const char* entry,
                                   const char* target, UINT flags1, UINT flags2, Blob** code, Blob** errors);

IDirect3DPixelShader9* Make(IDirect3DDevice9* device, int kernel, int factor) {
    if (g_made[kernel][factor] || g_failed[kernel][factor]) return g_made[kernel][factor];
    g_failed[kernel][factor] = true;  // (until it is made)
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        Log("resolve: no shader compiler on this system (d3dcompiler_47.dll); the game's own shrink step stays");
        return nullptr;
    }
    std::string source = Source(kernel, factor);
    Blob* code = nullptr;
    Blob* errors = nullptr;
    HRESULT hr = compile(source.data(), source.size(), "tscfix_resolve", nullptr, nullptr, "main", "ps_3_0", 1u << 15 /* O3 */, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        Log("resolve: the new shader did not compile (0x%08lx): %.300s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    IDirect3DPixelShader9* shader = nullptr;
    // (the device hook sees this creation too: it is not the game's shrink shader, so it passes)
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    Log("resolve: kernel %d for factor %d: %zu bytes of shader code, created 0x%08lx", kernel, factor, static_cast<size_t>(code->GetBufferSize()), hr);
    code->Release();
    if (FAILED(hr)) return nullptr;
    g_failed[kernel][factor] = false;
    return g_made[kernel][factor] = shader;
}

}  // namespace

void Configure(const Options& options) { g_opt = options; }
bool Enabled() { return g_opt.kernel > 0 || g_opt.compare || g_opt.anisotropy > 0 || g_opt.hooksWanted; }

// [graphics] anisotropy: the game asks for anisotropic texture filtering of at most 4 (measured:
// every MAXANISOTROPY call has the value 4 or 1). A higher value here replaces every value above 1.
volatile bool g_compareAniso = false;  // compare mode: the frame that is drawn now uses 16
DWORD Anisotropy(DWORD value) {
    if (value <= 1) return value;
    DWORD want = g_compareAniso ? 16 : (g_compareKernel >= 0 ? 0 : static_cast<DWORD>(g_opt.anisotropy));
    return want > value ? want : value;
}
void SetActive(bool on) { g_active = on; }
void ForceVariant(int kernel) { g_forced = kernel; }
bool ShrinkInUse() { return g_seenSets > 0; }

bool IsShrinkShader(const DWORD* code) {
    if (!code || code[0] != 0xFFFF0300) return false;
    bool named = false, loop = false;
    uint32_t constants = 0;
    for (const DWORD* p = code + 1; *p != 0x0000FFFF;) {
        DWORD t = *p;
        if ((t & 0xFFFF) == 0xFFFE) {  // comment block: the constant table is one
            DWORD words = (t >> 16) & 0x7FFF;
            const auto* bytes = reinterpret_cast<const uint8_t*>(p + 1);
            if (words >= 8 && memcmp(bytes, "CTAB", 4) == 0) {
                const uint8_t* base = bytes + 4;
                uint32_t count, info;
                memcpy(&count, base + 12, 4);
                memcpy(&info, base + 16, 4);
                if (count == 1 && info + 20 <= words * 4) {
                    uint32_t name;
                    memcpy(&name, base + info, 4);
                    if (name + 20 <= words * 4) named = strncmp(reinterpret_cast<const char*>(base + name), "IntermediateSampler", 20) == 0;
                }
            }
            p += 1 + words;
            continue;
        }
        DWORD op = t & 0xFFFF, n = (t >> 24) & 0xF;
        if (op == 38 || op == 27) loop = true;  // rep, loop
        if (op != 31 && op != 81 && op != 48)   // (not dcl, def, defi)
            for (DWORD i = 1; i <= n; ++i) {
                DWORD x = p[i];
                DWORD type = ((x >> 28) & 7) | ((x >> 8) & 0x18);
                if (type == 2 && (x & 0x7FF) < 32) constants |= 1u << (x & 0x7FF);
            }
        p += 1 + n;
    }
    return named && loop && (constants & 0x1FFFF) == 0x1FFFF;
}

void NoteShrinkShader(IDirect3DPixelShader9* shader) {
    if (!shader) return;
    LONG n = InterlockedIncrement(&g_originalCount);
    if (n > 8) return;
    shader->AddRef();  // (kept: its address is the key)
    g_originals[n - 1] = shader;
    Log("resolve: the game's shrink shader is known (%ld)", n);
}

void NoteRenderTarget(UINT width, UINT height) {
    if (static_cast<LONG>(width) > g_rtWidth) g_rtWidth = static_cast<LONG>(width), g_rtHeight = static_cast<LONG>(height);
}

// Pixel shaders by address: 0 not seen, 1 other, 2 the shrink shader. The shader object that the
// game sets is not the one whose creation the hook sees, so a shader is classed by its code the
// first time it is set. A positive is checked again by its code size at each use (an address can
// be used again after a release). An "other" is checked by its code size once per 256 sets: no
// runtime call in the common case, and a shrink shader made later at the address of a released
// shader is still found.
// A shader has kClassProbe places in a row. With one place, two shaders in use on the same place
// put each other out at every set (a copy of the code and a parse each time).
constexpr size_t kClassSlots = 16384;  // power of two
constexpr size_t kClassProbe = 4;
struct Classed {
    IDirect3DPixelShader9* shader;
    uint8_t kind;
    uint32_t size;
    uint8_t sets;  // (kind 1) sets since the code size was last compared
};
Classed g_classed[kClassSlots];
unsigned g_classEvict = 0;  // which of the places is given up next
volatile bool g_shrinkBound = false;       // the shader that is set now is the shrink shader (original or replacement)
volatile bool g_replacementBound = false;
volatile LONG g_sourceWidth = 0, g_sourceHeight = 0;  // the large picture: the texture at stage 0 while the shrink shader is set

bool IsShrinkObject(IDirect3DPixelShader9* shader) {
    size_t at = (reinterpret_cast<uintptr_t>(shader) >> 4) * 0x9E3779B97F4A7C15ull >> 50;
    Classed* found = nullptr;
    Classed* empty = nullptr;
    for (size_t n = 0; n < kClassProbe && !found; ++n) {
        Classed& e = g_classed[(at + n) & (kClassSlots - 1)];
        if (e.shader == shader) found = &e;
        else if (!e.shader && !empty) empty = &e;
    }
    UINT size = 0;
    if (found) {
        if (found->kind == 1 && ++found->sets != 0) return false;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size == found->size) return found->kind == 2;
    }
    // Not seen, or another shader at a known address: classed by its code. A shader whose code
    // cannot be read is stored as "other" with size 0, so it does not cost a read at every set.
    Classed& c = found ? *found : empty ? *empty : g_classed[(at + (g_classEvict++ & (kClassProbe - 1))) & (kClassSlots - 1)];
    c = {shader, 1, 0, 0};
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return false;
    std::vector<DWORD> code(size / 4 + 2);
    if (FAILED(shader->GetFunction(code.data(), &size))) return false;
    bool shrink = IsShrinkShader(code.data());
    c = {shader, static_cast<uint8_t>(shrink ? 2 : 1), size, 0};
    if (shrink) Log("resolve: the game's shrink shader is in use (%u bytes)", size);
    return shrink;
}

void NoteSource(IDirect3DBaseTexture9* texture) {
    if (texture->GetType() != D3DRTYPE_TEXTURE) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc))) return;
    if (!(desc.Usage & D3DUSAGE_RENDERTARGET)) return;
    g_sourceWidth = static_cast<LONG>(desc.Width), g_sourceHeight = static_cast<LONG>(desc.Height);
}

void SetSourceConstant(IDirect3DDevice9* device) {
    LONG w = g_sourceWidth, h = g_sourceHeight;
    if (!w || !h) return;
    const float p[4] = {1.0f / w, 1.0f / h, 0, 0};
    device->SetPixelShaderConstantF(kRegister, p, 1);
}

// (measurement, compare mode) For a few frames: every pixel shader that is set while the render
// target has the size of the window picture, with the names of its constants. One line per shader.
volatile LONG g_probeFrames = 0;
void Probe(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    static IDirect3DPixelShader9* seen[48];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (seen[i] == shader) return;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    target->Release();
    static UINT backWidth = 0;
    if (!backWidth) {
        IDirect3DSurface9* back = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
            D3DSURFACE_DESC b{};
            back->GetDesc(&b);
            back->Release();
            backWidth = b.Width;
            Log("resolve probe: back buffer %ux%u, largest render-target texture seen %ldx%ld", b.Width, b.Height, g_rtWidth, g_rtHeight);
        }
    }
    if (desc.Width != backWidth) return;
    if (count >= 48) return;
    seen[count++] = shader;
    UINT size = 0;
    shader->GetFunction(nullptr, &size);
    std::vector<uint8_t> code(size + 4);
    shader->GetFunction(code.data(), &size);
    std::string names;
    for (size_t i = 0; i + 32 < size; i += 4)
        if (memcmp(&code[i], "CTAB", 4) == 0) {
            const uint8_t* base = &code[i + 4];
            uint32_t n, info;
            memcpy(&n, base + 12, 4);
            memcpy(&info, base + 16, 4);
            for (uint32_t k = 0; k < n && k < 12; ++k) {
                uint32_t name;
                uint16_t reg;
                memcpy(&name, base + info + 20 * k, 4);
                memcpy(&reg, base + info + 20 * k + 6, 2);
                char one[64];
                sprintf_s(one, " %.40s@%u", reinterpret_cast<const char*>(base + name), reg);
                names += one;
            }
            break;
        }
    bool known = false;
    for (IDirect3DPixelShader9* o : g_originals) known |= o == shader;
    Log("resolve probe: target %ux%u fmt %d, pixel shader %p, %u bytes%s, constants:%s", desc.Width, desc.Height, desc.Format,
        static_cast<void*>(shader), size, known ? " (the shrink shader)" : "", names.c_str());
}

IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    static volatile LONG calls = 0;
    g_setCalls = InterlockedIncrement(&calls);
    g_shrinkBound = g_replacementBound = false;
    if (!shader) return shader;
    if (g_probeFrames > 0) Probe(device, shader);
    if (!IsShrinkObject(shader)) return shader;
    g_shrinkBound = true;
    LONG seen = InterlockedIncrement(&g_seenSets);
    // The large picture can be bound already (stage 0), or it comes after this call (OnSetTexture).
    IDirect3DBaseTexture9* bound = nullptr;
    if (SUCCEEDED(device->GetTexture(0, &bound)) && bound) {
        NoteSource(bound);
        bound->Release();
    }
    int kernel = g_forced >= 0 ? g_forced : g_compareKernel >= 0 ? g_compareKernel : (g_active ? g_opt.kernel : 0);
    if (kernel <= 0 || kernel > kKernels) return shader;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return shader;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    target->Release();
    LONG w = g_sourceWidth, h = g_sourceHeight;
    int factor = 0;
    for (int k = 2; k <= 3; ++k)
        if (desc.Width * k == static_cast<UINT>(w) && desc.Height * k == static_cast<UINT>(h)) factor = k;
    if (!factor) {
        if (seen < 4) Log("resolve: no factor yet: target %ux%u, large picture %ldx%ld", desc.Width, desc.Height, w, h);
        return shader;
    }
    IDirect3DPixelShader9* made = Make(device, kernel, factor);
    if (!made) return shader;
    g_replacementBound = true;
    SetSourceConstant(device);
    if (InterlockedIncrement(&g_substituted) == 1)
        Log("resolve: shrink step replaced: %ldx%ld to %ux%u (factor %d), kernel %d, %s light", w, h, desc.Width, desc.Height, factor, kernel,
            g_opt.linear ? "linear" : "stored");
    return made;
}

void OnSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (!g_shrinkBound || stage != 0 || !texture) return;
    NoteSource(texture);
    if (g_replacementBound) SetSourceConstant(device);
}

// Compare mode: every 900 frames, four frames in a row are drawn with kernel 0 (the game's own), 1,
// 2 and 3, and each is saved as a picture in %TEMP%\tscfix-resolve.
void OnPresent(IDirect3DDevice9* device) {
    if (!g_opt.compare) return;
    static unsigned frame = 0, set = 0;
    static int saving = -1;  // the kernel of the frame that is about to be presented
    ++frame;
    if (frame == 400) g_probeFrames = 3;
    else if (g_probeFrames > 0) InterlockedDecrement(&g_probeFrames);
    if (saving >= 0) {
        using SaveFn = HRESULT(WINAPI*)(const char*, DWORD, IDirect3DSurface9*, const void*, const RECT*);
        static SaveFn save = nullptr;
        if (!save)
            if (HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll")) save = reinterpret_cast<SaveFn>(GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA"));
        IDirect3DSurface9* back = nullptr;
        if (save && SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
            char dir[MAX_PATH], path[MAX_PATH];
            GetTempPathA(MAX_PATH, dir);
            strcat_s(dir, "tscfix-resolve");
            CreateDirectoryA(dir, nullptr);
            static const char* names[] = {"0-game", "1-lanczos", "2-mitchell", "3-average", "4-aniso16"};
            sprintf_s(path, "%s\\set%02u-%s.png", dir, set, names[saving]);
            HRESULT hr = save(path, 3 /* PNG */, back, nullptr, nullptr);
            if (saving == 0)
                Log("resolve: compare set %u at frame %u (0x%08lx): %s; shader sets seen %ld, of the shrink shader %ld, replaced %ld", set, frame, hr,
                    path, g_setCalls, g_seenSets, g_substituted);
            back->Release();
        }
        saving = saving < kKernels + 1 ? saving + 1 : -1;
        if (saving < 0) ++set;
    } else if (frame % 900 == 600 && set < 12) {
        saving = 0;
    }
    g_compareKernel = saving >= 0 ? (saving <= kKernels ? saving : 0) : -1;  // (variant 4: the game's filter, anisotropy 16)
    g_compareAniso = saving == kKernels + 1;
}

void OnReset() {
    g_rtWidth = g_rtHeight = 0;
    for (auto& row : g_made)
        for (auto& s : row)
            if (s) s->Release(), s = nullptr;
    memset(g_failed, 0, sizeof g_failed);
}

}  // namespace tscfix::resolve
