#include "clearmic/audio/noise_suppressor.hpp"
#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/wav.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstdint>
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

void write_u16(std::ofstream& output, const std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_u32(std::ofstream& output, const std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8)
        output.put(static_cast<char>((value >> shift) & 0xffU));
}

void write_chunk_header(std::ofstream& output, const char (&id)[5], const std::uint32_t size) {
    output.write(id, 4);
    write_u32(output, size);
}

void write_pcm_format(std::ofstream& output, const std::uint16_t channels = 1,
                      const std::uint32_t sample_rate = 48000, const std::uint16_t bits = 16,
                      const std::uint16_t block_align = 2) {
    write_chunk_header(output, "fmt ", 16);
    write_u16(output, 1);
    write_u16(output, channels);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate * block_align);
    write_u16(output, block_align);
    write_u16(output, bits);
}

template <typename Writer>
void write_riff_fixture(const std::filesystem::path& path, const std::uint32_t riff_size, Writer writer) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    write_u32(output, riff_size);
    output.write("WAVE", 4);
    writer(output);
}

bool rejected_wav(const std::filesystem::path& path) {
    try { static_cast<void>(clearmic::audio::read_pcm16_wav(path)); }
    catch (const std::exception&) { return true; }
    return false;
}
}

void run_wav_tests() {
    using namespace clearmic::audio;
    require(rms_normalized(PcmAudio{}) == 0.0, "Empty PCM should report zero RMS");
    PcmAudio rms_probe{48000, 1, std::vector<std::int16_t>(100, 16384)};
    require(std::abs(rms_normalized(rms_probe) - 0.5) < 0.0001,
            "Normalized PCM RMS should use signed 16-bit full scale");

    const auto root = std::filesystem::temp_directory_path() / "clearmic-audio-tests";
    std::filesystem::create_directories(root);
    const auto mono_path = root / "mono.wav";
    const auto processed_path = root / "processed.wav";
    const auto unsupported_path = root / "unsupported-rate.wav";
    const auto unsupported_bits_path = root / "unsupported-bits.wav";
    const auto malformed_path = root / "malformed.wav";
    const auto truncated_chunk_path = root / "truncated-chunk.wav";
    const auto missing_padding_path = root / "missing-padding.wav";
    const auto incomplete_frame_path = root / "incomplete-frame.wav";
    const auto invalid_alignment_path = root / "invalid-alignment.wav";
    const auto invalid_sample_rate_path = root / "invalid-sample-rate.wav";
    const auto invalid_riff_path = root / "invalid-riff-size.wav";

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

    for (const auto sample_count : {std::size_t{137}, NoiseSuppressor::frame_samples,
                                    NoiseSuppressor::frame_samples + 1}) {
        const auto short_input = make_audio(48000, 1, sample_count);
        const auto short_processed = suppress_noise(short_input);
        require(short_processed.samples.size() == short_input.samples.size(),
                "Short WAV processing changed the sample count");
        if (sample_count == NoiseSuppressor::frame_samples) {
            require(std::any_of(short_processed.samples.begin(), short_processed.samples.end(),
                                [](const auto sample) { return sample != 0; }),
                    "Single-frame WAV processing returned only startup silence");
        }
    }

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

    write_riff_fixture(truncated_chunk_path, 24, [](std::ofstream& output) {
        write_chunk_header(output, "fmt ", 16);
        write_u16(output, 1);
    });
    require(rejected_wav(truncated_chunk_path), "Chunk payload extending past the RIFF container was not rejected");

    write_riff_fixture(missing_padding_path, 26, [](std::ofstream& output) {
        write_pcm_format(output);
        write_chunk_header(output, "JUNK", 1);
        output.put('x');
    });
    require(rejected_wav(missing_padding_path), "Missing padding after an odd-sized RIFF chunk was not rejected");

    write_riff_fixture(incomplete_frame_path, 40, [](std::ofstream& output) {
        write_pcm_format(output);
        write_chunk_header(output, "data", 3);
        output.write("abc", 3);
        output.put('\0');
    });
    require(rejected_wav(incomplete_frame_path), "PCM data ending mid-frame was not rejected");

    write_riff_fixture(invalid_alignment_path, 36, [](std::ofstream& output) {
        write_pcm_format(output, 2, 48000, 16, 2);
        write_chunk_header(output, "data", 0);
    });
    require(rejected_wav(invalid_alignment_path), "Inconsistent stereo block alignment was not rejected");

    write_riff_fixture(invalid_sample_rate_path, 36, [](std::ofstream& output) {
        write_pcm_format(output, 1, 0, 16, 2);
        write_chunk_header(output, "data", 0);
    });
    require(rejected_wav(invalid_sample_rate_path), "Zero WAV sample rate was not rejected");

    write_riff_fixture(invalid_riff_path, 3, [](std::ofstream&) {});
    require(rejected_wav(invalid_riff_path), "RIFF container shorter than its WAVE header was not rejected");

    std::filesystem::remove_all(root);
}
