#pragma once

#include <cstdint>
#include <vector>

namespace clearmic::audio {

struct PcmAudio {
    std::uint32_t sample_rate_hz{};
    std::uint16_t channels{};
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::size_t frame_count() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

} // namespace clearmic::audio
