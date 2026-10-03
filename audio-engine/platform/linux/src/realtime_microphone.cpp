#include "clearmic/platform/linux/device_manager.hpp"
#include "clearmic/platform/linux/realtime_audio_ring.hpp"

#include "clearmic/audio/processor_chain.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace clearmic::platform::pipewire {
namespace {
enum class StreamError : std::uint8_t {
    none,
    writing_detail,
    stream_failure,
    disconnected,
    unsupported_capture_buffer,
    invalid_capture_range,
    unaligned_capture_chunk,
    oversized_capture_buffer,
    unsupported_source_buffer,
    oversized_source_buffer,
};

struct Session {
    pw_main_loop* loop{};
    audio::ProcessorChain processor{1};
    RealtimeAudioRing ring;
    std::array<std::int16_t, 8192> callback_output{};
    std::array<std::int16_t, 8192> source_silence{};
    std::atomic<std::uint64_t> capture_overruns{};
    std::atomic<std::uint64_t> source_underruns{};
    std::atomic<std::uint64_t> processed_samples{};
    std::atomic<float> max_dsp_ms{};
    std::atomic<float> max_dsp_budget_percent{};
    std::atomic<float> input_rms{};
    std::atomic<float> output_rms{};
    pw_stream* capture{};
    pw_stream* source{};
    spa_hook capture_listener{};
    spa_hook source_listener{};
    std::atomic<StreamError> error{StreamError::none};
    std::atomic_bool shutting_down{};
    std::array<char, 256> error_detail{};
};

const char* default_error_detail(const StreamError error) noexcept {
    switch (error) {
    case StreamError::stream_failure: return "PipeWire audio stream failed";
    case StreamError::disconnected: return "PipeWire audio stream disconnected";
    case StreamError::unsupported_capture_buffer: return "PipeWire capture returned an unsupported audio buffer";
    case StreamError::invalid_capture_range: return "PipeWire capture buffer reported an invalid chunk range";
    case StreamError::unaligned_capture_chunk: return "PipeWire capture chunk is not aligned to PCM16 samples";
    case StreamError::oversized_capture_buffer:
        return "PipeWire microphone callback exceeded the preallocated audio buffer";
    case StreamError::oversized_source_buffer:
        return "PipeWire virtual microphone callback exceeded the preallocated audio buffer";
    case StreamError::unsupported_source_buffer:
        return "PipeWire virtual microphone returned an unsupported audio buffer";
    case StreamError::none:
    case StreamError::writing_detail: return "PipeWire audio stream failed";
    }
    return "PipeWire audio stream failed";
}

bool set_stream_error(Session& session, const StreamError error, const char* detail = nullptr) noexcept {
    auto expected = StreamError::none;
    if (!session.error.compare_exchange_strong(expected, StreamError::writing_detail,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
    const char* text = detail && detail[0] ? detail : default_error_detail(error);
    std::size_t length = 0;
    while (text[length] != '\0' && length + 1 < session.error_detail.size()) {
        session.error_detail[length] = text[length];
        ++length;
    }
    session.error_detail[length] = '\0';
    session.error.store(error, std::memory_order_release);
    return true;
}

std::atomic<spa_source*> pending_signal_event{};
std::atomic<pw_main_loop*> pending_signal_loop{};
struct Diagnostics {
    Session* session{};
    std::uint64_t last_processed{};
};
spa_source* diagnostics_timer{};
void on_diagnostics_timer(void* data, std::uint64_t) {
    auto& diagnostics = *static_cast<Diagnostics*>(data);
    auto& session = *diagnostics.session;
    const auto processed = session.processed_samples.load(std::memory_order_relaxed);
    const auto overruns = session.capture_overruns.load(std::memory_order_relaxed);
    const auto underruns = session.source_underruns.load(std::memory_order_relaxed);
    const auto max_dsp_ms = session.max_dsp_ms.load(std::memory_order_relaxed);
    const auto max_dsp_budget = session.max_dsp_budget_percent.load(std::memory_order_relaxed);
    const auto input_level = session.input_rms.load(std::memory_order_relaxed);
    const auto output_level = session.output_rms.load(std::memory_order_relaxed);
    std::cout << "METER " << input_level << ' ' << output_level << '\n'
              << "DIAG " << max_dsp_ms << ' ' << max_dsp_budget << ' '
              << overruns << ' ' << underruns << ' ' << processed / 48000 << '\n' << std::flush;
    if (processed != diagnostics.last_processed || overruns != 0 || underruns != 0) {
        std::clog << "ClearMic metrics: processed=" << processed / 48000 << "s"
                  << " capture_overruns=" << overruns << " source_underruns=" << underruns << '\n';
        diagnostics.last_processed = processed;
    }
}

void handle_signal(int) {
    auto* loop = pending_signal_loop.load(std::memory_order_relaxed);
    auto* event = pending_signal_event.load(std::memory_order_relaxed);
    if (loop && event) pw_loop_signal_event(pw_main_loop_get_loop(loop), event);
}

void on_signal_event(void* data, std::uint64_t) {
    pw_main_loop_quit(static_cast<pw_main_loop*>(data));
}

void state_changed(void* data, pw_stream_state old_state, pw_stream_state state, const char* detail) {
    auto& session = *static_cast<Session*>(data);
    if (session.shutting_down.load(std::memory_order_relaxed)) return;
    const bool failed = state == PW_STREAM_STATE_ERROR
        ? set_stream_error(session, StreamError::stream_failure, detail)
        : (state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED
            ? set_stream_error(session, StreamError::disconnected) : false);
    if (failed) pw_main_loop_quit(session.loop);
}

void capture_process(void* data) {
    auto& session = *static_cast<Session*>(data);
    auto* buffer = pw_stream_dequeue_buffer(session.capture);
    if (!buffer) return;
    auto* b = buffer->buffer;
    if (!b || b->n_datas == 0 || !b->datas[0].data || !b->datas[0].chunk ||
        b->datas[0].chunk->stride != static_cast<int>(sizeof(std::int16_t))) {
        const bool failed = set_stream_error(session, StreamError::unsupported_capture_buffer);
        pw_stream_queue_buffer(session.capture, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    auto& d = b->datas[0];
    if (d.chunk->offset > d.maxsize || d.chunk->size > d.maxsize - d.chunk->offset) {
        const bool failed = set_stream_error(session, StreamError::invalid_capture_range);
        pw_stream_queue_buffer(session.capture, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    if ((d.chunk->size % sizeof(std::int16_t)) != 0) {
        const bool failed = set_stream_error(session, StreamError::unaligned_capture_chunk);
        pw_stream_queue_buffer(session.capture, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    const auto count = d.chunk->size / sizeof(std::int16_t);
    const auto* input = reinterpret_cast<const std::int16_t*>(static_cast<const std::byte*>(d.data) + d.chunk->offset);
    // PipeWire callback buffers are bounded; storage is preallocated before streaming.
    if (count > session.callback_output.size()) {
        const bool failed = set_stream_error(session, StreamError::oversized_capture_buffer);
        pw_stream_queue_buffer(session.capture, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    const auto process_start = std::chrono::steady_clock::now();
    session.processor.process(std::span<const std::int16_t>(input, count), std::span<std::int16_t>(session.callback_output).first(count));
    const auto process_end = std::chrono::steady_clock::now();
    if (count != 0) {
        const auto elapsed_ms = std::chrono::duration<float, std::milli>(process_end - process_start).count();
        const auto packet_ms = static_cast<float>(count) * 1000.0F / 48000.0F;
        const auto budget_percent = elapsed_ms * 100.0F / packet_ms;
        auto max_ms = session.max_dsp_ms.load(std::memory_order_relaxed);
        while (elapsed_ms > max_ms && !session.max_dsp_ms.compare_exchange_weak(
                   max_ms, elapsed_ms, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        auto max_budget = session.max_dsp_budget_percent.load(std::memory_order_relaxed);
        while (budget_percent > max_budget && !session.max_dsp_budget_percent.compare_exchange_weak(
                   max_budget, budget_percent, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
    double input_square_sum = 0.0;
    double output_square_sum = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const double before = static_cast<double>(input[index]) / 32768.0;
        const double after = static_cast<double>(session.callback_output[index]) / 32768.0;
        input_square_sum += before * before;
        output_square_sum += after * after;
    }
    if (count != 0) {
        session.input_rms.store(static_cast<float>(std::sqrt(input_square_sum / count)), std::memory_order_relaxed);
        session.output_rms.store(static_cast<float>(std::sqrt(output_square_sum / count)), std::memory_order_relaxed);
    }
    if (!session.ring.push(session.callback_output.data(), count))
        session.capture_overruns.fetch_add(1, std::memory_order_relaxed);
    session.processed_samples.fetch_add(count, std::memory_order_relaxed);
    pw_stream_queue_buffer(session.capture, buffer);
}

void source_process(void* data) {
    auto& session = *static_cast<Session*>(data);
    auto* buffer = pw_stream_dequeue_buffer(session.source);
    if (!buffer) return;
    auto* b = buffer->buffer;
    if (!b || b->n_datas == 0 || !b->datas[0].data || !b->datas[0].chunk) {
        const bool failed = set_stream_error(session, StreamError::unsupported_source_buffer);
        pw_stream_queue_buffer(session.source, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    auto& d = b->datas[0];
    const auto capacity = d.maxsize / sizeof(std::int16_t);
    if (capacity > session.source_silence.size()) {
        const bool failed = set_stream_error(session, StreamError::oversized_source_buffer);
        pw_stream_queue_buffer(session.source, buffer);
        if (failed) pw_main_loop_quit(session.loop);
        return;
    }
    auto* output = static_cast<std::int16_t*>(d.data);
    if (session.ring.pop(output, capacity, session.source_silence.data()) < capacity)
        session.source_underruns.fetch_add(1, std::memory_order_relaxed);
    d.chunk->offset = 0;
    d.chunk->stride = sizeof(std::int16_t);
    d.chunk->size = static_cast<std::uint32_t>(capacity * sizeof(std::int16_t));
    pw_stream_queue_buffer(session.source, buffer);
}

const pw_stream_events capture_events{.version = PW_VERSION_STREAM_EVENTS, .state_changed = state_changed, .process = capture_process};
const pw_stream_events source_events{.version = PW_VERSION_STREAM_EVENTS, .state_changed = state_changed, .process = source_process};

struct Runtime {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    Session session;
    void shutdown() noexcept {
        session.shutting_down.store(true, std::memory_order_relaxed);
        spa_hook_remove(&session.capture_listener);
        spa_hook_remove(&session.source_listener);
        if (session.capture) {
            pw_stream_destroy(session.capture);
            session.capture = nullptr;
        }
        if (session.source) {
            pw_stream_destroy(session.source);
            session.source = nullptr;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
        if (context) {
            pw_context_destroy(context);
            context = nullptr;
        }
        if (loop) {
            pw_main_loop_destroy(loop);
            loop = nullptr;
        }
    }
    ~Runtime() { shutdown(); }
};

pw_stream* create_stream(pw_core* core, Session& session, const char* name, pw_properties* properties,
                         spa_hook* listener, const pw_stream_events* events, pw_stream** out) {
    *out = pw_stream_new(core, name, properties);
    if (!*out) throw std::runtime_error(std::string("Could not create PipeWire stream: ") + name);
    pw_stream_add_listener(*out, listener, events, &session);
    return *out;
}

void connect_audio(pw_stream* stream, const pw_direction direction, const char* target = nullptr) {
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16_LE;
    format.rate = 48000;
    format.channels = 1;
    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (target && pw_properties_set(const_cast<pw_properties*>(pw_stream_get_properties(stream)), PW_KEY_TARGET_OBJECT, target) < 0)
        throw std::runtime_error("Could not set the selected PipeWire microphone target");
    const auto result = pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1);
    if (result < 0) throw std::runtime_error("Could not connect a ClearMic PipeWire audio stream");
}
}

void run_realtime_microphone(const std::string& device_id, const audio::ProcessingSettings settings) {
    DeviceManager devices;
    const auto available = devices.input_devices();
    if (available.empty()) throw std::runtime_error("PipeWire has no microphone source to capture");
    if (!device_id.empty() && std::none_of(available.begin(), available.end(), [&](const auto& d) { return d.id == device_id; }))
        throw std::runtime_error("The selected PipeWire microphone is no longer available");

    pw_init(nullptr, nullptr);
    Runtime runtime;
    runtime.loop = pw_main_loop_new(nullptr);
    if (!runtime.loop) throw std::runtime_error("Could not create PipeWire main loop");
    runtime.session.loop = runtime.loop;
    runtime.session.processor.set_settings(settings);
    runtime.context = pw_context_new(pw_main_loop_get_loop(runtime.loop), nullptr, 0);
    if (!runtime.context) throw std::runtime_error("Could not create PipeWire context");
    runtime.core = pw_context_connect(runtime.context, nullptr, 0);
    if (!runtime.core) throw std::runtime_error("Could not connect to PipeWire");

    auto* capture_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_NAME, "clearmic_capture",
        PW_KEY_NODE_DESCRIPTION, "ClearMic processed capture", nullptr);
    if (!capture_props) throw std::runtime_error("Could not allocate PipeWire capture properties");
    create_stream(runtime.core, runtime.session, "ClearMic capture", capture_props,
                  &runtime.session.capture_listener, &capture_events, &runtime.session.capture);

    auto* source_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, "clearmic_virtual_microphone", PW_KEY_NODE_DESCRIPTION, "ClearMic Virtual Microphone",
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr);
    if (!source_props) throw std::runtime_error("Could not allocate PipeWire virtual source properties");
    create_stream(runtime.core, runtime.session, "ClearMic Virtual Microphone", source_props,
                  &runtime.session.source_listener, &source_events, &runtime.session.source);
    connect_audio(runtime.session.capture, PW_DIRECTION_INPUT, device_id.empty() ? nullptr : device_id.c_str());
    connect_audio(runtime.session.source, PW_DIRECTION_OUTPUT);

    const auto previous_int = std::signal(SIGINT, handle_signal);
    const auto previous_term = std::signal(SIGTERM, handle_signal);
    const auto loop = pw_main_loop_get_loop(runtime.loop);
    auto* signal_event = pw_loop_add_event(loop, on_signal_event, runtime.loop);
    pending_signal_loop.store(runtime.loop, std::memory_order_relaxed);
    pending_signal_event.store(signal_event, std::memory_order_relaxed);
    Diagnostics diagnostics{&runtime.session};
    diagnostics_timer = pw_loop_add_timer(loop, on_diagnostics_timer, &diagnostics);
    if (!signal_event || !diagnostics_timer) throw std::runtime_error("Could not initialize PipeWire service diagnostics");
    timespec first_fire{1, 0};
    timespec repeat{1, 0};
    pw_loop_update_timer(loop, diagnostics_timer, &first_fire, &repeat, false);
    pw_main_loop_run(runtime.loop);
    pending_signal_loop.store(nullptr, std::memory_order_relaxed);
    pending_signal_event.store(nullptr, std::memory_order_relaxed);
    pw_loop_destroy_source(loop, signal_event);
    pw_loop_destroy_source(loop, diagnostics_timer);
    diagnostics_timer = nullptr;
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    runtime.shutdown(); // Stop PipeWire callbacks before reading their error detail.
    const auto error = runtime.session.error.load(std::memory_order_acquire);
    if (error != StreamError::none)
        throw std::runtime_error(runtime.session.error_detail.data());
}
}
