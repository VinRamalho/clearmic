#pragma once

#include "clearmic/audio/device.hpp"
#include "clearmic/audio/pcm.hpp"
#include "clearmic/audio/processor_chain.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace clearmic::platform::windows {
class DeviceManager final : public audio::IAudioDeviceManager {
public:
    [[nodiscard]] std::vector<audio::AudioDevice> input_devices() override;
};

struct CaptureDiagnostics {
    std::optional<std::uint32_t> buffer_frames;
    std::optional<double> stream_latency_ms;
    std::optional<double> processing_wall_time_ms;
};

[[nodiscard]] audio::AudioComparison capture_processed_audio(const std::string& device_id,
                                                             std::uint32_t duration_seconds,
                                                             audio::ProcessingSettings settings = {},
                                                             CaptureDiagnostics* diagnostics = nullptr);
int run_desktop_application();
}
