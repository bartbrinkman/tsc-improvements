#pragma once
#include <windows.h>

// Shadow-caster contribution culling. During each shadow cascade the engine asks its shadow
// map manager whether a bounding sphere should cast into that cascade. On top of the engine's
// own answer, this skips casters whose diameter is smaller than `texels` shadow-map texels of
// the cascade being drawn: they cannot change the shadow map in a visible way, but each one
// still costs a scene-graph walk and draw calls.
namespace tscfix::shadowcull {

struct Config {
    float texels = 1.0f;     // cull when diameter < texels * texel size of the cascade
    int minCascade = 1;      // never cull in cascades below this one (nearest = 0)
    int resolution = 1280;   // shadow map texels across one cascade (larger = more conservative)
    int cascadeField = 0;    // local: offset of the current cascade index in the manager
    int matrixField = 0;     // local: offset of the current cascade's projection in the manager
    int cascadeCount = 0;
};

bool Install(HMODULE engine, const Config& config);
void SetActive(bool active);  // probe-only while inactive
void OnFrame();               // game thread, once per frame

}  // namespace tscfix::shadowcull
