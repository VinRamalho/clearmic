#include "clearmic/audio/device.hpp"

#include <algorithm>
#include <limits>

namespace clearmic::audio {
bool is_valid_battery_percentage(const unsigned int percentage) noexcept { return percentage <= 100; }

std::optional<unsigned int> battery_percentage_from_capacity(const std::uint32_t current_capacity,
                                                              const std::uint32_t full_capacity) noexcept {
    constexpr auto unknown = std::numeric_limits<std::uint32_t>::max();
    if (current_capacity == unknown || full_capacity == 0 || full_capacity == unknown) return std::nullopt;
    const auto percentage = static_cast<std::uint64_t>(current_capacity) * 100U / full_capacity;
    return static_cast<unsigned int>(std::min<std::uint64_t>(percentage, 100U));
}
} // namespace clearmic::audio
