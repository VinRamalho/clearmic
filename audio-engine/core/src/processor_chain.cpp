#include "clearmic/audio/processor_chain.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace clearmic::audio {
namespace {
constexpr float minimum_gain = 0.01F;
constexpr float maximum_gain = 16.0F;

float db_to_linear(const float db) noexcept { return std::pow(10.0F, db / 20.0F); }

std::int16_t clip_sample(const float value) noexcept {
    return static_cast<std::int16_t>(std::lrint(std::clamp(value, -32768.0F, 32767.0F)));
}
}

ProcessingSettings settings_for_preset(const Preset preset) noexcept {
    switch (preset) {
    case Preset::natural:
        return ProcessingSettings{.enhancement_enabled = true, .noise_suppression_enabled = true,
                                  .noise_gate_enabled = false, .automatic_gain_enabled = false,
                                  .compressor_enabled = false, .input_gain_db = 0.0F};
    case Preset::meeting:
        return ProcessingSettings{.enhancement_enabled = true, .noise_suppression_enabled = true,
                                  .noise_gate_enabled = true, .automatic_gain_enabled = true,
                                  .compressor_enabled = true, .input_gain_db = 2.0F,
                                  .gate_threshold_db = -52.0F, .compressor_threshold_db = -20.0F};
    case Preset::strong_noise_reduction:
        return ProcessingSettings{.enhancement_enabled = true, .noise_suppression_enabled = true,
                                  .noise_gate_enabled = true, .automatic_gain_enabled = false,
                                  .compressor_enabled = false, .input_gain_db = 0.0F,
                                  .gate_threshold_db = -42.0F};
    }
    return {};
}

ProcessorChain::ProcessorChain(const std::uint16_t channels, ProcessingSettings settings)
    : channels_(channels), settings_(settings), noise_suppressor_(channels),
      input_frame_(NoiseSuppressor::frame_samples * channels),
      output_frame_(NoiseSuppressor::frame_samples * channels) {
    output_offset_ = output_frame_.size();
}

void ProcessorChain::process_frame() noexcept {
    if (!settings_.enhancement_enabled) {
        std::copy(input_frame_.begin(), input_frame_.end(), output_frame_.begin());
        input_samples_ = 0;
        output_offset_ = 0;
        return;
    }
    if (settings_.noise_suppression_enabled)
        noise_suppressor_.process(input_frame_, output_frame_);
    else
        std::copy(input_frame_.begin(), input_frame_.end(), output_frame_.begin());

    double square_sum = 0.0;
    for (const auto sample : output_frame_) {
        const auto value = static_cast<double>(sample);
        square_sum += value * value;
    }
    const auto rms = static_cast<float>(std::sqrt(square_sum / static_cast<double>(output_frame_.size())));
    const auto automatic_gain_db = settings_.automatic_gain_enabled && rms > 1.0F
        ? 20.0F * std::log10(8000.0F / rms)
        : 0.0F;
    const auto requested_gain = std::clamp(db_to_linear(settings_.input_gain_db + automatic_gain_db),
                                           minimum_gain, maximum_gain);
    // Smooth gain changes to avoid clicks when a setting or preset changes.
    constexpr float gain_smoothing = 0.02F;
    const auto samples_per_channel = output_frame_.size();
    for (std::size_t index = 0; index < samples_per_channel; ++index) {
        gain_linear_ += (requested_gain - gain_linear_) * gain_smoothing;
        float sample = static_cast<float>(output_frame_[index]) * gain_linear_;
        const auto magnitude = std::abs(sample);
        if (settings_.compressor_enabled) {
            const auto threshold = db_to_linear(settings_.compressor_threshold_db) * 32768.0F;
            if (magnitude > threshold) sample = std::copysign(threshold + (magnitude - threshold) * 0.25F, sample);
        }
        if (settings_.noise_gate_enabled) {
            const auto threshold = db_to_linear(settings_.gate_threshold_db) * 32768.0F;
            if (magnitude < threshold) sample = 0.0F;
        }
        output_frame_[index] = clip_sample(sample);
    }
    input_samples_ = 0;
    output_offset_ = 0;
}

void ProcessorChain::process(const std::span<const std::int16_t> input,
                             const std::span<std::int16_t> output) noexcept {
    const auto expected_samples = NoiseSuppressor::frame_samples * channels_;
    if (input.size() != output.size()) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (output_offset_ == output_frame_.size()) {
            input_frame_[input_samples_++] = input[index];
            output[index] = 0;
            if (input_samples_ == expected_samples) process_frame();
            continue;
        }

        output[index] = output_frame_[output_offset_++];
        input_frame_[input_samples_++] = input[index];
        if (input_samples_ == expected_samples) process_frame();
    }
}

} // namespace clearmic::audio
