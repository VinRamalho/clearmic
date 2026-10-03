#include "clearmic/audio/noise_suppressor.hpp"
#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/wav.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) {
        const std::string detail = "ClearMic test failure: " + std::string(message);
        const bool in_github_actions = std::getenv("GITHUB_ACTIONS") != nullptr;
        if (in_github_actions) std::cout << "::error::" << detail << '\n';
        throw std::runtime_error(detail);
    }
}

clearmic::audio::PcmAudio make_audio(const std::uint32_t sample_rate, const std::uint16_t channels,
                                     const std::size_t frame_count) {
    clearmic::audio::PcmAudio audio{sample_rate, channels, {}};
    audio.samples.resize(frame_count * channels);
    std::uint32_t noise = 0x12345678U;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        noise = noise * 1664525U + 1013904223U;
        const auto random_noise = static_cast<std::int32_t>((noise >> 16U) & 0x1fffU) - 4096;
        const auto time = static_cast<double>(frame) / sample_rate;
        const auto voice_tone = static_cast<std::int32_t>(std::sin(2.0 * 3.141592653589793 * 175.0 * time) * 10000.0);
        const auto sample = static_cast<std::int16_t>(voice_tone + random_noise);
        for (std::size_t channel = 0; channel < channels; ++channel)
            audio.samples[frame * channels + channel] = sample;
    }
    return audio;
}
}

void run_wav_tests() {
    using namespace clearmic::audio;
    const auto root = std::filesystem::temp_directory_path() / "clearmic-audio-tests";
    std::filesystem::create_directories(root);
    const auto mono_path = root / "mono.wav";
    const auto processed_path = root / "processed.wav";
    const auto unsupported_path = root / "unsupported-rate.wav";
    const auto unsupported_bits_path = root / "unsupported-bits.wav";
    const auto malformed_path = root / "malformed.wav";

    const auto original = make_audio(48000, 1, NoiseSuppressor::frame_samples * 4 + 137);
    write_pcm16_wav(mono_path, original);
    const auto loaded = read_pcm16_wav(mono_path);
    require(loaded.sample_rate_hz == original.sample_rate_hz, "WAV sample rate changed on round trip");
    require(loaded.channels == original.channels, "WAV channel count changed on round trip");
    require(loaded.samples == original.samples, "WAV PCM samples changed on round trip");

    const auto processed = suppress_noise(loaded);
    require(processed.samples.size() == original.samples.size(), "Noise suppression changed the sample count");
    require(processed.sample_rate_hz == original.sample_rate_hz, "Noise suppression changed the sample rate");
    bool changed = false;
    for (std::size_t index = 0; index < processed.samples.size(); ++index)
        if (processed.samples[index] != original.samples[index]) { changed = true; break; }
    require(changed, "RNNoise output unexpectedly matches every input sample");
    write_pcm16_wav(processed_path, processed);
    require(read_pcm16_wav(processed_path).samples == processed.samples, "Processed WAV could not be read back");

    const auto stereo = make_audio(48000, 2, 991);
    const auto stereo_path = root / "stereo.wav";
    write_pcm16_wav(stereo_path, stereo);
    require(read_pcm16_wav(stereo_path).samples == stereo.samples, "Stereo WAV round trip failed");
    const auto stereo_processed = suppress_noise(stereo);
    require(stereo_processed.samples.size() == stereo.samples.size(), "Stereo processing changed the sample count");

    write_pcm16_wav(unsupported_path, make_audio(44100, 1, 900));
    bool rejected_rate = false;
    try { static_cast<void>(suppress_noise(read_pcm16_wav(unsupported_path))); }
    catch (const std::invalid_argument&) { rejected_rate = true; }
    require(rejected_rate, "Unsupported processing sample rate was not rejected");

    write_pcm16_wav(unsupported_bits_path, make_audio(48000, 1, 900));
    {
        std::fstream unsupported_bits(unsupported_bits_path, std::ios::binary | std::ios::in | std::ios::out);
        unsupported_bits.seekp(34);
        unsupported_bits.put(static_cast<char>(8));
        unsupported_bits.put(static_cast<char>(0));
    }
    bool rejected_encoding = false;
    try { static_cast<void>(read_pcm16_wav(unsupported_bits_path)); }
    catch (const std::runtime_error&) { rejected_encoding = true; }
    require(rejected_encoding, "Unsupported WAV encoding was not rejected");

    {
        std::ofstream malformed(malformed_path, std::ios::binary);
        malformed.write("RIFF", 4);
    }
    bool rejected_malformed = false;
    try { static_cast<void>(read_pcm16_wav(malformed_path)); }
    catch (const std::runtime_error&) { rejected_malformed = true; }
    require(rejected_malformed, "Malformed WAV was not rejected");

    std::filesystem::remove_all(root);
}
