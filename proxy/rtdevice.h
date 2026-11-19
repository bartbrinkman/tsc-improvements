#pragma once
#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>

// Render-thread device wrapper. The engine talks to an RtDevice instead of the
// runtime's device.
//
//   PassThrough - every call is forwarded directly on the calling thread
//                 (validates that the wrapper is complete).
//   Threaded    - hot state/draw calls are recorded into a queue that a render
//                 thread replays, so the game thread can build the next frame
//                 while the previous one is submitted. Calls that return data
//                 are answered from shadow state where possible; everything
//                 else drains the queue first and then runs directly.
namespace tscfix::rt {

enum class Mode { Off = 0, PassThrough = 1, Threaded = 2 };

// Wraps a freshly created device; takes over the caller's reference to `real`.
IDirect3DDevice9* Wrap(IDirect3DDevice9* real, Mode mode);
// Threaded mode: put the game and render threads on separate physical cores.
void SetSplitCores(bool on);
// Threaded mode: switch recording on/off live (game thread, frame boundary; for A/B tests).
void SetThreadedActive(bool on);
// Threaded mode: answer GetDesc on bound render targets from a cache (A/B switch).
void SetDescCacheActive(bool on);
// Threaded mode: the recorder has a single producer. A thread that blocks while another
// one issues device calls on its behalf hands recording over (and takes it back after).
void SetRecordingThread(DWORD tid);
// Threaded mode, overlap: the calling thread hands the queue to `worker` and records into a
// side buffer until EndOverlap (calling thread, worker idle) appends it to the queue. A call
// on the side buffer that needs the queue first runs the join hook (which must wait for the
// worker and call EndOverlap). Returns false if recording is not active on this thread.
bool BeginOverlap(DWORD worker);
void EndOverlap();
void SetOverlapJoinHook(void (*hook)());
// Overlap: returns true once the worker has finished (the side buffer then joins at once).
void SetOverlapWorkerDoneHook(bool (*hook)());
// Queue positions (game side): where the next recorded command goes, and whether the render
// thread has run everything before `pos`. Side-buffer commands get their position when the
// side buffer is appended; the hook is told the queue head after that append.
uint64_t QueueHead();
bool QueueReached(uint64_t pos);
void SetSideAppendedHook(void (*hook)(uint64_t head));
// Drop SetRenderState / SetSamplerState calls that set the value the device already has (as the
// recorder knows it), so neither thread spends time on them.
void SetFilterStates(bool on);
// [experiments] park_keeps_state: device state saved and put back around a parked direct call.
void SetParkKeepsState(bool on);
long ParkSaves();  // saves since the last call (for the periodic report)
// 0: this thread records into the queue, 1: into the side buffer, -1: not recording.
int RecorderRole();

// Called by hooks on objects the engine uses directly (resource locks, state
// blocks): drain the queue so those calls observe all earlier device calls.
void SyncForDirectAccess(int reason);
// State changed behind the wrapper's back: forget shadow state.
void InvalidateShadow();
// An effect pass ran on the render thread: forget only state an effect can change.
void InvalidateEffectState();
// State block Capture/Apply recorded on the game thread: keep the shadow state in step
// (Apply restores the snapshot taken at Capture, or invalidates if there is none).
void StateBlockCaptured(IDirect3DStateBlock9* sb);
void StateBlockApplied(IDirect3DStateBlock9* sb);

// kSyncLockBase + kind * 4 + flags class: kind 0 VB, 1 IB, 2 texture, 3 surface; class 0 plain,
// 1 read-only, 2 no-overwrite, 3 discard.
enum SyncReason { kSyncLockBase = 210, kSyncLock = 200, kSyncStateBlock = 201, kSyncEffect = 202, kSyncQuery = 203, kSyncOverlap = 204 };

// For other wrappers (D3DX effects) that feed the same render thread.
// True when calls on this thread should be recorded rather than executed.
bool Recording();
// The calling thread records into the queue itself (threaded mode, not an overlap side buffer).
bool OwnsQueue();
// The calling thread records into an overlap side segment (not the queue itself).
bool SideRecorder();
// While alive, the calling side recorder may call the device directly: the render thread is
// parked between commands. No-op on other threads.
class ExclusiveDevice {
 public:
    ExclusiveDevice();
    ~ExclusiveDevice();
    ExclusiveDevice(const ExclusiveDevice&) = delete;
    ExclusiveDevice& operator=(const ExclusiveDevice&) = delete;
 private:
    bool held_ = false;
};
// An overlap is running (more than one thread records).
bool OverlapRunning();
// Overlap, called by a side recorder: `worker` records the next part of the command stream and
// the caller continues after it (replayed in that order). The worker calls SegmentDone when it
// has finished; the overlap may only end after that.
bool ForkSegment(DWORD worker);
void SegmentDone();
// Overlap, side recorder that keeps recording: reserve the next ordered slot for `worker`
// (after the caller's segment and earlier reservations); later ResumeAfterForks puts the
// caller's further commands behind all reserved slots.
// `fresh`: the worker starts knowing nothing of the device state (its commands are replayed after
// commands the caller records later, as when the queue holder forks and keeps the queue).
bool ForkAhead(DWORD worker, bool fresh = false);
// Crash report: the calling thread's recording role and the overlap's segments.
void LogThreadRole();
// Commands the calling thread has recorded so far (a counter for measurements).
uint64_t ThreadRecords();
// Counts the calling thread's recorded commands per method slot into counts[256] (nullptr: stop).
void CountSlots(uint32_t* counts);
// The calling thread issues device calls while another thread records (pipelined update):
// without a recording role its calls run directly, never draining the queue.
void SetDirectCaller(bool on);
// True on such a thread when it has no recording role (its device calls run directly).
bool DirectCaller();
// Such a thread parks the render thread at a command boundary around a direct D3DX call
// (nested per thread)...
DWORD RenderThreadId();
long PauseRequests();  // outstanding render-thread pause requests (hang reports)
void PauseRenderThread();
void ResumeRenderThread();
// ...or, where the call has no scope end (tail-jumping thunks), until it resumes it explicitly;
// it must do so before waiting for anything that needs the render thread.
void PauseForDirectCall();
void ResumeAfterDirectCalls();
// Counts DrawIndexedPrimitive call sites into `sites` (cap entries of {void* frames[3]; uint32_t n}).
void CountDrawSites(void* sites, int cap);
const char* SlotName(int slot);
bool ResumeAfterForks();
// A GetRenderState call site whose code was read: it only hands the value back to
// SetRenderState later (save / restore). While the value is unknown, such a caller gets a
// placeholder; the capture and the restore run in queue order on the render thread.
void AddSaveRestoreSite(const char* module, uintptr_t returnOffset);
// Records exec(copy of blob followed by payload) to run on the render thread, in order
// with device calls. Only valid when Recording() is true.
void Enqueue(void (*exec)(const uint8_t* blob), const void* blob, size_t size, const void* payload = nullptr,
             size_t payloadSize = 0);
// Keeps obj alive until queued commands using it have run (main thread).
void KeepAlive(IUnknown* obj);

}  // namespace tscfix::rt
