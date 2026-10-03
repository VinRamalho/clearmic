#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace clearmic::platform::pipewire {

// PipeWire's requested sample count is the payload size; maxsize is only the
// mapped allocation capacity. A zero request means no resampler hint was given.
[[nodiscard]] inline std::optional<std::size_t> source_sample_count(
    const std::uint64_t requested, const std::size_t max_bytes) noexcept {
    constexpr auto sample_bytes = sizeof(std::int16_t);
    if (max_bytes % sample_bytes != 0) return std::nullopt;
    const auto capacity = max_bytes / sample_bytes;
    if (requested == 0) return capacity;
    if (requested > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return std::nullopt;
    const auto count = static_cast<std::size_t>(requested);
    if (count > capacity) return std::nullopt;
    return count;
}

} // namespace clearmic::platform::pipewire
