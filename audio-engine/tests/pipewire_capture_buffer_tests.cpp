#include "clearmic/platform/linux/capture_buffer.hpp"
#include "clearmic/platform/linux/realtime_audio_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("ClearMic test failure: ") + message);
}
}

int main() {
    using clearmic::platform::pipewire::aligned_pcm16_chunk;
    using clearmic::platform::pipewire::valid_capture_chunk_range;

    require(valid_capture_chunk_range(0, 0, 0), "Empty range at an empty buffer should be valid");
    require(valid_capture_chunk_range(8, 8, 16), "Range ending exactly at buffer capacity should be valid");
    require(!valid_capture_chunk_range(17, 0, 16), "Offset beyond buffer capacity should be rejected");
    require(!valid_capture_chunk_range(8, 9, 16), "Range extending beyond buffer capacity should be rejected");
    require(valid_capture_chunk_range(std::numeric_limits<std::size_t>::max(), 0,
                                      std::numeric_limits<std::size_t>::max()),
            "Maximum valid offset should not overflow range validation");
    require(aligned_pcm16_chunk(0, 0) && aligned_pcm16_chunk(2, 2) && aligned_pcm16_chunk(480, 480),
            "PCM16 aligned chunks should be accepted");
    require(!aligned_pcm16_chunk(0, 1) && !aligned_pcm16_chunk(0, 3) && !aligned_pcm16_chunk(1, 2),
            "Unaligned PCM16 offsets and sizes should be rejected");

    using clearmic::platform::pipewire::RealtimeAudioRing;
    RealtimeAudioRing ring;
    constexpr auto ring_capacity = RealtimeAudioRing::capacity;
    std::array<std::int16_t, ring_capacity> first_block{};
    std::array<std::int16_t, ring_capacity / 2> second_block{};
    std::array<std::int16_t, ring_capacity> output{};
    std::array<std::int16_t, ring_capacity> silence{};
    for (std::size_t index = 0; index < first_block.size(); ++index)
        first_block[index] = static_cast<std::int16_t>(index % 30000U + 1U);
    second_block.fill(1234);
    require(ring.push(first_block.data(), first_block.size()), "An empty ring should accept a full-capacity block");
    require(!ring.push(second_block.data(), second_block.size()), "A full ring should report that old audio was dropped");
    require(ring.pop(output.data(), output.size(), silence.data()) == ring_capacity,
            "A full ring should return a full-capacity read");
    for (std::size_t index = 0; index < ring_capacity / 2; ++index)
        require(output[index] == first_block[index + ring_capacity / 2], "Overflow should discard the oldest queued samples");
    for (std::size_t index = ring_capacity / 2; index < output.size(); ++index)
        require(output[index] == second_block[index - ring_capacity / 2], "Wrapped samples should remain ordered after overflow");
    require(ring.pop(output.data(), 32, silence.data()) == 0, "An empty ring should report an underrun");
    require(std::all_of(output.begin(), output.begin() + 32, [](const auto sample) { return sample == 0; }),
            "An underrun should fill the output with silence");

    RealtimeAudioRing concurrent_ring;
    std::atomic_bool producer_done{};
    std::atomic_bool concurrent_data_valid{true};
    std::thread producer([&] {
        std::array<std::int16_t, 32> packet{};
        for (std::int32_t sequence = 1; sequence <= 20000; ++sequence) {
            packet.fill(static_cast<std::int16_t>(sequence));
            static_cast<void>(concurrent_ring.push(packet.data(), packet.size()));
            if ((sequence & 15) == 0) std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::array<std::int16_t, 32> packet{};
        std::array<std::int16_t, 32> empty{};
        while (!producer_done.load(std::memory_order_acquire)) {
            if (concurrent_ring.pop(packet.data(), packet.size(), empty.data()) == 0) {
                std::this_thread::yield();
                continue;
            }
            if (packet.front() == 0 ||
                !std::all_of(packet.begin(), packet.end(), [&](const auto sample) { return sample == packet.front(); }))
                concurrent_data_valid.store(false, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    require(concurrent_data_valid.load(std::memory_order_relaxed),
            "Concurrent overflow must not expose partially overwritten audio packets");

    std::cout << "PipeWire capture buffer validation passed\n";
}
