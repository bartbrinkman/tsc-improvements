#include "overlapstate.h"

#include <MinHook.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "sites.h"

// Engine data layouts come from a local research header (not published); without it the
// feature stays off.
#if __has_include("layouts.h")
#include "layouts.h"
#define TSCFIX_LAYOUTS 1
#endif

namespace tscfix::overlapstate {
namespace {

// Model draw: (model, world matrix, then seven values that go into the draw context). Hooked:
// Draw below replaces it on threads with a private palette.
constexpr sites::Fingerprint kModelDraw = {0x31f27391e07b6681ull, 32};
// Node palette: (model, world matrix); writes one 64-byte matrix per node into the palette
// (kPalette). Called by Draw on a copy of the model header.
constexpr sites::Fingerprint kNodePalette = {0x4f1b94c40c6699d6ull, 16};

thread_local bool t_private = false;  // another drawing thread (scenery ring worker): own palette

#ifdef TSCFIX_LAYOUTS
using namespace layout;

using DrawFn = void (*)(void* model, const float* world, const void* a3, void* a4, void* a5, void* a6, uint8_t a7,
                        uint32_t a8, int a9);
using PaletteFn = void (*)(void* model, const float* world);
using WalkFn = void (*)(void* model, DrawContext* context);
DrawFn g_draw = nullptr;
PaletteFn g_palette = nullptr;
// Nested model draws each need their own palette while the outer walk is still running.
constexpr int kDepth = 8;
struct Buffer {
    void* data;
    size_t size;
};
thread_local Buffer t_buffers[kDepth];
thread_local int t_depth = 0;

void* PaletteBuffer(uint32_t nodes) {
    Buffer& b = t_buffers[t_depth];
    size_t need = static_cast<size_t>(nodes) * 64;
    if (b.size < need) {
        size_t size = need < 4096 ? 4096 : need * 2;
        void* p = _aligned_malloc(size, 16);
        if (!p) return nullptr;
        _aligned_free(b.data);
        b.data = p;
        b.size = size;
    }
    return b.data;
}

void Draw(void* model, const float* world, const void* a3, void* a4, void* a5, void* a6, uint8_t a7, uint32_t a8,
          int a9) {
    if ((!t_shadow && !t_private) || !g_active || t_depth >= kDepth)
        return g_draw(model, world, a3, a4, a5, a6, a7, a8, a9);
    auto* m = static_cast<uint8_t*>(model);
    uint32_t nodes;
    memcpy(&nodes, m + kNodeCount, 4);
    void* palette = PaletteBuffer(nodes);
    if (!palette) return g_draw(model, world, a3, a4, a5, a6, a7, a8, a9);

    // The engine's own palette function, on a copy of the fields it reads with the palette
    // pointer swapped, so the matrices are bit-identical to the engine's.
    alignas(16) uint8_t header[kHeaderSize];
    memcpy(header, m, kHeaderSize);
    memcpy(header + kPalette, &palette, sizeof palette);
    g_palette(header, world);

    DrawContext c;
    memset(&c, 0, sizeof c);
    c.block = &c.blockWord;
    if (a3) memcpy(c.a3, a3, sizeof c.a3);
    c.a4 = a4;
    c.a5 = a5;
    c.a6 = a6;
    c.a7 = a7;
    c.a8 = a8;
    if (a9 > -1) {
        c.hasA9 = 1;
        c.a9 = static_cast<float>(a9);
    }
    c.blockFfff = 0xffff;
    c.nodeCount = nodes;
    c.palette = palette;
    memcpy(&c.paletteExtra, m + kPaletteExtra, sizeof c.paletteExtra);
    c.world = world;

    void** vt = *static_cast<void***>(model);
    ++t_depth;
    reinterpret_cast<WalkFn>(vt[kWalkSlot])(model, &c);
    --t_depth;
}
#endif  // TSCFIX_LAYOUTS

}  // namespace

bool Install(HMODULE engine) {
#ifndef TSCFIX_LAYOUTS
    (void)engine;
    Log("overlapstate: engine layouts not available in this build; disabled");
    return false;
#else
    void* draw = sites::Find(engine, kModelDraw);
    g_palette = static_cast<PaletteFn>(sites::Find(engine, kNodePalette));
    if (!draw || !g_palette) {
        Log("overlapstate: model draw not found (game version changed?); disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(draw, reinterpret_cast<void*>(&Draw), reinterpret_cast<void**>(&g_draw)) == MH_OK &&
              MH_EnableHook(draw) == MH_OK;
    Log("overlapstate: model palette %s", ok ? "per thread for the shadow pass" : "hook failed");
    return ok;
#endif
}

void SetActive(bool active) { g_active = active; }

void SetShadowRole(bool shadow) { t_shadow = shadow; }
void SetPrivateDraw(bool on) { t_private = on; }

volatile bool g_active = false;
thread_local bool t_shadow = false;

}  // namespace tscfix::overlapstate
