#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace clearmic::platform::pipewire {

// Single producer and single consumer. Both may advance read_index with CAS
// when the producer must discard old samples. Atomic sample slots let a
// consumer safely abandon a read if the producer moves the cursor mid-copy.
class RealtimeAudioRing {
public:
    static constexpr std::size_t capacity = 1U << 15U;
    static_assert((capacity & (capacity - 1U)) == 0);
    static_assert(std::atomic<std::int16_t>::is_always_lock_free);

    [[nodiscard]] bool push(const std::int16_t* input, const std::size_t count) noexcept {
        auto write = write_index_.load(std::memory_order_relaxed);
        auto read = read_index_.load(std::memory_order_acquire);
        bool overflow = false;
        if (count > capacity) {
            input += count - capacity;
            overflow = true;
        }
        const auto stored_count = std::min(count, capacity);
        while (stored_count > capacity - static_cast<std::size_t>(write - read)) {
            const auto new_read = write + stored_count - capacity;
            if (read_index_.compare_exchange_weak(read, new_read,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                read = new_read;
                overflow = true;
                break;
            }
        }
        for (std::size_t index = 0; index < stored_count; ++index)
            samples_[(write + index) & (capacity - 1U)].store(input[index], std::memory_order_relaxed);
        write_index_.store(write + stored_count, std::memory_order_release);
        return !overflow;
    }

    [[nodiscard]] std::size_t pop(std::int16_t* output, const std::size_t count,
                                  const std::int16_t* silence) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(write - read, count));
        for (std::size_t index = 0; index < available; ++index)
            output[index] = samples_[(read + index) & (capacity - 1U)].load(std::memory_order_relaxed);
        auto expected_read = read;
        if (!read_index_.compare_exchange_strong(expected_read, read + available,
                std::memory_order_release, std::memory_order_relaxed)) {
            std::copy_n(silence, count, output);
            return 0;
        }
        std::copy_n(silence, count - available, output + available);
        return available;
    }

private:
    std::array<std::atomic<std::int16_t>, capacity> samples_{};
    std::atomic<std::uint64_t> write_index_{};
    std::atomic<std::uint64_t> read_index_{};
};

} // namespace clearmic::platform::pipewire
