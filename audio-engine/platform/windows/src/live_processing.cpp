#include "clearmic/platform/windows/device_manager.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace clearmic::platform::windows {
namespace {
template <typename T> struct ComRelease { void operator()(T* value) const noexcept { if (value) value->Release(); } };
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease<T>>;

void check_hresult(const HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}

class AudioEvent {
public:
    AudioEvent() : handle_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (!handle_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                              "Create WASAPI stream event");
    }
    ~AudioEvent() { if (handle_) CloseHandle(handle_); }
    AudioEvent(const AudioEvent&) = delete;
    AudioEvent& operator=(const AudioEvent&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_{};
};

class AudioRing {
public:
    bool push(std::span<const std::int16_t> input) noexcept {
        bool overflow = false;
        if (input.size() > samples_.size()) {
            input = input.last(samples_.size());
            read_ = write_;
            overflow = true;
        }
        if (write_ + input.size() - read_ > samples_.size()) {
            read_ = write_ + input.size() - samples_.size();
            overflow = true;
        }
        for (const auto value : input) samples_[static_cast<std::size_t>(write_++) & mask] = value;
        return overflow;
    }

    std::size_t pop(std::span<std::int16_t> output) noexcept {
        const auto count = std::min<std::uint64_t>(output.size(), write_ - read_);
        for (std::size_t index = 0; index < count; ++index)
            output[index] = samples_[static_cast<std::size_t>(read_++) & mask];
        std::fill(output.begin() + static_cast<std::ptrdiff_t>(count), output.end(), 0);
        return static_cast<std::size_t>(count);
    }
private:
    static constexpr std::size_t capacity = 4096;
    static constexpr std::size_t mask = capacity - 1;
    static_assert((capacity & mask) == 0);
    std::array<std::int16_t, capacity> samples_{};
    std::uint64_t read_{};
    std::uint64_t write_{};
};

struct ApartmentGuard {
    bool active{};
    ~ApartmentGuard() { if (active) CoUninitialize(); }
};

ComPtr<IMMDevice> open_endpoint(IMMDeviceEnumerator* enumerator, const std::string& utf8_id) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_id.data(),
                                          static_cast<int>(utf8_id.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("The selected WASAPI device ID is invalid UTF-8");
    std::wstring id(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_id.data(), static_cast<int>(utf8_id.size()),
                            id.data(), count) != count)
        throw std::runtime_error("Could not decode the selected WASAPI device ID");
    IMMDevice* raw = nullptr;
    check_hresult(enumerator->GetDevice(id.c_str(), &raw), "Open the selected WASAPI endpoint");
    return ComPtr<IMMDevice>(raw);
}

ComPtr<IAudioClient> activate_audio_client(IMMDevice* device, const char* description) {
    IAudioClient* raw = nullptr;
    const HRESULT result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void**>(&raw));
    check_hresult(result, description);
    return ComPtr<IAudioClient>(raw);
}

void drain_capture(IAudioCaptureClient* capture, audio::ProcessorChain& processor,
                   AudioRing& queue, bool& saw_overrun, LiveProcessingMetrics& metrics) {
    UINT32 packet_frames = 0;
    check_hresult(capture->GetNextPacketSize(&packet_frames), "Read live microphone packet size");
    std::array<std::int16_t, 8192> input{};
    std::array<std::int16_t, 8192> output{};
    while (packet_frames != 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (result == AUDCLNT_E_DEVICE_INVALIDATED)
            throw std::runtime_error("The selected microphone disconnected during live processing");
        check_hresult(result, "Read live microphone audio");
        if (frames > input.size()) {
            capture->ReleaseBuffer(frames);
            throw std::runtime_error("WASAPI microphone packet exceeds the preallocated processing buffer");
        }
        for (UINT32 index = 0; index < frames; ++index)
            input[index] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 0 : reinterpret_cast<const std::int16_t*>(data)[index];
        const auto processing_start = std::chrono::steady_clock::now();
        processor.process(std::span<const std::int16_t>(input).first(frames),
                          std::span<std::int16_t>(output).first(frames));
        const auto processing_ms = static_cast<float>(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - processing_start).count());
        float previous_max = metrics.max_processing_packet_ms.load(std::memory_order_relaxed);
        while (processing_ms > previous_max &&
               !metrics.max_processing_packet_ms.compare_exchange_weak(previous_max, processing_ms,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}
        metrics.processing_time_available.store(true, std::memory_order_relaxed);
        if (frames != 0) {
            double input_square_sum = 0.0;
            double output_square_sum = 0.0;
            for (UINT32 index = 0; index < frames; ++index) {
                const auto input_value = static_cast<double>(input[index]) / 32768.0;
                const auto output_value = static_cast<double>(output[index]) / 32768.0;
                input_square_sum += input_value * input_value;
                output_square_sum += output_value * output_value;
            }
            metrics.input_rms.store(static_cast<float>(std::sqrt(input_square_sum / frames)),
                                    std::memory_order_relaxed);
            metrics.output_rms.store(static_cast<float>(std::sqrt(output_square_sum / frames)),
                                     std::memory_order_relaxed);
        }
        saw_overrun = queue.push(std::span<const std::int16_t>(output).first(frames)) || saw_overrun;
        check_hresult(capture->ReleaseBuffer(frames), "Release live microphone packet");
        check_hresult(capture->GetNextPacketSize(&packet_frames), "Read next live microphone packet size");
    }
}

