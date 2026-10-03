#pragma once

#include "clearmic/audio/noise_suppressor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace clearmic::audio {

enum class Preset { natural, meeting, strong_noise_reduction };

struct ProcessingSettings {
    bool enhancement_enabled{true};
    bool noise_suppression_enabled{true};
    bool noise_gate_enabled{false};
    bool automatic_gain_enabled{false};
    bool compressor_enabled{false};
    float input_gain_db{0.0F};
    float gate_threshold_db{-48.0F};
    float compressor_threshold_db{-18.0F};
};

[[nodiscard]] ProcessingSettings settings_for_preset(Preset preset) noexcept;

// Stateful processor for interleaved signed PCM16 at 48 kHz. Construct it
// before starting an audio callback; process() does not allocate or block.
class ProcessorChain {
public:
    explicit ProcessorChain(std::uint16_t channels, ProcessingSettings settings = {});
    void set_settings(ProcessingSettings settings) noexcept { settings_ = settings; }
    [[nodiscard]] const ProcessingSettings& settings() const noexcept { return settings_; }

    // Accept arbitrary whole interleaved sample counts. RNNoise frames may
    // span calls; initial latency is filled with silence until one 10 ms frame
    // is ready, and pending output is drained before subsequent input.
    void process(std::span<const std::int16_t> input, std::span<std::int16_t> output) noexcept;

private:
    void process_frame() noexcept;

    std::uint16_t channels_;
    ProcessingSettings settings_;
    NoiseSuppressor noise_suppressor_;
    std::vector<std::int16_t> input_frame_;
    std::vector<std::int16_t> output_frame_;
    std::size_t input_samples_{};
    std::size_t output_offset_{};
    float gain_linear_{1.0F};
};

} // namespace clearmic::audio
