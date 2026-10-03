#pragma once

#include "clearmic/audio/pcm.hpp"

namespace clearmic::audio {

// Supports mono or stereo signed 16-bit PCM at 48 kHz. Other formats fail
// explicitly instead of being silently resampled or copied unchanged.
[[nodiscard]] PcmAudio suppress_noise(const PcmAudio& input);

} // namespace clearmic::audio
