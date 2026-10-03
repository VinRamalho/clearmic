#pragma once

#include "clearmic/audio/device.hpp"
#include "clearmic/audio/pcm.hpp"
#include "clearmic/audio/processor_chain.hpp"

#include <cstdint>
#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace clearmic::platform::windows {
class DeviceManager final : public audio::IAudioDeviceManager {
public:
    [[nodiscard]] std::vector<audio::AudioDevice> input_devices() override;
    [[nodiscard]] std::vector<audio::AudioDevice> output_devices();
};

struct CaptureDiagnostics {
    std::optional<std::uint32_t> buffer_frames;
    std::optional<double> stream_latency_ms;
    std::optional<double> processing_wall_time_ms;
};

struct LiveProcessingMetrics {
    std::atomic<float> input_rms{};
    std::atomic<float> output_rms{};
};

[[nodiscard]] audio::AudioComparison capture_processed_audio(const std::string& device_id,
                                                             std::uint32_t duration_seconds,
                                                             audio::ProcessingSettings settings = {},
                                                             CaptureDiagnostics* diagnostics = nullptr);
void run_live_processing(const std::string& input_device_id, const std::string& output_device_id,
                         audio::ProcessingSettings settings, const std::atomic_bool& stop_requested,
                         LiveProcessingMetrics& metrics, const std::function<void()>& on_started = {});
int run_desktop_application();
}
