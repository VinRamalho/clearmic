#pragma once

#include "clearmic/audio/pcm.hpp"

#include <filesystem>

namespace clearmic::audio {

[[nodiscard]] PcmAudio read_pcm16_wav(const std::filesystem::path& path);
void write_pcm16_wav(const std::filesystem::path& path, const PcmAudio& audio);

} // namespace clearmic::audio
