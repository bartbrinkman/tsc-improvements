#include "offload.h"

#include "rtdevice.h"

namespace tscfix::offload {

bool Worker::Start(const wchar_t* name) {
    if (thread_) return true;
    go_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    done_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, size_t(8) << 20, &Main, this, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid_);
    if (!thread_) return false;
    SetThreadDescription(thread_, name);
    return true;
}

uintptr_t Worker::Run(Job job, void* ctx) {
    job_ = job;
    ctx_ = ctx;
    DWORD self = GetCurrentThreadId();
    rt::SetRecordingThread(tid_);  // the event handoff orders this thread's recording before the worker's
    SetEvent(go_);
    WaitForSingleObject(done_, INFINITE);
    rt::SetRecordingThread(self);
    return result_;
}

void Worker::Launch(Job job, void* ctx) {
    job_ = job;
    ctx_ = ctx;
    busy_ = true;
    finished_ = false;
    SetEvent(go_);
}

uintptr_t Worker::Wait() {
    if (!busy_) return result_;
    WaitForSingleObject(done_, INFINITE);
    busy_ = false;
    return result_;
}

bool Worker::WaitFor(DWORD ms) {
    if (!busy_) return true;
    if (WaitForSingleObject(done_, ms) != WAIT_OBJECT_0) return false;
    busy_ = false;
    return true;
}

DWORD WINAPI Worker::Main(void* p) {
    auto* w = static_cast<Worker*>(p);
    for (;;) {
        WaitForSingleObject(w->go_, INFINITE);
        w->result_ = w->job_(w->ctx_);
        w->finished_ = true;
        SetEvent(w->done_);
    }
}

}  // namespace tscfix::offload
