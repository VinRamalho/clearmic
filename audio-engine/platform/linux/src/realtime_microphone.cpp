#include "clearmic/platform/linux/device_manager.hpp"

#include "clearmic/audio/processor_chain.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace clearmic::platform::pipewire {
namespace {
constexpr std::size_t ring_capacity = 1U << 15U;
static_assert((ring_capacity & (ring_capacity - 1U)) == 0);

// Single producer (capture callback), single consumer (virtual source callback).
struct AudioRing {
    std::array<std::int16_t, ring_capacity> samples{};
    std::atomic<std::uint64_t> write_index{};
    std::atomic<std::uint64_t> read_index{};

    void push(const std::int16_t* input, std::size_t count) noexcept {
        auto write = write_index.load(std::memory_order_relaxed);
        auto read = read_index.load(std::memory_order_acquire);
        if (count > ring_capacity) {
            input += count - ring_capacity;
            count = ring_capacity;
        }
        if (write + count - read > ring_capacity) {
            const auto drop = write + count - read - ring_capacity;
            read_index.store(read + drop, std::memory_order_release);
        }
        for (std::size_t i = 0; i < count; ++i) samples[(write + i) & (ring_capacity - 1U)] = input[i];
        write_index.store(write + count, std::memory_order_release);
    }

    std::size_t pop(std::int16_t* output, std::size_t count) noexcept {
        const auto read = read_index.load(std::memory_order_relaxed);
        const auto write = write_index.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(write - read, count));
        for (std::size_t i = 0; i < available; ++i) output[i] = samples[(read + i) & (ring_capacity - 1U)];
        read_index.store(read + available, std::memory_order_release);
        std::fill(output + available, output + count, 0);
        return available;
    }
};

struct Session {
    pw_main_loop* loop{};
    audio::ProcessorChain processor{1};
    AudioRing ring;
    std::vector<std::int16_t> callback_output;
    pw_stream* capture{};
    pw_stream* source{};
    spa_hook capture_listener{};
    spa_hook source_listener{};
    std::string error;
};

std::atomic<pw_main_loop*> active_loop{};
void handle_signal(int) {
    if (auto* loop = active_loop.load(std::memory_order_relaxed)) pw_main_loop_quit(loop);
}

void state_changed(void* data, pw_stream_state old_state, pw_stream_state state, const char* error) {
    auto& session = *static_cast<Session*>(data);
    if (state == PW_STREAM_STATE_ERROR) session.error = error ? error : "PipeWire stream failed";
    else if (state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED)
        session.error = "PipeWire audio stream disconnected";
    if (!session.error.empty()) pw_main_loop_quit(session.loop);
}

void capture_process(void* data) {
    auto& session = *static_cast<Session*>(data);
    auto* buffer = pw_stream_dequeue_buffer(session.capture);
    if (!buffer) return;
    auto* b = buffer->buffer;
    if (!b || b->n_datas == 0 || !b->datas[0].data || !b->datas[0].chunk ||
        b->datas[0].chunk->stride != static_cast<int>(sizeof(std::int16_t))) {
        session.error = "PipeWire capture returned an unsupported audio buffer";
        pw_stream_queue_buffer(session.capture, buffer);
        pw_main_loop_quit(session.loop);
        return;
    }
    auto& d = b->datas[0];
    const auto count = d.chunk->size / sizeof(std::int16_t);
    const auto* input = reinterpret_cast<const std::int16_t*>(static_cast<const std::byte*>(d.data) + d.chunk->offset);
    // PipeWire callback buffers are bounded; storage is preallocated before streaming.
    if (session.callback_output.size() < count) {
        session.error = "PipeWire callback exceeded the preallocated audio buffer";
        pw_stream_queue_buffer(session.capture, buffer);
        pw_main_loop_quit(session.loop);
        return;
    }
    session.processor.process(std::span<const std::int16_t>(input, count), std::span<std::int16_t>(session.callback_output).first(count));
    session.ring.push(session.callback_output.data(), count);
    pw_stream_queue_buffer(session.capture, buffer);
}

void source_process(void* data) {
    auto& session = *static_cast<Session*>(data);
    auto* buffer = pw_stream_dequeue_buffer(session.source);
    if (!buffer) return;
    auto* b = buffer->buffer;
    if (!b || b->n_datas == 0 || !b->datas[0].data || !b->datas[0].chunk) {
        session.error = "PipeWire virtual microphone returned an unsupported audio buffer";
        pw_stream_queue_buffer(session.source, buffer);
        pw_main_loop_quit(session.loop);
        return;
    }
    auto& d = b->datas[0];
    const auto capacity = d.maxsize / sizeof(std::int16_t);
    auto* output = static_cast<std::int16_t*>(d.data);
    session.ring.pop(output, capacity);
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
    ~Runtime() {
        spa_hook_remove(&session.capture_listener);
        spa_hook_remove(&session.source_listener);
        if (session.capture) pw_stream_destroy(session.capture);
        if (session.source) pw_stream_destroy(session.source);
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
    }
};

pw_stream* create_stream(pw_core* core, Session& session, const char* name, pw_properties* properties,
                         spa_hook* listener, const pw_stream_events* events, pw_stream** out) {
    *out = pw_stream_new(core, name, properties);
    if (!*out) throw std::runtime_error(std::string("Could not create PipeWire stream: ") + name);
    pw_stream_add_listener(*out, listener, events, &session);
    return *out;
}

void connect_audio(pw_stream* stream, const pw_direction direction) {
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16_LE;
    format.rate = 48000;
    format.channels = 1;
    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    const auto result = pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1);
    if (result < 0) throw std::runtime_error("Could not connect a ClearMic PipeWire audio stream");
}
}

void run_realtime_microphone(const std::string& device_id) {
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
    runtime.session.callback_output.resize(8192);
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
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_TARGET_OBJECT, PW_ID_ANY, PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr);
    if (!source_props) throw std::runtime_error("Could not allocate PipeWire virtual source properties");
    create_stream(runtime.core, runtime.session, "ClearMic Virtual Microphone", source_props,
                  &runtime.session.source_listener, &source_events, &runtime.session.source);
    connect_audio(runtime.session.capture, PW_DIRECTION_INPUT);
    connect_audio(runtime.session.source, PW_DIRECTION_OUTPUT);

    const auto previous_int = std::signal(SIGINT, handle_signal);
    const auto previous_term = std::signal(SIGTERM, handle_signal);
    active_loop.store(runtime.loop, std::memory_order_relaxed);
    pw_main_loop_run(runtime.loop);
    active_loop.store(nullptr, std::memory_order_relaxed);
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    if (!runtime.session.error.empty()) throw std::runtime_error(runtime.session.error);
}
}
