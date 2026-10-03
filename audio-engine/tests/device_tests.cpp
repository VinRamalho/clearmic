#include "clearmic/audio/device.hpp"

#include <cassert>
#include <optional>

int main() {
    using namespace clearmic::audio;
    assert(is_valid_battery_percentage(0));
    assert(is_valid_battery_percentage(100));
    assert(!is_valid_battery_percentage(101));
    AudioDevice device;
    assert(device.capabilities.battery == std::nullopt);
    assert(device.capabilities.transmitter_battery == std::nullopt);
    assert(device.capabilities.receiver_battery == std::nullopt);
    return 0;
}
