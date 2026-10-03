#include "clearmic/audio/processing.hpp"

#include "clearmic/audio/noise_suppressor.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace clearmic::audio {

PcmAudio suppress_noise(const PcmAudio& input) {
    if (input.sample_rate_hz != 48000)
        throw std::invalid_argument("Noise suppression currently requires 48000 Hz audio");
    if (input.channels != 1 && input.channels != 2)
        throw std::invalid_argument("Noise suppression currently requires mono or stereo audio");
    if (input.samples.size() % input.channels != 0)
        throw std::invalid_argument("Input audio has an incomplete sample frame");
    if (input.samples.empty()) return input;

    NoiseSuppressor processor(input.channels);
    const auto channel_count = static_cast<std::size_t>(input.channels);
    const auto input_frames = input.frame_count();
    const auto processing_frames = (input_frames + NoiseSuppressor::frame_samples - 1) / NoiseSuppressor::frame_samples;
    const auto block_size = NoiseSuppressor::frame_samples * channel_count;
    std::vector<std::int16_t> source_block(block_size, 0);
    std::vector<std::int16_t> output_block(block_size, 0);
    std::vector<std::int16_t> processed;
    processed.reserve(processing_frames * block_size);

    for (std::size_t frame = 0; frame < processing_frames; ++frame) {
        std::fill(source_block.begin(), source_block.end(), 0);
        const auto sample_offset = frame * block_size;
        const auto available = std::min(block_size, input.samples.size() - sample_offset);
        std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(sample_offset), available, source_block.begin());
        processor.process(source_block, output_block);
        if (frame > 0) processed.insert(processed.end(), output_block.begin(), output_block.end());
    }

    // RNNoise returns the previous frame; discard its startup frame and flush
    // the final padded input frame without adding another full output frame.
    if (processing_frames > 0) {
        std::fill(source_block.begin(), source_block.end(), 0);
        processor.process(source_block, output_block);
        processed.insert(processed.end(), output_block.begin(), output_block.end());
    }
    processed.resize(input.samples.size());
    return PcmAudio{input.sample_rate_hz, input.channels, std::move(processed)};
}

} // namespace clearmic::audio
