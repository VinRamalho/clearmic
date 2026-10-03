#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>

namespace clearmic::audio {

class NoiseSuppressor {
public:
    static constexpr std::size_t frame_samples = 480;

    explicit NoiseSuppressor(std::uint16_t channels);
    ~NoiseSuppressor();
    NoiseSuppressor(NoiseSuppressor&&) noexcept;
    NoiseSuppressor& operator=(NoiseSuppressor&&) noexcept;
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // RNNoise consumes one 10 ms, 48 kHz frame per channel. Its output has a
    // one-frame algorithmic delay; continuous callers should preserve state.
    void process(std::span<const std::int16_t> input, std::span<std::int16_t> output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clearmic::audio
