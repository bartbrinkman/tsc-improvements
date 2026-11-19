#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Copy-on-write snapshots for one writer thread and many reader threads.
//
// The writer (the update) is about to change part of an object while readers (the render) may
// still be reading the previous frame's state. Before its first write in a frame it copies the
// bytes the readers use into a table; readers that look the object up get that copy. At the frame
// boundary, when no reader runs, the table is emptied.
//
// Ordering (x86): the writer publishes a snapshot before it touches the object. A reader that
// finds none reads the object and must look again afterwards (see Read): if a snapshot appeared
// meanwhile the write may have overlapped its read, and the snapshot is what it should use.
namespace tscfix {

template <size_t Bytes, int Bits = 14, int Probes = 64>
class CowTable {
public:
    bool Init() {
        slots_ = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) << Bits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        used_.reserve(size_t(1) << Bits);
        return slots_ != nullptr;
    }

    // Writer thread: snapshot `size` bytes at obj+offset unless this frame already has one.
    // False when the table is full (the caller must then wait for the readers instead).
    bool Snapshot(void* obj, size_t offset) {
        for (size_t i = Slot0(obj), n = 0; n < Probes; ++n, i = (i + 1) & kMask) {
            Slot& s = slots_[i];
            if (s.key == obj) return true;
            if (s.key) continue;
            memcpy(s.data, static_cast<uint8_t*>(obj) + offset, Bytes);
            InterlockedExchangePointer(const_cast<void**>(&s.key), obj);
            used_.push_back(static_cast<uint32_t>(i));
            InterlockedIncrement(&count_);
            ++taken_;
            return true;
        }
        ++full_;
        return false;
    }

    // Any thread: the snapshot of obj, or nullptr.
    const uint8_t* Find(const void* obj) const {
        for (size_t i = Slot0(obj), n = 0; n < Probes; ++n, i = (i + 1) & kMask) {
            void* k = slots_[i].key;
            if (k == obj) return slots_[i].data;
            if (!k) return nullptr;
        }
        return nullptr;
    }

    bool Empty() const { return count_ == 0; }

    // Reader threads registered for this frame (set at the boundary by the writer).
    void SetReaders(const DWORD* tids, int n) {
        n = n < kMaxReaders ? n : kMaxReaders;
        for (int i = 0; i < n; ++i) readers_[i] = tids[i];
        readerCount_ = n;
    }
    bool IsReader() const {
        DWORD tid = GetCurrentThreadId();
        for (LONG i = 0, n = readerCount_; i < n; ++i)
            if (readers_[i] == tid) return true;
        return false;
    }

    // Writer thread at the frame boundary, no reader running.
    void Reset() {
        for (uint32_t i : used_) slots_[i].key = nullptr;
        used_.clear();
        count_ = 0;
    }

    // Measurement, writer thread: snapshots taken / table-full fallbacks since the last call.
    uint64_t TakeTaken() { uint64_t t = taken_; taken_ = 0; return t; }
    uint64_t TakeFull() { uint64_t f = full_; full_ = 0; return f; }

private:
    static constexpr size_t kMask = (size_t(1) << Bits) - 1;
    static constexpr int kMaxReaders = 16;
    struct Slot {
        void* volatile key;
        uint8_t data[Bytes];
    };
    static size_t Slot0(const void* p) {
        return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - Bits));
    }
    Slot* slots_ = nullptr;
    std::vector<uint32_t> used_;
    volatile LONG count_ = 0;
    DWORD readers_[kMaxReaders] = {};
    volatile LONG readerCount_ = 0;
    uint64_t taken_ = 0, full_ = 0;
};

}  // namespace tscfix