void render_queued(IAudioClient* client, IAudioRenderClient* render, AudioRing& queue,
                   std::vector<std::int16_t>& output) {
    UINT32 padding = 0;
    check_hresult(client->GetCurrentPadding(&padding), "Read virtual audio output padding");
    UINT32 buffer_frames = 0;
    check_hresult(client->GetBufferSize(&buffer_frames), "Read virtual audio output buffer size");
    if (padding > buffer_frames) throw std::runtime_error("WASAPI reported invalid render buffer padding");
    const auto available = buffer_frames - padding;
    if (available == 0) return;
    if (available > output.size()) throw std::runtime_error("WASAPI render buffer exceeds preallocated storage");
    BYTE* raw = nullptr;
    check_hresult(render->GetBuffer(available, &raw), "Acquire processed audio output buffer");
    auto samples = std::span<std::int16_t>(output).first(available);
    queue.pop(samples);
    std::memcpy(raw, samples.data(), samples.size_bytes());
    check_hresult(render->ReleaseBuffer(available, 0), "Submit processed audio output buffer");
}
}

void run_live_processing(const std::string& input_device_id, const std::string& output_device_id,
                         const audio::ProcessingSettings settings, const std::atomic_bool& stop_requested,
                         LiveProcessingMetrics& metrics, const std::function<void()>& on_started) {
    if (input_device_id.empty() || output_device_id.empty())
        throw std::invalid_argument("Select both a microphone and a virtual-cable playback endpoint");

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        check_hresult(initialized, "Initialize Windows audio COM");
    ApartmentGuard apartment{SUCCEEDED(initialized)};

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator)),
                  "Create WASAPI device enumerator");
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);
    auto input_device = open_endpoint(enumerator.get(), input_device_id);
    auto output_device = open_endpoint(enumerator.get(), output_device_id);
    auto input_client = activate_audio_client(input_device.get(), "Open microphone audio client");
    auto output_client = activate_audio_client(output_device.get(), "Open selected virtual-cable playback client");

    WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0};
    constexpr DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    check_hresult(input_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0,
                                           &format, nullptr),
                  "Initialize 48 kHz microphone capture (the endpoint must support Windows shared-mode conversion)");
    check_hresult(output_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0,
                                            &format, nullptr),
                  "Initialize 48 kHz processed audio output (select a compatible virtual cable playback endpoint)");

    AudioEvent capture_event;
    AudioEvent render_event;
    check_hresult(input_client->SetEventHandle(capture_event.get()), "Set microphone capture event");
    check_hresult(output_client->SetEventHandle(render_event.get()), "Set processed audio render event");

    IAudioCaptureClient* raw_capture = nullptr;
    check_hresult(input_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&raw_capture)),
                  "Open live microphone capture buffer");
    ComPtr<IAudioCaptureClient> capture(raw_capture);
    IAudioRenderClient* raw_render = nullptr;
    check_hresult(output_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&raw_render)),
                  "Open selected audio render buffer");
    ComPtr<IAudioRenderClient> render(raw_render);
    UINT32 render_buffer_frames = 0;
    check_hresult(output_client->GetBufferSize(&render_buffer_frames), "Read audio render capacity");
    UINT32 capture_buffer_frames = 0;
    check_hresult(input_client->GetBufferSize(&capture_buffer_frames), "Read microphone capture capacity");
    metrics.capture_buffer_frames.store(capture_buffer_frames, std::memory_order_relaxed);
    metrics.render_buffer_frames.store(render_buffer_frames, std::memory_order_relaxed);
    REFERENCE_TIME capture_latency = 0;
    if (SUCCEEDED(input_client->GetStreamLatency(&capture_latency)) && capture_latency > 0) {
        metrics.capture_latency_ms.store(static_cast<float>(capture_latency) / 10000.0F,
                                         std::memory_order_relaxed);
        metrics.capture_latency_available.store(true, std::memory_order_relaxed);
    }
    REFERENCE_TIME render_latency = 0;
    if (SUCCEEDED(output_client->GetStreamLatency(&render_latency)) && render_latency > 0) {
        metrics.render_latency_ms.store(static_cast<float>(render_latency) / 10000.0F,
                                        std::memory_order_relaxed);
        metrics.render_latency_available.store(true, std::memory_order_relaxed);
    }
    std::vector<std::int16_t> render_buffer(render_buffer_frames);
    BYTE* startup_data = nullptr;
    check_hresult(render->GetBuffer(render_buffer_frames, &startup_data), "Acquire initial processed audio buffer");
    std::memset(startup_data, 0, render_buffer_frames * sizeof(std::int16_t));
    check_hresult(render->ReleaseBuffer(render_buffer_frames, 0), "Prime processed audio output buffer");

    audio::ProcessorChain processor(1, settings);
    AudioRing queue;
    bool saw_overrun = false;
    struct StreamGuard {
        IAudioClient* input{};
        IAudioClient* output{};
        ~StreamGuard() { if (input) input->Stop(); if (output) output->Stop(); }
    } stream_guard{input_client.get(), output_client.get()};
    check_hresult(output_client->Start(), "Start processed audio output");
    check_hresult(input_client->Start(), "Start live microphone capture");
    if (on_started) on_started();

    HANDLE events[]{capture_event.get(), render_event.get()};
    while (!stop_requested.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait == WAIT_FAILED)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for WASAPI audio event");
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1)
            throw std::runtime_error("WASAPI audio event returned an unexpected status");
        const bool capture_ready = wait == WAIT_OBJECT_0 ||
            WaitForSingleObject(capture_event.get(), 0) == WAIT_OBJECT_0;
        const bool render_ready = wait == WAIT_OBJECT_0 + 1 ||
            WaitForSingleObject(render_event.get(), 0) == WAIT_OBJECT_0;
        if (render_ready) render_queued(output_client.get(), render.get(), queue, render_buffer);
        if (capture_ready) drain_capture(capture.get(), processor, queue, saw_overrun, metrics);
        if (saw_overrun) throw std::runtime_error("Live audio output could not keep up; stop and restart the route");
    }
}
}
