#include "clearmic/audio/wav.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace clearmic::audio {
namespace {

constexpr std::uint32_t max_data_bytes = 512U * 1024U * 1024U;

std::uint16_t read_u16(const std::array<unsigned char, 16>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_u32(const std::array<unsigned char, 16>& bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void write_u16(std::ostream& output, const std::uint16_t value) {
    const char bytes[] = {static_cast<char>(value & 0xffU), static_cast<char>((value >> 8U) & 0xffU)};
    output.write(bytes, sizeof(bytes));
}

void write_u32(std::ostream& output, const std::uint32_t value) {
    const char bytes[] = {static_cast<char>(value & 0xffU), static_cast<char>((value >> 8U) & 0xffU),
                          static_cast<char>((value >> 16U) & 0xffU), static_cast<char>((value >> 24U) & 0xffU)};
    output.write(bytes, sizeof(bytes));
}

bool read_exact(std::istream& input, char* data, const std::streamsize count) {
    input.read(data, count);
    return input.gcount() == count;
}

void expect_id(const std::array<unsigned char, 16>& header, const std::size_t offset, const char* expected) {
    for (std::size_t index = 0; index < 4; ++index)
        if (header[offset + index] != static_cast<unsigned char>(expected[index]))
            throw std::runtime_error(std::string("Invalid WAV: expected ") + expected);
}

} // namespace

PcmAudio read_pcm16_wav(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open WAV file: " + path.string());

    std::array<unsigned char, 16> header{};
    if (!read_exact(input, reinterpret_cast<char*>(header.data()), 12))
        throw std::runtime_error("Invalid WAV: truncated RIFF header");
    expect_id(header, 0, "RIFF");
    expect_id(header, 8, "WAVE");
    const auto riff_size = read_u32(header, 4);
    if (riff_size < 4) throw std::runtime_error("Invalid WAV: RIFF container is too short");
    input.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::streamoff>(input.tellg());
    if (file_size < 0 || static_cast<std::uint64_t>(riff_size) + 8U > static_cast<std::uint64_t>(file_size))
        throw std::runtime_error("Invalid WAV: RIFF size exceeds the file length");
    const auto riff_end = static_cast<std::streamoff>(riff_size) + 8;
    input.seekg(12, std::ios::beg);

    bool have_format = false;
    bool have_data = false;
    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t block_align = 0;
    std::uint32_t sample_rate = 0;
    std::vector<unsigned char> data;

    while (input && static_cast<std::streamoff>(input.tellg()) < riff_end) {
        const auto chunk_start = static_cast<std::streamoff>(input.tellg());
        if (chunk_start < 0 || riff_end - chunk_start < 8)
            throw std::runtime_error("Invalid WAV: incomplete chunk header");
        input.read(reinterpret_cast<char*>(header.data()), 8);
        const auto chunk_header_size = input.gcount();
        if (chunk_header_size == 0) break;
        if (chunk_header_size != 8) throw std::runtime_error("Invalid WAV: truncated chunk header");
        const auto chunk_size = read_u32(header, 4);
        const auto padded_chunk_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1U);
        const auto payload_start = static_cast<std::streamoff>(input.tellg());
        if (payload_start < 0 || padded_chunk_size > static_cast<std::uint64_t>(riff_end - payload_start))
            throw std::runtime_error("Invalid WAV: chunk extends beyond the RIFF container");
        if (std::string(reinterpret_cast<const char*>(header.data()), 4) == "fmt ") {
            if (chunk_size < 16) throw std::runtime_error("Invalid WAV: format chunk is shorter than 16 bytes");
            if (!read_exact(input, reinterpret_cast<char*>(header.data()), 16))
                throw std::runtime_error("Invalid WAV: truncated format chunk");
            format_tag = read_u16(header, 0);
            channels = read_u16(header, 2);
            sample_rate = read_u32(header, 4);
            block_align = read_u16(header, 12);
            bits_per_sample = read_u16(header, 14);
            const auto remaining = static_cast<std::uint64_t>(chunk_size) - 16U;
            input.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
            if (!input) throw std::runtime_error("Invalid WAV: truncated extended format chunk");
            have_format = true;
        } else if (std::string(reinterpret_cast<const char*>(header.data()), 4) == "data") {
            if (chunk_size > max_data_bytes - data.size())
                throw std::runtime_error("WAV data is too large for a PCM RIFF file");
            const auto current_size = data.size();
            data.resize(current_size + chunk_size);
            if (chunk_size > 0 && !read_exact(input, reinterpret_cast<char*>(data.data() + current_size), chunk_size))
                throw std::runtime_error("Invalid WAV: truncated audio data");
            have_data = true;
        } else {
            input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
            if (!input) throw std::runtime_error("Invalid WAV: truncated chunk");
        }
        if ((chunk_size & 1U) != 0U) {
            input.seekg(1, std::ios::cur);
            if (!input) throw std::runtime_error("Invalid WAV: missing chunk padding");
        }
    }

    if (!have_format || !have_data) throw std::runtime_error("Invalid WAV: missing format or data chunk");
    if (format_tag != 1) throw std::runtime_error("Unsupported WAV format: only integer PCM is supported");
    if (bits_per_sample != 16) throw std::runtime_error("Unsupported WAV format: only 16-bit PCM is supported");
    if (channels != 1 && channels != 2) throw std::runtime_error("Unsupported WAV format: expected one or two channels");
    if (block_align != channels * sizeof(std::int16_t)) throw std::runtime_error("Invalid WAV: inconsistent block alignment");
    if (sample_rate == 0 || data.size() % block_align != 0) throw std::runtime_error("Invalid WAV: incomplete audio frame");

    PcmAudio audio{sample_rate, channels, {}};
    audio.samples.resize(data.size() / sizeof(std::int16_t));
    for (std::size_t index = 0; index < audio.samples.size(); ++index) {
        const auto value = static_cast<std::uint16_t>(data[index * 2]) |
                           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[index * 2 + 1]) << 8U);
        audio.samples[index] = value <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())
            ? static_cast<std::int16_t>(value)
            : static_cast<std::int16_t>(static_cast<std::int32_t>(value) - 0x10000);
    }
    return audio;
}

void write_pcm16_wav(const std::filesystem::path& path, const PcmAudio& audio) {
    if ((audio.channels != 1 && audio.channels != 2) || audio.sample_rate_hz == 0)
        throw std::runtime_error("Invalid PCM audio format");
    if (audio.samples.size() % audio.channels != 0)
        throw std::runtime_error("PCM samples do not contain complete frames");
    const auto data_size64 = static_cast<std::uint64_t>(audio.samples.size()) * sizeof(std::int16_t);
    if (data_size64 > max_data_bytes) throw std::runtime_error("PCM data is too large for a RIFF WAV file");
    const auto data_size = static_cast<std::uint32_t>(data_size64);
    const auto block_align = static_cast<std::uint16_t>(audio.channels * sizeof(std::int16_t));
    const auto byte_rate = static_cast<std::uint64_t>(audio.sample_rate_hz) * block_align;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("PCM format has an invalid byte rate");

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not create WAV file: " + path.string());
    output.write("RIFF", 4);
    write_u32(output, 36U + data_size);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, audio.channels);
    write_u32(output, audio.sample_rate_hz);
    write_u32(output, static_cast<std::uint32_t>(byte_rate));
    write_u16(output, block_align);
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, data_size);
    for (const auto sample : audio.samples) write_u16(output, static_cast<std::uint16_t>(sample));
    if (!output) throw std::runtime_error("Failed while writing WAV file: " + path.string());
}

} // namespace clearmic::audio
