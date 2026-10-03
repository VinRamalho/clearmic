#include "clearmic/audio/device.hpp"

#include <algorithm>
#include <charconv>
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

std::optional<std::uint16_t> parse_device_identifier(std::string_view value) noexcept {
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) {
        value.remove_prefix(2);
        base = 16;
    }
    if (value.empty()) return std::nullopt;
    unsigned int parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 0xffffU)
        return std::nullopt;
    return static_cast<std::uint16_t>(parsed);
}
} // namespace clearmic::audio
