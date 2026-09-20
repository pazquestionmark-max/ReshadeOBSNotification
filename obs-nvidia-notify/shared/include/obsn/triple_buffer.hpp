// SPDX-License-Identifier: MIT
// Single-producer / single-consumer triple buffer.
//
// This is what lets the render thread pick up the newest OBS state with one atomic
// exchange and no lock, no allocation and no copy — the property the whole "never block the
// game's render thread" requirement rests on.
//
// Three slots: the producer always owns one, the consumer always owns one, and the third is the
// handoff. Publishing is an exchange of the producer's slot index with the shared one; acquiring
// is the mirror. Neither side ever waits for the other, and the consumer always sees either the
// previous complete frame or the newest complete frame, never a partially written one.
#ifndef OBSN_TRIPLE_BUFFER_HPP
#define OBSN_TRIPLE_BUFFER_HPP

#include <atomic>
#include <cstdint>

namespace obsn {

template <typename T>
class TripleBuffer {
public:
    TripleBuffer() : shared_(kIndex1) {}

    /// Producer-side slot. Safe to mutate until publish() is called.
    T& write_slot() noexcept { return slots_[producer_]; }

    /// Makes the producer's slot visible to the consumer and takes a fresh slot to write into.
    void publish() noexcept {
        const std::uint32_t swapped =
            shared_.exchange(producer_ | kDirtyBit, std::memory_order_acq_rel);
        producer_ = swapped & kIndexMask;
    }

    /// True when the producer has published something the consumer has not taken yet.
    bool has_update() const noexcept {
        return (shared_.load(std::memory_order_acquire) & kDirtyBit) != 0;
    }

    /// Takes the newest published slot if there is one, and returns the consumer's current slot.
    /// Cheap enough to call unconditionally once per frame.
    const T& acquire() noexcept {
        if (has_update()) {
            const std::uint32_t swapped = shared_.exchange(consumer_, std::memory_order_acq_rel);
            consumer_ = swapped & kIndexMask;
        }
        return slots_[consumer_];
    }

    /// The consumer's slot without checking for an update.
    const T& current() const noexcept { return slots_[consumer_]; }

private:
    static constexpr std::uint32_t kIndexMask = 0x3;
    static constexpr std::uint32_t kDirtyBit = 0x4;
    static constexpr std::uint32_t kIndex1 = 1;

    T slots_[3];
    std::uint32_t producer_ = 0;   ///< producer thread only
    std::uint32_t consumer_ = 2;   ///< consumer thread only
    std::atomic<std::uint32_t> shared_;
};

}  // namespace obsn

#endif  // OBSN_TRIPLE_BUFFER_HPP
