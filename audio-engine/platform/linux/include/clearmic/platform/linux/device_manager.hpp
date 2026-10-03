#pragma once

#include "clearmic/audio/device.hpp"

namespace clearmic::platform::pipewire {
class DeviceManager final : public audio::IAudioDeviceManager {
public:
    [[nodiscard]] std::vector<audio::AudioDevice> input_devices() override;
};
}
