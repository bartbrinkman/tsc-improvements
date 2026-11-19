#pragma once
#include <windows.h>

#include <cstdint>

// Overlapping the shadow pass with other render work. The engine's scheduler runs its
// render modules one after another through a single dispatch call; that call site is
// redirected here so each module can be timed (census) and, with overlap active, run
// while the shadow pass is still being prepared on a worker thread.
namespace tscfix::overlap {

// Redirects the scheduler's module dispatch (located from code; nothing game-specific is
// configured). `census`: log module order and time every 300 frames.
bool Install(HMODULE engine, bool census);
void OnFrame();  // game thread, at Present
// Research builds: single-step trace each following render module up to the scenery manager.
void ArmModuleTrace();

// Overlap: starts the worker and reads the allow-list (comma-separated class-name parts of
// render modules that may run alongside the shadow pass). Needs Install() first.
bool InstallOverlap(const char* allowList);
// Before the first overlap: 1 = worker on its own physical core, 2 = worker and game thread
// share one physical core (both logical processors).
void SetDedicatedCore(int mode);
void SetActive(bool on);
// Frame boundary: the shadow pass goes to the worker or not; nothing else changes.
void SetOverlapOnly(bool on);  // game thread, frame boundary (A/B toggle)
// (diagnosis) While on, modules' update phases are skipped except an allowed one (see below).
void SetUpdateFreeze(bool on);
// The per-object update (frame update loop): skip this object's update now?
bool UpdateSkipped(void** vtable);
// Comma list of module class-name substrings; each becomes glitch switch "upd:<name>" that lets
// that module's update run while the rest stays frozen.
void SetUpdateSkipOne(bool on);
void RegisterUpdateSwitches(const char* list);
// Automatic: overlap on while the smoothed frame time is above onMs, off below offMs.
void SetAuto(double onMs, double offMs);
// Shadow cascade drawing time (QPC ticks), inline or on the worker: the automatic mode decides
// on what the overlap hides, not on frame time (the overlap itself changes the frame time).
void AddShadowTime(int64_t ticks);
// Research builds: removes / restores the engine hooks the overlap needs (false: not now).
void SetEngineHooksSwitch(bool (*fn)(bool on));
// Called by the shadow cascade hook at entry: the worker has finished the cascade setup.
void ShadowSetupDone();
// Overlap on: runs job(ctx) (the shadow cascade drawing) on the worker and returns true; the
// game thread goes on recording into the side buffer until a module not on the allow-list,
// or a device call that needs the queue, joins. beforeLaunch(worker id) runs on the game
// thread first (per-thread state snapshots).
bool LaunchCascades(uintptr_t (*job)(void*), void* ctx, void (*beforeLaunch)(DWORD worker));
// Game thread: wait for the worker and end the overlap now (before taking a lock the worker
// may need, so the game thread never waits for the worker while holding it).
void JoinIfOverlapping(const char* why);
// The game thread is blocked waiting for the worker (a lock it holds cannot be released now).
bool GameThreadJoining();
// The game thread (the overlap's owner) is the holder of `lock`.
bool GameThreadHolds(const CRITICAL_SECTION* lock);
// Second shadow thread (experiments.cpp hands it some of the shadow maps): its worker.
bool StartSplitWorker();
DWORD SplitWorkerTid();
void LaunchSplit(uintptr_t (*job)(void*), void* ctx);
void WaitSplit();
// The game thread is blocked until the calling worker (shadow or forked module) finishes its
// job. A lock the game thread holds stays held, but it does nothing under it meanwhile.
bool GameThreadWaitingForMe();
// ...and it is the thread that holds `lock` (another worker holding it is simply waited for).
bool GameThreadHoldsWhileWaitingForMe(const CRITICAL_SECTION* lock);
// A worker may be drawing right now (between launch and join). Locks that only guard against
// the overlap are needed only then: the game thread starts and ends it, so none of its calls
// straddles a change.
bool Overlapping();
// overlap_core=1: keeps the calling thread off the worker's core too (another thread that runs
// beside the overlap, e.g. a pipelined update). False if no core is reserved.
bool KeepOffWorkerCore();
// Logs the calling thread's, `other`'s and the overlap worker's stacks (hang reports).
void LogStacks(const char* why, HANDLE other, const char* otherName);
// Thread ids of the overlap worker and the fork worker (0 while not started).
void WorkerThreadIds(DWORD out[2]);

// Fork ([experiments] overlap_fork=<class-name part>): during the overlap that render module
// runs on a second worker. prepare(worker) runs on the game thread before the launch (snapshots
// for the worker), enter / leave on the worker around the module.
// closeCaller runs on the game thread before the fork and again before it continues behind the
// fork: what it leaves open in the command stream (a bound material) is closed, so each part of
// the stream starts from nothing.
struct ForkHooks {
    void (*prepare)(DWORD worker);
    void (*enter)();
    void (*leave)();
    void (*closeCaller)();
    void (*joined)();  // game thread, after the forked module's worker has finished: the module's place in the engine's order is passed
    void (*slot)();    // (measurement) game thread, at the forked module's place in the engine's order when it was started ahead
};
void SetForkHooks(const ForkHooks& hooks);
// (measurement) Called on the game thread right before it runs a render module itself, with the
// module's class name.
void SetModuleBeginHook(void (*hook)(const char* className));
// The same, right after the module returns.
void SetModuleEndHook(void (*hook)(const char* className));
// Runs on the calling thread right before the shadow pass goes to the worker.
void SetBeforeShadowHook(void (*hook)());
// Called around each module call the dispatch hook makes (class name, phase argument, enter/leave).
using ModuleGuardFn = void (*)(const char* className, int arg, bool enter);
void SetModuleGuard(ModuleGuardFn guard);
bool OnForkWorker();
bool ForkActive();  // a module is configured to run on the fork worker
bool OnSplitWorker();  // the calling thread is the second shadow thread
// Game thread, frame boundary (in-session A/B): the forked module runs on its worker (on) or
// stays on the game thread (off).
void SetForkActive(bool on);
// Game thread: waits for the forked module (not for the shadow pass) if one is running. Call
// before taking a lock the forked module's worker may need: the game thread must never wait
// for that worker while holding it.
void WaitForFork();

}  // namespace tscfix::overlap
