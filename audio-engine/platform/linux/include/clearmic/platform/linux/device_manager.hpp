#pragma once

#include "clearmic/audio/device.hpp"
#include "clearmic/audio/pcm.hpp"
#include "clearmic/audio/processor_chain.hpp"

#include <cstdint>
#include <string>

namespace clearmic::platform::pipewire {
class DeviceManager final : public audio::IAudioDeviceManager {
public:
    [[nodiscard]] std::vector<audio::AudioDevice> input_devices() override;
};

[[nodiscard]] audio::AudioComparison capture_processed_audio(const std::string& device_id,
                                                             std::uint32_t duration_seconds,
                                                             audio::ProcessingSettings settings = {});
void run_realtime_microphone(const std::string& device_id, audio::ProcessingSettings settings = {});
int run_desktop_application(const char* executable_path);
}
