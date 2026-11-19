#pragma once
#include <windows.h>

#include <cstdint>

// Runs a piece of game work on a dedicated worker thread. The worker becomes the
// render-thread recorder's producer while it runs (rt::SetRecordingThread).
namespace tscfix::offload {

using Job = uintptr_t (*)(void* ctx);

class Worker {
 public:
    // Starts the thread (large stack: engine traversals recurse deeply).
    bool Start(const wchar_t* name);
    // Runs job(ctx) on the worker and waits for it; returns the job's result.
    uintptr_t Run(Job job, void* ctx);
    // Starts job(ctx) without waiting (the caller arranges recording, see rt::BeginOverlap);
    // Wait() blocks until it is done. One job at a time.
    void Launch(Job job, void* ctx);
    uintptr_t Wait();
    bool WaitFor(DWORD ms);  // true when the launched job is done (then like Wait())
    HANDLE Thread() const { return thread_; }
    bool Busy() const { return busy_; }
    bool Finished() const { return finished_; }  // the launched job has returned
    DWORD ThreadId() const { return tid_; }

 private:
    static DWORD WINAPI Main(void* self);
    HANDLE thread_ = nullptr;
    DWORD tid_ = 0;
    HANDLE go_ = nullptr, done_ = nullptr;
    Job job_ = nullptr;
    void* ctx_ = nullptr;
    uintptr_t result_ = 0;
    volatile bool busy_ = false;
    volatile bool finished_ = false;
};

}  // namespace tscfix::offload
