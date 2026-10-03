#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>

namespace {
constexpr std::uint32_t sample_rate = 48000;
constexpr double tone_hz = 440.0;
constexpr double two_pi = 6.28318530717958647692;

struct Source {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* stream{};
    spa_hook listener{};
    std::uint64_t sample_index{};
};

pw_main_loop* pending_loop{};

void handle_signal(int) {
    if (pending_loop) pw_main_loop_quit(pending_loop);
}

void stream_state_changed(void* data, pw_stream_state, pw_stream_state state, const char* error) {
    auto& source = *static_cast<Source*>(data);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        if (error) std::fprintf(stderr, "PipeWire test source: %s\n", error);
        pw_main_loop_quit(source.loop);
    }
}

void stream_process(void* data) {
    auto& source = *static_cast<Source*>(data);
    auto* buffer = pw_stream_dequeue_buffer(source.stream);
    if (!buffer) return;
    auto* pipewire_buffer = buffer->buffer;
    if (!pipewire_buffer || pipewire_buffer->n_datas == 0 || !pipewire_buffer->datas[0].data ||
        !pipewire_buffer->datas[0].chunk) {
        pw_stream_queue_buffer(source.stream, buffer);
        pw_main_loop_quit(source.loop);
        return;
    }

    auto& data_block = pipewire_buffer->datas[0];
    const auto capacity = data_block.maxsize / sizeof(std::int16_t);
    const auto requested = buffer->requested == 0
        ? capacity
        : static_cast<std::size_t>(std::min<std::uint64_t>(buffer->requested, capacity));
    auto* samples = static_cast<std::int16_t*>(data_block.data);
    for (std::size_t index = 0; index < requested; ++index) {
        const auto phase = two_pi * tone_hz * static_cast<double>(source.sample_index++) / sample_rate;
        samples[index] = static_cast<std::int16_t>(std::sin(phase) * 16384.0);
    }
    data_block.chunk->offset = 0;
    data_block.chunk->stride = sizeof(std::int16_t);
    data_block.chunk->size = static_cast<std::uint32_t>(requested * sizeof(std::int16_t));
    pw_stream_queue_buffer(source.stream, buffer);
}

const pw_stream_events stream_events{
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .process = stream_process,
};

void destroy(Source& source) noexcept {
    if (source.stream) {
        spa_hook_remove(&source.listener);
        pw_stream_destroy(source.stream);
    }
    if (source.core) pw_core_disconnect(source.core);
    if (source.context) pw_context_destroy(source.context);
    if (source.loop) pw_main_loop_destroy(source.loop);
}
} // namespace

int main(int argc, char** argv) {
    pw_init(&argc, &argv);
    Source source;
    source.loop = pw_main_loop_new(nullptr);
    if (!source.loop) return 1;
    pending_loop = source.loop;
    const auto previous_int = std::signal(SIGINT, handle_signal);
    const auto previous_term = std::signal(SIGTERM, handle_signal);
    source.context = pw_context_new(pw_main_loop_get_loop(source.loop), nullptr, 0);
    if (!source.context) {
        destroy(source);
        return 1;
    }
    source.core = pw_context_connect(source.context, nullptr, 0);
    if (!source.core) {
        destroy(source);
        return 1;
    }

    auto* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Test",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, "clearmic_e2e_source",
        PW_KEY_NODE_DESCRIPTION, "ClearMic E2E Test Source",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_AUDIO_CHANNELS, "1",
        "audio.position", "[ MONO ]",
        nullptr);
    source.stream = pw_stream_new(source.core, "ClearMic E2E Test Source", properties);
    if (!source.stream) {
        destroy(source);
        return 1;
    }
    pw_stream_add_listener(source.stream, &source.listener, &stream_events, &source);

    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16_LE;
    format.rate = sample_rate;
    format.channels = 1;
    format.position[0] = SPA_AUDIO_CHANNEL_MONO;
    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto result = pw_stream_connect(source.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), parameters, 1);
    if (result < 0) {
        std::fprintf(stderr, "Could not connect PipeWire test source: %s\n", spa_strerror(result));
        destroy(source);
        return 1;
    }
    pw_main_loop_run(source.loop);
    pending_loop = nullptr;
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    destroy(source);
    pw_deinit();
    return 0;
}
