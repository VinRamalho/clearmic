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

Preset preset_from_name(const std::string_view name) noexcept {
    if (name == "meeting") return Preset::meeting;
    if (name == "strong") return Preset::strong_noise_reduction;
    return Preset::natural;
}

std::string_view preset_name(const Preset preset) noexcept {
    switch (preset) {
    case Preset::meeting: return "meeting";
    case Preset::strong_noise_reduction: return "strong";
    case Preset::natural: return "natural";
    }
    return "natural";
}

ProcessorChain::ProcessorChain(const std::uint16_t channels, ProcessingSettings settings)
    : channels_(channels), noise_suppressor_(channels),
      input_frame_(NoiseSuppressor::frame_samples * channels),
      output_frame_(NoiseSuppressor::frame_samples * channels) {
    set_settings(settings);
    output_offset_ = output_frame_.size();
}

void ProcessorChain::set_settings(const ProcessingSettings settings) noexcept {
    enhancement_enabled_.store(settings.enhancement_enabled, std::memory_order_relaxed);
    noise_suppression_enabled_.store(settings.noise_suppression_enabled, std::memory_order_relaxed);
    noise_gate_enabled_.store(settings.noise_gate_enabled, std::memory_order_relaxed);
    automatic_gain_enabled_.store(settings.automatic_gain_enabled, std::memory_order_relaxed);
    compressor_enabled_.store(settings.compressor_enabled, std::memory_order_relaxed);
    input_gain_db_.store(settings.input_gain_db, std::memory_order_relaxed);
    gate_threshold_db_.store(settings.gate_threshold_db, std::memory_order_relaxed);
    compressor_threshold_db_.store(settings.compressor_threshold_db, std::memory_order_relaxed);
}

ProcessingSettings ProcessorChain::settings() const noexcept {
    return ProcessingSettings{
        .enhancement_enabled = enhancement_enabled_.load(std::memory_order_relaxed),
        .noise_suppression_enabled = noise_suppression_enabled_.load(std::memory_order_relaxed),
        .noise_gate_enabled = noise_gate_enabled_.load(std::memory_order_relaxed),
        .automatic_gain_enabled = automatic_gain_enabled_.load(std::memory_order_relaxed),
        .compressor_enabled = compressor_enabled_.load(std::memory_order_relaxed),
        .input_gain_db = input_gain_db_.load(std::memory_order_relaxed),
        .gate_threshold_db = gate_threshold_db_.load(std::memory_order_relaxed),
        .compressor_threshold_db = compressor_threshold_db_.load(std::memory_order_relaxed)};
}

void ProcessorChain::process_frame() noexcept {
    const auto current_settings = settings();
    if (!current_settings.enhancement_enabled) {
        std::copy(input_frame_.begin(), input_frame_.end(), output_frame_.begin());
        input_samples_ = 0;
        output_offset_ = 0;
        return;
    }
    if (current_settings.noise_suppression_enabled)
        noise_suppressor_.process(input_frame_, output_frame_);
    else
        std::copy(input_frame_.begin(), input_frame_.end(), output_frame_.begin());

    double square_sum = 0.0;
    for (const auto sample : output_frame_) {
        const auto value = static_cast<double>(sample);
        square_sum += value * value;
    }
    const auto rms = static_cast<float>(std::sqrt(square_sum / static_cast<double>(output_frame_.size())));
    const auto automatic_gain_db = current_settings.automatic_gain_enabled && rms > 1.0F
        ? 20.0F * std::log10(8000.0F / rms)
        : 0.0F;
    const auto requested_gain = std::clamp(db_to_linear(current_settings.input_gain_db + automatic_gain_db),
                                           minimum_gain, maximum_gain);
    const auto compressor_threshold = current_settings.compressor_enabled
        ? db_to_linear(current_settings.compressor_threshold_db) * 32768.0F
        : 0.0F;
    constexpr float gate_open_hysteresis = 2.0F; // 6 dB above the close threshold.
    constexpr float gate_attack_smoothing = 0.004158F; // 5 ms at 48 kHz.
    constexpr float gate_release_smoothing = 0.000260F; // 80 ms at 48 kHz.
    float gate_target = 1.0F;
    if (current_settings.noise_gate_enabled) {
        const auto close_threshold = db_to_linear(current_settings.gate_threshold_db) * 32768.0F;
        if (gate_open_ && rms < close_threshold) gate_open_ = false;
        else if (!gate_open_ && rms >= close_threshold * gate_open_hysteresis) gate_open_ = true;
        gate_target = gate_open_ ? 1.0F : 0.0F;
    } else {
        gate_open_ = true;
        gate_gain_ = 1.0F;
    }
    // Smooth gain changes to avoid clicks when a setting or preset changes.
    constexpr float gain_attack_smoothing = 0.004158F; // 5 ms at 48 kHz: reduce gain promptly on loud input.
    constexpr float gain_release_smoothing = 0.000208F; // 100 ms at 48 kHz: recover gain without pumping.
    for (std::size_t index = 0; index < output_frame_.size(); index += channels_) {
        const auto gain_smoothing = requested_gain < gain_linear_
            ? gain_attack_smoothing : gain_release_smoothing;
        gain_linear_ += (requested_gain - gain_linear_) * gain_smoothing;
        if (current_settings.noise_gate_enabled)
            gate_gain_ += (gate_target - gate_gain_) *
                          (gate_target > gate_gain_ ? gate_attack_smoothing : gate_release_smoothing);
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto sample_index = index + channel;
            float sample = static_cast<float>(output_frame_[sample_index]) * gain_linear_;
            const auto magnitude = std::abs(sample);
            if (current_settings.compressor_enabled && magnitude > compressor_threshold)
                sample = std::copysign(compressor_threshold + (magnitude - compressor_threshold) * 0.25F, sample);
            output_frame_[sample_index] = clip_sample(sample * gate_gain_);
        }
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
