#include "clearmic/audio/device.hpp"

namespace clearmic::audio {
bool is_valid_battery_percentage(const unsigned int percentage) noexcept { return percentage <= 100; }
} // namespace clearmic::audio
