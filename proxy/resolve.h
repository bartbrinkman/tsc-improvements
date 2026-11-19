#pragma once
#include <windows.h>
#include <d3d9.h>

// [graphics] better_resolve: the game's supersampling shrink step with a better filter.
//
// With supersampling on, the game draws its picture at 2 or 3 times the window size per axis and
// shrinks it with one pixel shader. That shader reads 8 single points per screen pixel with equal
// weights. Here it is replaced by a filter that reads every rendered point of the pixel and the
// nearest points of the neighbour pixels (see resolve.cpp).
namespace tscfix::resolve {

struct Options {
    int kernel = 0;       // 0 off, 1 Lanczos (radius 2 pixels), 2 Mitchell (radius 2), 3 exact average of the pixel's points
    bool linear = true;   // filter in linear light (gamma 2.0) instead of on the stored values
    int anisotropy = 0;   // [graphics] anisotropy: 0 leaves the game's value (4), else the level to use (8, 16)
    bool compare = false; // (measurement) save pictures of every kernel in turn, same scene
    bool hooksWanted = false;  // another module (postfx) needs the same device hooks
};
void Configure(const Options& options);
bool Enabled();  // the device hooks are needed

// From the device hooks (see d3dlayer.cpp).
bool IsShrinkShader(const DWORD* code);                                  // at CreatePixelShader
void NoteShrinkShader(IDirect3DPixelShader9* shader);                    // the created original
void NoteRenderTarget(UINT width, UINT height);                          // every render-target texture
// At SetPixelShader: the shader to set in place of `shader` (the same one when nothing changes).
IDirect3DPixelShader9* Substitute(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
void OnSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);  // before the real SetTexture
void OnPresent(IDirect3DDevice9* device);  // before the real Present
void OnReset();
DWORD Anisotropy(DWORD value);  // at SetSamplerState(MAXANISOTROPY): the value to set
void SetActive(bool on);  // in-session A/B
bool ShrinkInUse();  // the game's supersampling shrink shader was set in this session
void ForceVariant(int kernel);  // (postfx compare) the shrink filter of the frame drawn now: -1 by the options, 0 the game's, 1 to 3

}  // namespace tscfix::resolve
