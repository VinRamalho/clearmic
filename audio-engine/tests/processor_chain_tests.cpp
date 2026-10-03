#include "clearmic/audio/processor_chain.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
    require(std::all_of(next_frame_output.begin(), next_frame_output.end(), [](const auto value) { return value == 5000; }),
            "Disabling enhancement should bypass DSP while preserving pipeline latency");
}
