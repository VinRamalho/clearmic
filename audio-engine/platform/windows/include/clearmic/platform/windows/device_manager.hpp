#pragma once

#include "clearmic/audio/device.hpp"

namespace clearmic::platform::windows {
class DeviceManager final : public audio::IAudioDeviceManager {
public:
    [[nodiscard]] std::vector<audio::AudioDevice> input_devices() override;
};
}
