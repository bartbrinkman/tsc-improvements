#pragma once
// Engine data layouts used by overlapstate.cpp.
#include <cstddef>
#include <cstdint>

namespace tscfix::overlapstate::layout {

// Model fields read by Draw in overlapstate.cpp.
inline constexpr size_t kHeaderSize = 0x98;  // bytes copied before calling the palette function
inline constexpr size_t kNodeCount = 0x0c;   // uint32
inline constexpr size_t kPalette = 0x70;     // pointer, 64 bytes per node
inline constexpr size_t kPaletteExtra = 0x88;
inline constexpr int kWalkSlot = 2;

// Draw context that Draw builds and passes to the model's walk (vtable slot kWalkSlot).
struct DrawContext {
    void* block;  // -> palette block below
    uint8_t a3[12];       // copied from the third argument when it is not null
    uint32_t pad14;
    void* a4;
    void* a5;
    void* a6;
    uint64_t zero30;
    uint8_t a7;
    uint8_t pad39[3];
    uint32_t a8;
    uint8_t hasA9;
    uint8_t pad41[11];  // zero
    float a9;
    uint8_t pad50[16];  // zero
    // palette block (zeros, 0xffff at +4)
    uint16_t blockWord;
    uint8_t blockByte;
    uint8_t pad63;
    uint16_t blockFfff;
    uint16_t pad66;
    uint32_t nodeCount;
    uint32_t pad6c;
    void* palette;
    void* paletteExtra;
    const float* world;
};
static_assert(offsetof(DrawContext, a4) == 0x18 && offsetof(DrawContext, zero30) == 0x30 &&
                  offsetof(DrawContext, a7) == 0x38 && offsetof(DrawContext, a8) == 0x3c &&
                  offsetof(DrawContext, hasA9) == 0x40 && offsetof(DrawContext, a9) == 0x4c &&
                  offsetof(DrawContext, blockWord) == 0x60 &&
                  offsetof(DrawContext, nodeCount) == 0x68 && offsetof(DrawContext, palette) == 0x70 &&
                  offsetof(DrawContext, world) == 0x80 && sizeof(DrawContext) == 0x88,
              "draw context layout");

}  // namespace tscfix::overlapstate::layout
