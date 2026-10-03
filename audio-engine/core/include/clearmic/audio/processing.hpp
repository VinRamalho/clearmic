#pragma once

#include "clearmic/audio/pcm.hpp"

namespace clearmic::audio {

// Supports mono or stereo signed 16-bit PCM at 48 kHz. Other formats fail
// explicitly instead of being silently resampled or copied unchanged.
[[nodiscard]] PcmAudio suppress_noise(const PcmAudio& input);
// Returns interleaved PCM16 RMS normalized to the [-1, 1] full-scale range.
[[nodiscard]] double rms_normalized(const PcmAudio& input) noexcept;

} // namespace clearmic::audio
