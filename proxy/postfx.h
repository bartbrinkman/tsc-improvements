#pragma once
#include <windows.h>
#include <d3d9.h>

// Picture passes of the game that the mod can switch off or replace (see postfx.cpp):
// [graphics] fxaa=0 (no FXAA after the supersampling shrink), [graphics] depth_of_field=0.
// [experiments] picture_compare=1 saves the same scene with each variant in turn.
namespace tscfix::postfx {

struct Options {
    int fxaa = -1;             // the game's FXAA pass: 1 on, 0 off, -1 off when the game supersamples (default)
    bool depthOfField = true;  // the game's depth-of-field pass
    int film = 1;              // [graphics] film: 0 the game's bloom and tone pass, 1 the mod's (default), 2 the mod's strong one
    bool lampFade = true;      // [graphics] lamp_fade: lamp light gets weaker as the scene gets bright
    float lampFadeFrom = 0.25f, lampFadeTo = 0.70f;  // scene brightness where the fade starts and where it is complete
    bool compare = false;      // (measurement) save every variant of the same scene in turn
    int noLampLight = 0;       // (diagnosis) [experiments] no_lamp_light: 1 no lamp light, 2 no point/spot lights, 3 no glow pass
};
void Configure(const Options& options);
bool Enabled();
// At SetPixelShader, after resolve::Substitute.
IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
void OnPresent(IDirect3DDevice9* device);
void OnReset();
void SetActive(bool on);  // in-session A/B: off = the game's own picture passes
DWORD BlendValue(D3DRENDERSTATETYPE state, DWORD value);  // at SetRenderState

}  // namespace tscfix::postfx
