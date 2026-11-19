#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>

// Event telemetry for the threaded build ([experiments] trace_mask=<categories>, default 0 = off).
//
// The text log takes a lock and flushes per line: it changes the timing it reports on and cannot
// carry the thousands of events per frame that explain a multithreaded frame. Here every thread
// writes fixed 32-byte records into its own ring (no lock, no system call); a writer thread
// drains the rings into a binary file next to the game (tscfix.trace). tools/tracetool.py decodes
// it: per-frame timelines, what ran beside what, waits and their causes, the life of one engine
// object, slow-frame attribution, and state comparisons between two launches.
//
// Cost when off: one relaxed load and a predictable branch per call site.
namespace tscfix::trace {

enum Cat : uint32_t {
    kFrame = 1,      // frame boundary phases, render job, update
    kDefer = 2,      // work queued for the frame boundary and its replay
    kSync = 4,       // waits: transform sync, render idle, render-thread pauses, locks
    kOverlap = 8,    // shadow launch / join, ring and network splits, segments
    kLight = 16,     // light life and setters
    kNode = 32,      // model node enables
    kDevice = 64,    // recorder drains, presents
    kState = 128,    // periodic state snapshots (lights, ...)
    kMaterial = 256, // material modifier applies, binds of interest
    kScript = 512,   // script-driven engine calls of interest
};

// X(name, category, "what a and b hold")
#define TSCFIX_TRACE_EVENTS(X)                                                                      \
    X(Thread, 0, "a=thread id; blob=name")                                                          \
    X(Dropped, 0, "a=records this thread lost to a full ring")                                      \
    X(BoundaryBegin, kFrame, "a=pipelined frame; game thread reached the frame boundary")           \
    X(RenderWaited, kFrame, "a=ticks waited for the render job")                                    \
    X(BoundaryReplayed, kFrame, "a=ticks spent replaying deferred work")                            \
    X(FinishFrameDone, kFrame, "a=ticks (UI, end of frame, present recorded)")                      \
    X(FreesReleased, kFrame, "a=count b=ticks")                                                     \
    X(RenderLaunched, kFrame, "a=pipelined frame; update of the next frame starts now")             \
    X(RenderJobBegin, kFrame, "a=pipelined frame (render hop thread)")                              \
    X(RenderJobEnd, kFrame, "a=pipelined frame b=ticks")                                            \
    X(PipelineOn, kFrame, "pipelining starts")                                                      \
    X(DeferNode, kDefer, "a=model b=index | on<<32 | all<<40 | late<<48")                            \
    X(DeferLight, kDefer, "a=light b=setter | late<<48")                                            \
    X(DeferOther, kDefer, "a=object b=family (1 terrain 2 camera 3 consist 4 build)")               \
    X(ReplayNodes, kDefer, "a=count")                                                               \
    X(ReplayLights, kDefer, "a=count")                                                              \
    X(TransformSync, kSync, "a=caller rva b=ticks waited")                                          \
    X(RenderIdleWait, kSync, "a=caller rva b=ticks waited")                                         \
    X(LockWait, kSync, "a=lock (1 service 2 net stream 3 traffic 4 queue 5 dl heap) b=ticks")       \
    X(RenderPause, kSync, "a=ticks until parked b=1 parked 0 gave up")                              \
    X(ShadowLaunch, kOverlap, "a=shadow pass id")                                                   \
    X(ShadowJoin, kOverlap, "a=ticks waited b=reason")                                              \
    X(ShadowPassEnd, kOverlap, "a=shadow pass id b=ticks (shadow thread)")                          \
    X(RingPass, kOverlap, "a=shadow?1:0 b=rings | workers<<16")                                     \
    X(RingJob, kOverlap, "a=worker b=ticks")                                                        \
    X(LightCreate, kLight, "a=light b=kind (0 spot 1 point); blob=first 0x90 bytes of the record")  \
    X(LightDestroy, kLight, "a=light")                                                              \
    X(LightSet, kLight, "a=light b=setter | deferred<<32; blob=argument when it is a pointer")      \
    X(LightTurn, kLight, "a=component b=on | hadLight<<8 | hasPosOri<<16 | point<<24")              \
    X(LightWalkBegin, kLight, "a=point lights b=spot lights")                                       \
    X(LightWalkEnd, kLight, "a=ticks")                                                              \
    X(NodeWrite, kNode, "a=model b=index | on<<32 | all<<40 | deferred<<48")                        \
    X(NodeVisibility, kNode, "a=model b=visible | skipped<<8")                                      \
    X(Drain, kDevice, "a=reason / slot b=ticks")                                                    \
    X(Present, kDevice, "a=frame (replay thread)")                                                  \
    X(StateLights, kState, "a=count; blob=count * 0xd0 bytes of point light records")               \
    X(StateLightPtrs, kState, "a=count; blob=count * 8 bytes: the lights' addresses")               \
    X(ModifierShared, kMaterial, "a=material b=count: applied while another thread had it forced")  \
    X(ScriptActivate, kScript, "a=component b=on")                                                   \
    X(ModuleName, kFrame, "a=module vtable; blob=class name")                                       \
    X(ModuleBegin, kFrame, "a=module vtable b=phase (32 = render)")                                 \
    X(ModuleEnd, kFrame, "a=module vtable b=phase")                                                 \
    X(FramePresent, kFrame, "the frame's Present was issued (game / recording thread)")             \
    X(WorkerPassBegin, kOverlap, "a=shadow pass id (shadow thread starts its cascades)")            \
    X(WorkerPassEnd, kOverlap, "a=shadow pass id")                                                      X(MapPassBegin, kOverlap, "a=shadow thread (0 first, 1 second) starts drawing its shadow maps")     X(MapPassEnd, kOverlap, "a=shadow thread")                                                      \
    X(RingJobBegin, kOverlap, "a=worker index b=shadow?1:0 | rings<<8")                             \
    X(RingJobEnd, kOverlap, "a=worker index")                                                       \
    X(NetJobBegin, kOverlap, "a=worker")                                                            \
    X(NetJobEnd, kOverlap, "a=worker")                                                              \
    X(UpdateBegin, kFrame, "game thread: frame update starts")                                      \
    X(UpdateEnd, kFrame, "game thread: frame update done")                                          \
    X(RenderBegin, kFrame, "frame render dispatch starts (not pipelined)")                          \
    X(RenderEnd, kFrame, "frame render dispatch done (not pipelined)")                             \
    X(ForkJobBegin, kOverlap, "a=module vtable (fork worker starts a render module)")              \
    X(ForkJobEnd, kOverlap, "a=module vtable")                                                     \
    X(ForkWait, kOverlap, "a=ticks the game thread waited for the forked module b=1 at a join")    \
    X(JoinWait, kOverlap, "a=ticks the game thread waited for the shadow pass; blob=who joined")

enum Event : uint16_t {
#define X(name, cat, doc) k##name,
    TSCFIX_TRACE_EVENTS(X)
#undef X
    kEventCount
};

extern std::atomic<uint32_t> g_mask;
inline bool On(uint32_t cat) { return (g_mask.load(std::memory_order_relaxed) & cat) != 0; }

// Call only when On(category). a / b as documented per event.
void Emit(Event event, uint64_t a = 0, uint64_t b = 0);
// An event with a payload (copied into the following records).
void Blob(Event event, uint64_t a, const void* data, size_t size);
// The pipelined frame the game thread is updating; stamped into every record.
void SetFrame(uint32_t frame);
// Per-thread role bits stamped into its records: 1 shadow role, 2 ring work, 4 render job.
void SetFlag(uint8_t bit, bool on);
int64_t Now();
// Starts the writer (path next to the executable). mask 0: stays off.
void Start(uint32_t mask);

}  // namespace tscfix::trace

#define TSCFIX_TRACE(cat, ...)                                             \
    do {                                                                   \
        if (::tscfix::trace::On(cat)) ::tscfix::trace::Emit(__VA_ARGS__); \
    } while (0)
