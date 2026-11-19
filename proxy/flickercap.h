#pragma once
#include <windows.h>
#include <d3d9.h>

// [capture] flicker_key=<virtual key code> (0 = off; 145 is Scroll Lock): the mod keeps copies of the
// last presented pictures on the graphics card. When the player presses the key, it keeps recording
// for a short time and then writes the whole window of pictures to disk, in order. For faults that
// the player sees and the measures do not: the pictures hold the frames around the moment.
namespace tscfix::flickercap {

void Configure(int key, int frames);
void SetCrop(int x, int y, int width, int height);  // keep only this part of each picture (0 width: all of it)
bool Enabled();
void OnPresent(IDirect3DDevice9* device);  // before the real Present
void OnReset();

}  // namespace tscfix::flickercap
