#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace hand_sdk
{

// A single slot holding the most recent value of a POD, handed between one
// writer thread and one reader thread without either waiting on the other.
//
// Implemented as a seqlock: the writer bumps the sequence to odd, copies, then
// bumps it to even. A reader that sees an odd sequence, or sees the sequence
// change across its copy, knows it read a half-written value and retries.
// Why not a mutex: notes.md, "Holder".

template <typename T>
class Holder
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "Holder<T> copies T with memcpy; T must be trivially copyable");

public:
    // Publish a new value. Wait-free: never blocks, never fails.
    // Exactly one thread may call this.
    void write(const T& value)
    {
        const uint64_t seq = sequence_.load(std::memory_order_relaxed);

        sequence_.store(seq + 1, std::memory_order_relaxed);  // odd: writing
        std::atomic_thread_fence(std::memory_order_release);

        std::memcpy(&value_, &value, sizeof(T));

        std::atomic_thread_fence(std::memory_order_release);
        sequence_.store(seq + 2, std::memory_order_relaxed);  // even: settled
    }

    // Copy the newest published value into `out`.
    //
    // Returns the number of writes that had completed when the copy was taken,
    // so a caller can tell a fresh value from a repeat of the previous one.
    // Returns 0 — leaving `out` untouched — if nothing has ever been published,
    // or if every attempt raced with the writer.
    uint64_t tryRead(T& out, int max_attempts = 8) const
    {
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const uint64_t before = sequence_.load(std::memory_order_relaxed);
            if (before == 0) {
                return 0;  // nothing published yet
            }
            if ((before & 1u) != 0u) {
                continue;  // writer is mid-copy
            }
            std::atomic_thread_fence(std::memory_order_acquire);

            T scratch;
            std::memcpy(&scratch, &value_, sizeof(T));

            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                std::memcpy(&out, &scratch, sizeof(T));
                return before / 2;
            }
            // Sequence moved under us: the copy is torn, try again.
        }
        return 0;
    }

    // Completed writes so far. Safe from any thread, and cheap enough to poll.
    uint64_t sequence() const
    {
        return sequence_.load(std::memory_order_acquire) / 2;
    }

private:
    std::atomic<uint64_t> sequence_{0};
    T value_{};
};

} // namespace hand_sdk
