#include "clearmic/platform/linux/device_manager.hpp"

#include "clearmic/audio/processor_chain.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace clearmic::platform::pipewire {
namespace {
struct CaptureState {
    explicit CaptureState(const audio::ProcessingSettings settings) : processor(1, settings) {}
    pw_main_loop* loop{};
    audio::ProcessorChain processor;
    audio::PcmAudio original{48000, 1, {}};
    audio::PcmAudio processed{48000, 1, {}};
    std::size_t offset{};
    std::string error;
};

void on_state_changed(void* data, const pw_stream_state old_state, const pw_stream_state state,
                      const char* error) {
    auto& capture = *static_cast<CaptureState*>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        capture.error = error ? error : "PipeWire capture stream failed";
        pw_main_loop_quit(capture.loop);
    } else if (state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED) {
        capture.error = "PipeWire capture stream disconnected";
        pw_main_loop_quit(capture.loop);
    }
}

struct StreamData {
    explicit StreamData(const audio::ProcessingSettings settings) : capture(settings) {}
    CaptureState capture;
    pw_stream* stream{};
    spa_hook listener{};

    ~StreamData() { spa_hook_remove(&listener); }
};

void process_stream(void* data) {
    auto& state = *static_cast<StreamData*>(data);
    auto* buffer = pw_stream_dequeue_buffer(state.stream);
    if (!buffer) return;
    auto* spa_buffer = buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas[0].data ||
        !spa_buffer->datas[0].chunk || spa_buffer->datas[0].chunk->stride != static_cast<int>(sizeof(std::int16_t))) {
        state.capture.error = "PipeWire returned an unsupported audio buffer layout";
        pw_stream_queue_buffer(state.stream, buffer);
        pw_main_loop_quit(state.capture.loop);
        return;
    }

    auto& data_buffer = spa_buffer->datas[0];
    const auto available_samples = data_buffer.chunk->size / sizeof(std::int16_t);
    const auto remaining = state.capture.original.samples.size() - state.capture.offset;
    const auto sample_count = std::min<std::size_t>(available_samples, remaining);
    if (sample_count != 0) {
        const auto* input = reinterpret_cast<const std::int16_t*>(
            static_cast<const std::byte*>(data_buffer.data) + data_buffer.chunk->offset);
        auto original_output = std::span(state.capture.original.samples).subspan(state.capture.offset, sample_count);
        auto processed_output = std::span(state.capture.processed.samples).subspan(state.capture.offset, sample_count);
        std::copy_n(input, sample_count, original_output.begin());
        state.capture.processor.process(std::span<const std::int16_t>(input, sample_count), processed_output);
        state.capture.offset += sample_count;
    }
    pw_stream_queue_buffer(state.stream, buffer);
    if (state.capture.offset == state.capture.original.samples.size()) pw_main_loop_quit(state.capture.loop);
}

const pw_stream_events stream_events{
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = process_stream};

struct PipeWireRuntime {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* stream{};

    ~PipeWireRuntime() {
        if (stream) pw_stream_destroy(stream);
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
    }
};
}

audio::AudioComparison capture_processed_audio(const std::string& device_id, const std::uint32_t duration_seconds,
                                               const audio::ProcessingSettings settings) {
    if (duration_seconds == 0 || duration_seconds > 30)
        throw std::invalid_argument("Capture duration must be between 1 and 30 seconds");
    DeviceManager device_manager;
    const auto available_devices = device_manager.input_devices();
    if (available_devices.empty()) throw std::runtime_error("PipeWire has no microphone source to capture");
    if (!device_id.empty() && std::none_of(available_devices.begin(), available_devices.end(),
            [&](const auto& device) { return device.id == device_id; }))
        throw std::runtime_error("The selected PipeWire microphone is no longer available");
    pw_init(nullptr, nullptr);
    PipeWireRuntime runtime;
    runtime.loop = pw_main_loop_new(nullptr);
    if (!runtime.loop) throw std::runtime_error("Could not create the PipeWire main loop");
    runtime.context = pw_context_new(pw_main_loop_get_loop(runtime.loop), nullptr, 0);
    if (!runtime.context) throw std::runtime_error("Could not create the PipeWire context");
    runtime.core = pw_context_connect(runtime.context, nullptr, 0);
    if (!runtime.core) throw std::runtime_error("Could not connect to the PipeWire server");

    StreamData state(settings);
    state.capture.loop = runtime.loop;
    const auto sample_count = static_cast<std::size_t>(duration_seconds) * 48000U;
    state.capture.original.samples.resize(sample_count);
    state.capture.processed.samples.resize(sample_count);

    auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                         PW_KEY_MEDIA_CATEGORY, "Capture",
                                         PW_KEY_MEDIA_ROLE, "Communication",
                                         PW_KEY_NODE_NAME, "clearmic_capture_test",
                                         PW_KEY_NODE_DESCRIPTION, "ClearMic microphone test",
                                         PW_KEY_NODE_LATENCY, "480/48000",
                                         nullptr);
    if (!properties) throw std::runtime_error("Could not allocate PipeWire stream properties");
    if (!device_id.empty()) pw_properties_set(properties, PW_KEY_TARGET_OBJECT, device_id.c_str());
    runtime.stream = pw_stream_new(runtime.core, "ClearMic microphone test", properties);
    if (!runtime.stream) throw std::runtime_error("Could not create the PipeWire capture stream");
    state.stream = runtime.stream;
    pw_stream_add_listener(runtime.stream, &state.listener, &stream_events, &state);

    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16_LE;
    format.rate = 48000;
    format.channels = 1;
    std::array<std::uint8_t, 1024> format_buffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(format_buffer.data(), format_buffer.size());
    const spa_pod* parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto connect_result = pw_stream_connect(runtime.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        parameters, 1);
    if (connect_result < 0) throw std::runtime_error("Could not connect to the selected PipeWire microphone");

    pw_main_loop_run(runtime.loop);
    if (!state.capture.error.empty()) throw std::runtime_error(state.capture.error);
    if (state.capture.offset != sample_count) throw std::runtime_error("Microphone disconnected before the test recording completed");
    return audio::AudioComparison{std::move(state.capture.original), std::move(state.capture.processed)};
}
}
