#pragma once

#include <cstddef>
#include <cstdint>

namespace clearmic::platform::pipewire {

[[nodiscard]] constexpr bool valid_capture_chunk_range(const std::size_t offset, const std::size_t size,
                                                        const std::size_t capacity) noexcept {
    return offset <= capacity && size <= capacity - offset;
}

[[nodiscard]] constexpr bool aligned_pcm16_chunk(const std::size_t offset, const std::size_t size) noexcept {
    return offset % alignof(std::int16_t) == 0 && size % sizeof(std::int16_t) == 0;
}

} // namespace clearmic::platform::pipewire
