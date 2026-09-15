// pcm_ring.h — lock-free SPSC ring of interleaved stereo int16 samples.
//
// Decoupling step (T2-3): the OpenSL buffer-queue callback has hard real-time
// constraints, so it must not run Vorbis decoding inline. A feed thread decodes
// ahead into this ring; the callback only copies samples out (and pads silence
// if the feed thread has fallen behind).
//
// One producer (the feed thread, or a one-shot prime before the voice becomes
// visible to it) and one consumer (the callback). Capacity is rounded up to a
// power of two; one slot is kept free so full/empty are distinguishable.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace artc {

class PcmRing {
public:
    explicit PcmRing(size_t capacity_samples)
        : buf_(RoundPow2(capacity_samples + 1)) {}

    // Producer: writes up to `samples`, returning how many were stored
    // (truncated at the free space).
    size_t Write(const int16_t *src, size_t samples) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free_slots = buf_.size() - 1 - Count(head, tail);
        const size_t n = samples < free_slots ? samples : free_slots;
        for (size_t i = 0; i < n; ++i) buf_[(head + i) & mask()] = src[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer: reads up to `samples`, returning how many were available.
    size_t Read(int16_t *dst, size_t samples) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t avail = Count(head, tail);
        const size_t n = samples < avail ? samples : avail;
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(tail + i) & mask()];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    size_t Available() const {
        return Count(head_.load(std::memory_order_acquire),
                     tail_.load(std::memory_order_acquire));
    }
    size_t Space() const { return buf_.size() - 1 - Available(); }
    size_t Capacity() const { return buf_.size() - 1; }

private:
    static size_t RoundPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    size_t mask() const { return buf_.size() - 1; }
    size_t Count(size_t head, size_t tail) const { return (head - tail) & mask(); }

    std::vector<int16_t> buf_;
    std::atomic<size_t> head_{0};   // producer cursor
    std::atomic<size_t> tail_{0};   // consumer cursor
};

} // namespace artc
