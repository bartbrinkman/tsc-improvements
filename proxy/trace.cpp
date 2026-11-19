#include "trace.h"

#include <cstdio>
#include <cstring>

#include "log.h"

namespace tscfix::trace {

std::atomic<uint32_t> g_mask{0};

namespace {

// File: a text header ("TSCFIXTRACE 1", "freq=<qpc ticks per second>", one "event <id> <category>
// <name> <doc>" line per event, "END"), then 32-byte records. A record with blob > 0 is followed
// by that many payload records (raw bytes) from the same thread.
struct Record {
    int64_t t;        // QueryPerformanceCounter
    uint16_t event;
    uint8_t thread;   // index into the thread table (Thread events)
    uint8_t flags;    // role bits (SetFlag)
    uint32_t frame;   // pipelined frame being updated when emitted
    uint64_t a;
    uint64_t b;       // blob events (event | kBlobBit): payload size in bytes
};
static_assert(sizeof(Record) == 32, "record size");

constexpr uint32_t kRing = 1u << 17;  // records per thread (4 MB)
constexpr uint16_t kBlobBit = 0x8000;  // event | kBlobBit: b = payload bytes, payload records follow

struct ThreadBuf {
    Record* ring = nullptr;
    std::atomic<uint32_t> head{0};  // next record the writer takes
    std::atomic<uint32_t> tail{0};  // next record the owner writes
    uint64_t dropped = 0, reported = 0;
    uint8_t index = 0;
    DWORD tid = 0;
};

constexpr int kMaxThreads = 128;
ThreadBuf g_bufs[kMaxThreads];
std::atomic<int> g_bufCount{0};
SRWLOCK g_bufLock = SRWLOCK_INIT;
thread_local ThreadBuf* t_buf = nullptr;
thread_local uint8_t t_flags = 0;
std::atomic<uint32_t> g_frame{0};
HANDLE g_file = INVALID_HANDLE_VALUE;

ThreadBuf* Reserve(uint32_t n);

ThreadBuf* Mine() {
    if (t_buf) return t_buf;
    AcquireSRWLockExclusive(&g_bufLock);
    int i = g_bufCount.load();
    ThreadBuf* b = nullptr;
    if (i < kMaxThreads) {
        b = &g_bufs[i];
        b->ring = static_cast<Record*>(VirtualAlloc(nullptr, sizeof(Record) * kRing, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        b->index = static_cast<uint8_t>(i);
        b->tid = GetCurrentThreadId();
        if (b->ring) g_bufCount.store(i + 1);
        else b = nullptr;
    }
    ReleaseSRWLockExclusive(&g_bufLock);
    if (!b) return nullptr;
    t_buf = b;
    // First record of every thread: who it is.
    char name[64] = "";
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof name, nullptr, nullptr);
        LocalFree(desc);
    }
    Blob(kThread, b->tid, name, strlen(name) + 1);
    return b;
}

// Room for n consecutive records, or nullptr (counted as dropped).
ThreadBuf* Reserve(uint32_t n) {
    ThreadBuf* b = Mine();
    if (!b) return nullptr;
    uint32_t tail = b->tail.load(std::memory_order_relaxed);
    if (tail - b->head.load(std::memory_order_acquire) + n > kRing) {
        b->dropped += n;
        return nullptr;
    }
    return b;
}

void Fill(Record& r, uint16_t event, uint64_t a, uint64_t b, const ThreadBuf& buf) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    r.t = t.QuadPart;
    r.event = event;
    r.thread = buf.index;
    r.flags = t_flags;
    r.frame = g_frame.load(std::memory_order_relaxed);
    r.a = a;
    r.b = b;
}

DWORD WINAPI Writer(void*) {
    SetThreadDescription(GetCurrentThread(), L"tscfix trace writer");
    for (;;) {
        Sleep(20);
        int n = g_bufCount.load();
        for (int i = 0; i < n; ++i) {
            ThreadBuf& b = g_bufs[i];
            uint32_t head = b.head.load(std::memory_order_relaxed);
            uint32_t tail = b.tail.load(std::memory_order_acquire);
            while (head != tail) {
                uint32_t at = head & (kRing - 1);
                uint32_t run = tail - head;
                if (run > kRing - at) run = kRing - at;
                DWORD wrote = 0;
                WriteFile(g_file, b.ring + at, run * sizeof(Record), &wrote, nullptr);
                head += run;
            }
            b.head.store(head, std::memory_order_release);
        }
    }
}

}  // namespace

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void SetFrame(uint32_t frame) { g_frame.store(frame, std::memory_order_relaxed); }

void SetFlag(uint8_t bit, bool on) { t_flags = on ? static_cast<uint8_t>(t_flags | bit) : static_cast<uint8_t>(t_flags & ~bit); }

void Emit(Event event, uint64_t a, uint64_t b) {
    ThreadBuf* buf = Reserve(2);  // room for a Dropped report too
    if (!buf) return;
    uint32_t tail = buf->tail.load(std::memory_order_relaxed);
    if (buf->dropped != buf->reported) {
        Fill(buf->ring[tail++ & (kRing - 1)], kDropped, buf->dropped - buf->reported, 0, *buf);
        buf->reported = buf->dropped;
    }
    Fill(buf->ring[tail++ & (kRing - 1)], event, a, b, *buf);
    buf->tail.store(tail, std::memory_order_release);
}

void Blob(Event event, uint64_t a, const void* data, size_t size) {
    uint32_t extra = static_cast<uint32_t>((size + sizeof(Record) - 1) / sizeof(Record));
    if (extra >= kRing / 2) return;
    ThreadBuf* buf = Reserve(1 + extra);
    if (!buf) return;
    uint32_t tail = buf->tail.load(std::memory_order_relaxed);
    Fill(buf->ring[tail++ & (kRing - 1)], static_cast<uint16_t>(event | kBlobBit), a, size, *buf);
    auto* p = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < extra; ++i) {
        Record& r = buf->ring[tail++ & (kRing - 1)];
        size_t n = size - i * sizeof(Record);
        if (n >= sizeof(Record)) {
            memcpy(&r, p + i * sizeof(Record), sizeof(Record));
        } else {
            memset(&r, 0, sizeof(Record));
            memcpy(&r, p + i * sizeof(Record), n);
        }
    }
    buf->tail.store(tail, std::memory_order_release);
}

void Start(uint32_t mask) {
    if (!mask || g_file != INVALID_HANDLE_VALUE) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), "tscfix.trace");
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        Log("trace: cannot create %s", path);
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    char header[16384];
    int n = sprintf_s(header, "TSCFIXTRACE 1\nfreq=%lld\nrecord=32\n", f.QuadPart);
    struct Info { const char* name; uint32_t cat; const char* doc; };
    static const Info events[] = {
#define X(name, cat, doc) {#name, cat, doc},
        TSCFIX_TRACE_EVENTS(X)
#undef X
    };
    for (int i = 0; i < kEventCount; ++i)
        n += sprintf_s(header + n, sizeof header - n, "event %d %u %s %s\n", i, events[i].cat, events[i].name, events[i].doc);
    n += sprintf_s(header + n, sizeof header - n, "END\n");
    DWORD wrote = 0;
    WriteFile(g_file, header, n, &wrote, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &Writer, nullptr, 0, nullptr)) CloseHandle(t);
    g_mask.store(mask);
    Log("trace: on, categories 0x%x, file %s", mask, path);
}

}  // namespace tscfix::trace
