#include "clearmic/audio/noise_suppressor.hpp"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace clearmic::audio {
namespace {
struct DenoiseStateDeleter {
    void operator()(DenoiseState* state) const noexcept { rnnoise_destroy(state); }
};
}

struct NoiseSuppressor::Impl {
    explicit Impl(const std::uint16_t channel_count) {
        if (channel_count != 1 && channel_count != 2)
            throw std::invalid_argument("RNNoise supports one or two independent channels");
        if (rnnoise_get_frame_size() != static_cast<int>(NoiseSuppressor::frame_samples))
            throw std::runtime_error("Unexpected RNNoise frame size");
        states.reserve(channel_count);
        for (std::uint16_t channel = 0; channel < channel_count; ++channel) {
            std::unique_ptr<DenoiseState, DenoiseStateDeleter> state(rnnoise_create(nullptr));
            if (!state) throw std::runtime_error("Could not initialize RNNoise");
            states.push_back(std::move(state));
        }
    }

    std::vector<std::unique_ptr<DenoiseState, DenoiseStateDeleter>> states;
};

NoiseSuppressor::NoiseSuppressor(const std::uint16_t channels) : impl_(std::make_unique<Impl>(channels)) {}
NoiseSuppressor::~NoiseSuppressor() = default;
NoiseSuppressor::NoiseSuppressor(NoiseSuppressor&&) noexcept = default;
NoiseSuppressor& NoiseSuppressor::operator=(NoiseSuppressor&&) noexcept = default;

void NoiseSuppressor::process(const std::span<const std::int16_t> input, const std::span<std::int16_t> output) {
    if (!impl_) throw std::logic_error("RNNoise processor has been moved from");
    const auto channels = impl_->states.size();
    const auto frame_size = frame_samples * channels;
    if (input.size() != output.size() || input.size() % frame_size != 0)
        throw std::invalid_argument("RNNoise input and output must contain matching complete 10 ms frames");

    float frame_input[frame_samples];
    float frame_output[frame_samples];
    const auto frame_count = input.size() / frame_size;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            for (std::size_t sample = 0; sample < frame_samples; ++sample)
                frame_input[sample] = input[frame * frame_size + sample * channels + channel];
            rnnoise_process_frame(impl_->states[channel].get(), frame_output, frame_input);
            for (std::size_t sample = 0; sample < frame_samples; ++sample) {
                const auto clipped = std::clamp(frame_output[sample], -32768.0F, 32767.0F);
                output[frame * frame_size + sample * channels + channel] = static_cast<std::int16_t>(std::lrint(clipped));
            }
        }
    }
}

} // namespace clearmic::audio
