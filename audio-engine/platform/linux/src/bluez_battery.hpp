#pragma once

#include "clearmic/audio/device.hpp"

#include <vector>

namespace clearmic::platform::pipewire {
void populate_bluez_battery(std::vector<audio::AudioDevice>& devices) noexcept;
}
