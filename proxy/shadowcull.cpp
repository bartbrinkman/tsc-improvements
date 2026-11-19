#include "shadowcull.h"

#include <MinHook.h>

#include <cmath>
#include <cstdint>

#include "log.h"
#include "sites.h"

namespace tscfix::shadowcull {
namespace {

// Shadow caster test (game version v79.0a): (manager, const float* center, float radius) -> bool,
// false = the sphere does not need to be drawn into the current cascade.
constexpr sites::Fingerprint kCasterTest = {0xe231e2a7acf023ebull, 16};
using CasterTestFn = bool (*)(void* manager, const float* center, float radius);
CasterTestFn g_test = nullptr;

Config g_cfg;
volatile bool g_active = false;
constexpr int kMaxCascades = 8;

struct Stat { uint64_t tests, engineCulled, sizeCulled; float texel; };
Stat g_stat[kMaxCascades];
unsigned g_frames = 0;

// Texel size in world units: the current cascade's projection maps x to clip space with
// scale |column 0| = 2 / width, and the cascade spans `resolution` texels.
float TexelSize(void* manager) {
    const float* m = reinterpret_cast<const float*>(static_cast<uint8_t*>(manager) + g_cfg.matrixField);
    float n = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    return n > 0 ? 2.0f / (n * g_cfg.resolution) : 0.0f;
}

bool CasterTest(void* manager, const float* center, float radius) {
    bool draw = g_test(manager, center, radius);
    int cascade = *reinterpret_cast<const int*>(static_cast<uint8_t*>(manager) + g_cfg.cascadeField);
    if (cascade < 0 || cascade >= g_cfg.cascadeCount || cascade >= kMaxCascades) return draw;
    Stat& s = g_stat[cascade];
    ++s.tests;
    if (!draw) {
        ++s.engineCulled;
        return false;
    }
    if (cascade < g_cfg.minCascade) return true;
    float texel = TexelSize(manager);
    s.texel = texel;
    if (2.0f * radius < g_cfg.texels * texel) {
        ++s.sizeCulled;  // counted in probe mode too: what activation would remove
        if (g_active) return false;
    }
    return true;
}

}  // namespace

bool Install(HMODULE engine, const Config& config) {
    g_cfg = config;
    if (g_cfg.cascadeField <= 0 || g_cfg.matrixField <= 0 || g_cfg.cascadeCount < 1) {
        Log("shadowcull: local offsets not configured; disabled");
        return false;
    }
    void* target = sites::Find(engine, kCasterTest);
    if (!target) {
        Log("shadowcull: caster test not found; disabled");
        return false;
    }
    MH_Initialize();
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&CasterTest), reinterpret_cast<void**>(&g_test)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    Log("shadowcull: %s (diameter < %.2f texels from cascade %d, %d texels per cascade)", ok ? "installed" : "hook failed",
        g_cfg.texels, g_cfg.minCascade, g_cfg.resolution);
    return ok;
}

void SetActive(bool active) { g_active = active; }

void OnFrame() {
    if (!g_test || ++g_frames % 300 != 0) return;
    for (int c = 0; c < g_cfg.cascadeCount && c < kMaxCascades; ++c) {
        Stat& s = g_stat[c];
        Log("shadowcull: cascade %d: tests/frame %.0f, engine culled %.1f%%, size culled %.1f%% (texel %.3f m)%s", c,
            s.tests / 300.0, s.tests ? 100.0 * s.engineCulled / s.tests : 0.0,
            s.tests ? 100.0 * s.sizeCulled / s.tests : 0.0, s.texel, g_active ? "" : " [probe]");
        s = {};
    }
}

}  // namespace tscfix::shadowcull
