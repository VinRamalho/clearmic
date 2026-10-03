#pragma once

#include "clearmic/audio/device.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clearmic::platform::pipewire {
[[nodiscard]] inline bool upower_address_matches(std::string_view native_path, std::string_view serial,
                                                 std::string_view address) noexcept {
    auto normalize = [](const std::string_view value, const bool path) {
        std::string normalized;
        normalized.reserve(value.size() + (path ? 4U : 0U));
        if (path) normalized = "DEV_";
        for (const unsigned char character : value) {
            if (character == ':' || character == '-') {
                if (path) normalized.push_back('_');
                continue;
            }
            normalized.push_back(static_cast<char>(character >= 'a' && character <= 'z'
                ? character - 'a' + 'A' : character));
        }
        return normalized;
    };
    const auto expected_path = normalize(address, true);
    const auto expected_serial = normalize(address, false);
    const auto normalized_path = normalize(native_path, false);
    return normalized_path.ends_with(expected_path) ||
           normalize(serial, false) == expected_serial;
}

[[nodiscard]] inline std::optional<audio::BatteryInfo> upower_battery_from_values(
    bool power_supply, bool address_matches, unsigned int type, double percentage,
    std::optional<unsigned int> battery_level, unsigned int state) noexcept {
    if (power_supply || !address_matches ||
        (type != 17U && type != 19U && type != 21U && type != 28U) ||
        !std::isfinite(percentage) || percentage < 0.0 || percentage > 100.0 ||
        (battery_level && *battery_level != 1U)) return std::nullopt;

    auto charging = audio::ChargingState::unknown;
    switch (state) {
    case 1U: charging = audio::ChargingState::charging; break;
    case 2U: charging = audio::ChargingState::not_charging; break;
    case 4U: charging = audio::ChargingState::full; break;
    default: break;
    }
    return audio::BatteryInfo{static_cast<unsigned int>(std::lround(percentage)), charging};
}
void populate_upower_battery(std::vector<audio::AudioDevice>& devices) noexcept;
}
