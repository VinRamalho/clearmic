#include "clearmic/platform/windows/device_manager.hpp"

#include "clearmic/audio/processor_chain.hpp"

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace clearmic::platform::windows {
namespace {
constexpr GUID pcm_subtype{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID ieee_float_subtype{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <typename T> struct ComRelease { void operator()(T* value) const noexcept { if (value) value->Release(); } };
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease<T>>;

void check_hresult(const HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}

std::wstring to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 Windows audio endpoint ID");
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            output.data(), size) != size)
        throw std::runtime_error("Could not decode Windows audio endpoint ID");
    return output;
}

struct AudioFormat {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::uint16_t bits_per_sample{};
    std::uint16_t block_align{};
    bool floating_point{};

    [[nodiscard]] std::size_t bytes_per_frame() const noexcept { return block_align; }
};

AudioFormat parse_format(const WAVEFORMATEX& format) {
    AudioFormat result{format.nSamplesPerSec, format.nChannels, format.wBitsPerSample, format.nBlockAlign, false};
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            throw std::runtime_error("Windows microphone returned an incomplete extensible audio format");
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (IsEqualGUID(extended.SubFormat, ieee_float_subtype)) result.floating_point = true;
        else if (!IsEqualGUID(extended.SubFormat, pcm_subtype))
            throw std::runtime_error("Unsupported Windows microphone encoding");
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        result.floating_point = true;
    } else if (format.wFormatTag != WAVE_FORMAT_PCM) {
        throw std::runtime_error("Unsupported Windows microphone encoding");
    }
    if (result.sample_rate < 8000 || result.sample_rate > 192000 || result.block_align == 0 ||
        (result.channels != 1 && result.channels != 2) ||
        (result.floating_point ? result.bits_per_sample != 32 : result.bits_per_sample != 16) ||
        result.block_align != result.channels * (result.bits_per_sample / 8U))
        throw std::runtime_error("Windows microphone must use mono/stereo 16-bit PCM or 32-bit float at 8–192 kHz");
    return result;
}

float read_sample(const BYTE* frame, const AudioFormat& format, const std::uint16_t channel) noexcept {
    const auto* sample = frame + channel * (format.bits_per_sample / 8U);
    if (format.floating_point) {
        float value{};
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    std::int16_t value{};
    std::memcpy(&value, sample, sizeof(value));
    return static_cast<float>(value) / 32768.0F;
}

audio::PcmAudio process_for_comparison(audio::PcmAudio original, const audio::ProcessingSettings settings) {
    audio::PcmAudio processed{original.sample_rate_hz, original.channels, {}};
    processed.samples.resize(original.samples.size() + audio::NoiseSuppressor::frame_samples);
    audio::ProcessorChain processor(original.channels, settings);
    auto output = std::span<std::int16_t>(processed.samples);
    processor.process(original.samples, output.first(original.samples.size()));
    std::vector<std::int16_t> silence(audio::NoiseSuppressor::frame_samples, 0);
    processor.process(silence, output.last(silence.size()));
    std::move(processed.samples.begin() + static_cast<std::ptrdiff_t>(silence.size()),
              processed.samples.end(), processed.samples.begin());
    processed.samples.resize(original.samples.size());
    return processed;
}
}

