#pragma once
#include <windows.h>
#include <d3d9.h>

// Picture passes of the game that the mod can switch off or replace (see postfx.cpp):
// [graphics] fxaa=0 (no FXAA after the supersampling shrink), [graphics] depth_of_field=0.
// [experiments] picture_compare=1 saves the same scene with each variant in turn.
// [graphics] blend_over_time=1 (off by default) calms far thin lines while the camera stands.
namespace tscfix::postfx {

struct Options {
    int fxaa = -1;             // the game's FXAA pass: 1 on, 0 off, -1 off when the game supersamples (default)
    bool depthOfField = true;  // the game's depth-of-field pass
    int film = 1;              // [graphics] film: 0 the game's bloom and tone pass, 1 the mod's (default), 2 the mod's strong one
    bool lampFade = true;      // [graphics] lamp_fade: lamp light gets weaker as the scene gets bright
    float lampFadeFrom = 0.25f, lampFadeTo = 0.70f;  // scene brightness where the fade starts and where it is complete
    bool compare = false;      // (measurement) save every variant of the same scene in turn
    int noLampLight = 0;       // (diagnosis) [experiments] no_lamp_light: 1 no lamp light, 2 no point/spot lights, 3 no glow pass
    // Blend over time (work-items: blend-over-time.md, stages 1 to 3). Needs the mod's last pass (film > 0).
    bool blendOverTime = false;  // [graphics] blend_over_time: while the camera stands, each frame also takes a part of the frame before
    float blendWeight = 0.75f;   // [experiments] taa_weight_percent: the part of the frame before
    float stillMove = 0.0005f;   // [experiments] taa_still_move_um: the camera stands when it moved at most this far in one frame (metres)
    float stillTurn = 0.0001f;   // [experiments] taa_still_turn_ppm: ... and no value of its turn and projection changed more than this
    bool blendLog = false;       // (measurement) [experiments] taa_log: one line per 30 s with the camera position and its movement
    bool showHistory = false;    // (diagnosis) [experiments] taa_show_history: show the kept picture (the frame before) in place of the frame
    // Steady glow: the glow around lamps does not alternate when the game's mask of lamp pixels does.
    int steadyGlow = 0;            // [graphics] steady_glow: 1 on, 2 on for the mask pass only (not the last pass of technique HIGH)
    float steadyGlowKeep = 0.6f;   // [experiments] steady_glow_keep_percent: the part of the kept mask that stays per frame (0: only the frame before counts)
    // Glow gain: the game's blur passes multiply the glow of far lamps by up to 175. See postfx.cpp.
    float glowFarGain = 1.0f;      // [graphics] glow_far_gain_percent / 100: the largest gain as a share of the game's (1: the game's shaders)
    bool glowLog = false;          // (measurement) [experiments] glow_log: one line per 30 s with the count of glow passes
};
void Configure(const Options& options);
bool Enabled();
bool CameraWanted();  // the camera of each frame is read: the hook for OnPixelConstants is needed
// At SetPixelShaderConstantF, in every hook of that slot.
void OnPixelConstants(UINT start, const float* data, UINT count);
void SetBlendActive(bool on);  // in-session A/B of blend_over_time
void SetSteadyGlowActive(bool on);  // in-session A/B of steady_glow
void SetGlowGainActive(bool on);    // in-session A/B "glow_gain"
// At SetPixelShader, after resolve::Substitute.
IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
// (blend over time) At every draw call (DrawPrimitive, DrawIndexedPrimitive and the two UP calls),
// before the draw: the first draw call of the last pass reads the target of the pass.
bool DrawWanted();       // the draw hooks are needed
bool FilmDrawPending();  // the last pass is set and did not draw yet
void OnDrawSlow(IDirect3DDevice9* device);
inline void OnDraw(IDirect3DDevice9* device) {
    if (FilmDrawPending()) OnDrawSlow(device);
}
void OnPresent(IDirect3DDevice9* device);
void OnReset();
void SetActive(bool on);  // in-session A/B: off = the game's own picture passes
DWORD BlendValue(D3DRENDERSTATETYPE state, DWORD value);  // at SetRenderState

}  // namespace tscfix::postfx
