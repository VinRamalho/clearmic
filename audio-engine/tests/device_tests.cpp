#include "clearmic/audio/device.hpp"

#include <optional>

int main() {
    using namespace clearmic::audio;
    if (!is_valid_battery_percentage(0) || !is_valid_battery_percentage(100) || is_valid_battery_percentage(101)) return 1;
    AudioDevice device;
    if (device.capabilities.battery != std::nullopt) return 2;
    if (device.capabilities.transmitter_battery != std::nullopt) return 3;
    if (device.capabilities.receiver_battery != std::nullopt) return 4;
    return 0;
}
