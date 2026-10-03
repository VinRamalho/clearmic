#include "clearmic/audio/processor_chain.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::array<std::int16_t, clearmic::audio::NoiseSuppressor::frame_samples> process_constant_frame(
    clearmic::audio::ProcessorChain& processor, const std::int16_t value) {
    std::array<std::int16_t, clearmic::audio::NoiseSuppressor::frame_samples> input{};
    std::array<std::int16_t, clearmic::audio::NoiseSuppressor::frame_samples> output{};
    input.fill(value);
    processor.process(input, output);
    return output;
}
}

void run_processor_chain_tests() {
    using namespace clearmic::audio;
    const auto natural = settings_for_preset(Preset::natural);
    const auto meeting = settings_for_preset(Preset::meeting);
    const auto strong = settings_for_preset(Preset::strong_noise_reduction);
    require(natural.noise_suppression_enabled && !natural.noise_gate_enabled && !natural.compressor_enabled,
            "Natural preset should preserve dynamics while suppressing noise");
    require(meeting.noise_gate_enabled && meeting.automatic_gain_enabled && meeting.compressor_enabled,
            "Meeting preset should enable its processing stages");
    require(strong.noise_gate_enabled && strong.gate_threshold_db > meeting.gate_threshold_db,
            "Strong reduction preset should use a stronger gate threshold");

    ProcessorChain settings_probe(1, meeting);
    require(settings_probe.settings().compressor_enabled && settings_probe.settings().input_gain_db == 2.0F,
            "Processor should retain preset controls");
    settings_probe.set_settings(natural);
    require(!settings_probe.settings().compressor_enabled && settings_probe.settings().input_gain_db == 0.0F,
            "Processor controls should update without rebuilding the chain");

    auto settings = natural;
    settings.noise_suppression_enabled = false;
    ProcessorChain processor(1, settings);
    constexpr std::size_t frame = NoiseSuppressor::frame_samples;
    std::array<std::int16_t, 160> input_a{};
    std::array<std::int16_t, 160> output_a{};
    std::array<std::int16_t, 170> input_b{};
    std::array<std::int16_t, 170> output_b{};
    std::array<std::int16_t, 150> input_c{};
    std::array<std::int16_t, 150> output_c{};
    input_a.fill(1000);
    input_b.fill(2000);
    input_c.fill(3000);
    processor.process(input_a, output_a);
    processor.process(input_b, output_b);
    processor.process(input_c, output_c);
    require(std::all_of(output_a.begin(), output_a.end(), [](const auto value) { return value == 0; }),
            "Processor should expose the documented initial 10 ms latency");
    require(std::all_of(output_b.begin(), output_b.end(), [](const auto value) { return value == 0; }) &&
            std::all_of(output_c.begin(), output_c.end(), [](const auto value) { return value == 0; }),
            "Processor should not emit an incomplete frame");

    std::array<std::int16_t, 160> next_input{};
    std::array<std::int16_t, 160> next_output{};
    next_input.fill(4000);
    processor.process(next_input, next_output);
    require(std::all_of(next_output.begin(), next_output.end(), [](const auto value) { return value == 1000; }),
            "Processor should preserve order and content across irregular callback sizes");

    settings.enhancement_enabled = false;
    processor.set_settings(settings);
    std::array<std::int16_t, frame - 160> bypass_input{};
    std::array<std::int16_t, frame - 160> bypass_output{};
    bypass_input.fill(5000);
    processor.process(bypass_input, bypass_output);
    std::array<std::int16_t, frame> next_frame{};
    std::array<std::int16_t, frame> next_frame_output{};
    next_frame.fill(6000);
    processor.process(next_frame, next_frame_output);
    require(std::all_of(next_frame_output.begin(), next_frame_output.begin() + 160,
                        [](const auto value) { return value == 4000; }) &&
            std::all_of(next_frame_output.begin() + 160, next_frame_output.end(),
                        [](const auto value) { return value == 5000; }),
            "Disabling enhancement should bypass DSP while preserving pipeline latency");

    auto synthetic_settings = natural;
    synthetic_settings.noise_suppression_enabled = false;
    synthetic_settings.input_gain_db = 6.0F;
    ProcessorChain gain_processor(1, synthetic_settings);
    (void)process_constant_frame(gain_processor, 1000); // Fills the initial latency frame.
    const auto gained = process_constant_frame(gain_processor, 1000);
    require(gained.back() >= 1950 && gained.back() <= 2050,
            "Input gain should reach the configured +6 dB level on a steady synthetic signal");

    synthetic_settings.input_gain_db = 0.0F;
    synthetic_settings.noise_gate_enabled = true;
    synthetic_settings.gate_threshold_db = -30.0F;
    ProcessorChain gate_processor(1, synthetic_settings);
    (void)process_constant_frame(gate_processor, 500);
    const auto gated = process_constant_frame(gate_processor, 500);
    require(std::all_of(gated.begin(), gated.end(), [](const auto value) { return value == 0; }),
            "Noise gate should mute a steady synthetic signal below its configured threshold");

    synthetic_settings.noise_gate_enabled = false;
    synthetic_settings.compressor_enabled = true;
    synthetic_settings.compressor_threshold_db = -18.0F;
    ProcessorChain compressor_processor(1, synthetic_settings);
    (void)process_constant_frame(compressor_processor, 16000);
    const auto compressed = process_constant_frame(compressor_processor, 16000);
    require(compressed.back() > 7000 && compressed.back() < 8000,
            "Compressor should reduce a steady synthetic signal above its threshold");

    synthetic_settings.compressor_enabled = false;
    synthetic_settings.automatic_gain_enabled = true;
    ProcessorChain agc_processor(1, synthetic_settings);
    (void)process_constant_frame(agc_processor, 1000);
    const auto leveled = process_constant_frame(agc_processor, 1000);
    require(leveled.back() >= 7900 && leveled.back() <= 8100,
            "Automatic gain should raise a steady synthetic signal toward its configured RMS target");
}
