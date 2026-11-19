#pragma once
#include <d3d9.h>

#include <cstdint>

// One-frame glitch detector ([experiments] glitch_detect=1). At every present a small grayscale
// copy of the back buffer is kept; a frame is a glitch candidate where a patch of it differs
// from both neighbours in the same direction while the neighbours agree with each other (an
// object or light that vanishes or appears for one frame). Ordinary motion changes the image
// progressively and does not match. Events go to the log, with the three frames saved next to
// the game as .pgm images. Off: no cost.
namespace tscfix::glitch {

bool Enabled();
// Render thread, right before the real Present.
void OnPresent(IDirect3DDevice9* device);
// Render thread (the device's real calls): bound state and indexed draws, kept for the last
// three frames; when a frame is flagged, its draws are compared with its neighbours'.
// kShadow: the sampler stages that have a shadow map (cascade atlas) bound, as a bit mask.
// kVSConst / kPSConst: a hash of all float constant registers of that shader stage.
// kTechnique: the name of the effect technique set last (a string owned by the effect).
enum StateKind { kVS, kPS, kIB, kVB, kTex0, kShadow, kVSConst, kPSConst, kTechnique, kStateKinds };
void NoteState(StateKind kind, const void* object);
// Any sampler stage: keeps kTex0 and the shadow-map stage mask.
void NoteTexture(DWORD stage, IDirect3DBaseTexture9* texture);
// Float shader constants set (pixel: the pixel shader's registers, else the vertex shader's).
void NoteConstants(bool pixel, UINT start, const float* data, UINT count);
void NoteDraw(INT baseVertex, UINT startIndex, UINT primCount);
// Render thread, around a real indexed draw (after NoteDraw): pixel pick, see glitch.cpp.
bool PickBegin(IDirect3DDevice9* device);
bool PickWanted();  // [experiments] glitch_pick_x is set: the non-indexed draws are hooked too
void PickEnd(IDirect3DDevice9* device);
// Render thread, right before a recorded draw replays: who recorded it (engine return address,
// recording thread).
// seq: the recording thread's count of draws so far; a jump at replay means recorded draws were
// lost on the way.
void NoteRecorder(uintptr_t caller, DWORD tid, uint64_t seq);
// Likewise for a recorded SetTexture(0): kept with the draws that use that binding.
void NoteTexSetter(uintptr_t caller, DWORD tid);
// A recorded effect SetTexture (any parameter) replays: the latest one is kept with each draw.
// r14/rdi: the caller's registers at the call (for the configured glitch_watch_rva caller,
// the material object and slot index).
void NoteFxTexture(uintptr_t caller, DWORD tid, uint64_t r14, uint64_t rdi);
// Render thread, before the real SetRenderTarget(0) / SetDepthStencilSurface: a square target
// (a shadow map) being left is copied small, so one-frame changes in it are found like on screen.
void NoteRenderTarget(IDirect3DDevice9* device, IDirect3DSurface9* next);
void NoteDepthStencil(IDirect3DSurface9* next);
void NoteViewport(const D3DVIEWPORT9* vp);
void NoteRenderState(DWORD state, DWORD v);
void NoteSlopeSetter(DWORD v, void* caller);
void NoteRealSlope(DWORD real);
bool CensusOnly();
void NoteSamplerState(DWORD stage, DWORD type, DWORD v);
void NoteBinding(int slot, uint64_t v);
void Configure(bool enabled, uintptr_t watchRva);
// Runtime switches (hypotheses), compared in one session: [experiments] glitch_variants lists
// switch names; every glitch_slice frames the next variant runs (baseline, then each switch
// alone, round robin), and one-frame events are tallied per variant.
using Switch = void (*)(bool on);
void RegisterSwitch(const char* name, Switch fn);
// A registered switch by name (nullptr: none), e.g. for an in-session A/B.
Switch FindSwitch(const char* name);
void ConfigureVariants(const char* names, int sliceFrames);
// Oracle ([experiments] oracle=1, with glitch_detect=1): frames alternate between serial
// (overlap off) and threaded rendering; draws of a threaded frame are compared with the serial
// frames around it (same scene when standing). A draw both serial frames agree on that the
// threaded frame lacks, adds or draws with other state is a threading defect, logged with its
// render target, engine call site, technique and recording thread.
void ConfigureOracle(bool on);
// Recording thread, at the frame boundary (before this frame's Present is recorded).
void OracleOnFrame();
// Mode of the frame being recorded now (0 serial, 1 threaded; -1 without the oracle).
int OracleRecordMode();
// Step mode: skip this update (consumed by the next frame update).
bool OracleSkipUpdate();
void ConfigureOracleStep(bool on, bool calibrate, int threadedPerState);

}  // namespace tscfix::glitch