audio::AudioComparison capture_processed_audio(const std::string& device_id,
                                               const std::uint32_t duration_seconds,
                                               const audio::ProcessingSettings settings) {
    if (duration_seconds == 0 || duration_seconds > 30)
        throw std::invalid_argument("Capture duration must be between 1 and 30 seconds");
    const auto devices = DeviceManager{}.input_devices();
    if (devices.empty()) throw std::runtime_error("Windows reports no active microphone endpoint");
    if (!device_id.empty() && std::none_of(devices.begin(), devices.end(),
            [&](const auto& device) { return device.id == device_id; }))
        throw std::runtime_error("The selected Windows microphone is no longer available");

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) check_hresult(initialized, "Initialize Windows audio COM");
    struct ApartmentGuard { bool active; ~ApartmentGuard() { if (active) CoUninitialize(); } } apartment{SUCCEEDED(initialized)};

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator)),
                  "Create Windows audio device enumerator");
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);

    IMMDevice* raw_device = nullptr;
    if (device_id.empty()) {
        check_hresult(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &raw_device),
                      "Find the default Windows microphone");
    } else {
        const auto wide_id = to_wide(device_id);
        check_hresult(enumerator->GetDevice(wide_id.c_str(), &raw_device), "Open the selected Windows microphone");
    }
    ComPtr<IMMDevice> device(raw_device);

    IAudioClient* raw_client = nullptr;
    check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&raw_client)), "Activate Windows microphone capture");
    ComPtr<IAudioClient> client(raw_client);

    WAVEFORMATEX* raw_mix_format = nullptr;
    check_hresult(client->GetMixFormat(&raw_mix_format), "Read Windows microphone format");
    struct FormatGuard { WAVEFORMATEX* value; ~FormatGuard() { CoTaskMemFree(value); } } format_guard{raw_mix_format};
    const auto format = parse_format(*raw_mix_format);
    const auto source_frame_count = static_cast<std::size_t>(format.sample_rate) * duration_seconds;
    if (source_frame_count > 192000U * 30U)
        throw std::runtime_error("Windows microphone recording exceeds the 30 second limit");
    std::vector<float> mono_source(source_frame_count, 0.0F);

    constexpr REFERENCE_TIME buffer_duration = 1000000;
    check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    buffer_duration, 0, raw_mix_format, nullptr),
                  "Initialize shared Windows microphone capture");
    HANDLE capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!capture_event) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create microphone capture event");
    struct EventGuard { HANDLE value; ~EventGuard() { if (value) CloseHandle(value); } } event_guard{capture_event};
    check_hresult(client->SetEventHandle(capture_event), "Set Windows microphone capture event");

    IAudioCaptureClient* raw_capture = nullptr;
    check_hresult(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&raw_capture)),
                  "Open Windows microphone capture buffer");
    ComPtr<IAudioCaptureClient> capture(raw_capture);
    check_hresult(client->Start(), "Start Windows microphone capture");
    struct CaptureGuard { IAudioClient* value; ~CaptureGuard() { if (value) value->Stop(); } } capture_guard{client.get()};

    std::size_t captured_frames = 0;
    unsigned int missed_events = 0;
    while (captured_frames < source_frame_count) {
        const auto wait_result = WaitForSingleObject(capture_event, 1000);
        if (wait_result == WAIT_TIMEOUT) {
            if (++missed_events >= 5) throw std::runtime_error("No microphone audio arrived for five seconds");
            continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
            if (wait_result == WAIT_FAILED)
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for Windows microphone audio");
            throw std::runtime_error("Windows microphone wait returned an unexpected status");
        }
        missed_events = 0;
        UINT32 packet_frames = 0;
        check_hresult(capture->GetNextPacketSize(&packet_frames), "Read Windows microphone packet size");
        while (packet_frames != 0 && captured_frames < source_frame_count) {
            BYTE* data = nullptr;
            UINT32 frame_count = 0;
            DWORD flags = 0;
            const HRESULT packet_result = capture->GetBuffer(&data, &frame_count, &flags, nullptr, nullptr);
            if (packet_result == AUDCLNT_E_DEVICE_INVALIDATED)
                throw std::runtime_error("The Windows microphone was disconnected during capture");
            check_hresult(packet_result, "Read Windows microphone audio packet");
            const auto take = std::min<std::size_t>(frame_count, source_frame_count - captured_frames);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                for (std::size_t frame = 0; frame < take; ++frame) {
                    const auto* source = data + frame * format.bytes_per_frame();
                    float sample = read_sample(source, format, 0);
                    if (format.channels == 2) sample = (sample + read_sample(source, format, 1)) * 0.5F;
                    mono_source[captured_frames + frame] = sample;
                }
            }
            captured_frames += take;
            check_hresult(capture->ReleaseBuffer(frame_count), "Release Windows microphone packet");
            check_hresult(capture->GetNextPacketSize(&packet_frames), "Read next Windows microphone packet size");
        }
    }
    check_hresult(client->Stop(), "Stop Windows microphone capture");
    capture_guard.value = nullptr;

    constexpr std::uint32_t target_rate = 48000;
    const auto target_frames = static_cast<std::size_t>(duration_seconds) * target_rate;
    audio::PcmAudio original{target_rate, 1, std::vector<std::int16_t>(target_frames)};
    for (std::size_t frame = 0; frame < target_frames; ++frame) {
        const double source_position = static_cast<double>(frame) * format.sample_rate / target_rate;
        const auto source_index = std::min(static_cast<std::size_t>(source_position), source_frame_count - 1);
        const auto next_index = std::min(source_index + 1, source_frame_count - 1);
        const auto fraction = static_cast<float>(source_position - static_cast<double>(source_index));
        const auto value = std::lerp(mono_source[source_index], mono_source[next_index], fraction) * 32768.0F;
        original.samples[frame] = static_cast<std::int16_t>(std::lrint(std::clamp(value, -32768.0F, 32767.0F)));
    }
    auto processed = process_for_comparison(original, settings);
    return audio::AudioComparison{std::move(original), std::move(processed)};
}
}
