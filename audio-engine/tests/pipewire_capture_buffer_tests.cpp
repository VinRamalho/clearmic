#include "clearmic/platform/linux/capture_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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

    std::cout << "PipeWire capture buffer validation passed\n";
}
