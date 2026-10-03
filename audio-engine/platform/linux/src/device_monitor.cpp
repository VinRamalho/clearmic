#include "clearmic/platform/linux/device_manager.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace clearmic::platform::pipewire {
namespace {
struct MonitorRuntime {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_registry* registry{};
    pw_metadata* metadata{};
    spa_hook core_listener{};
    spa_hook registry_listener{};
    spa_hook metadata_listener{};
    spa_source* stop_event{};
    std::unordered_set<std::uint32_t> input_source_ids;
    std::atomic_bool ready{};
    bool core_listener_added{};
    bool registry_listener_added{};
    int sync_sequence{};
    std::function<void()>* on_change{};
};

void notify_if_ready(MonitorRuntime& state) {
    if (state.ready.load(std::memory_order_acquire) && state.on_change) (*state.on_change)();
}

int on_metadata_property(void* data, const std::uint32_t, const char* key, const char*, const char*) {
    if (key && std::string_view(key) == "default.audio.source")
        notify_if_ready(*static_cast<MonitorRuntime*>(data));
    return 0;
}
const pw_metadata_events metadata_events{.version = PW_VERSION_METADATA_EVENTS, .property = on_metadata_property};

void on_global(void* data, const std::uint32_t id, const std::uint32_t, const char* type,
               const std::uint32_t, const spa_dict* properties) {
    auto& state = *static_cast<MonitorRuntime*>(data);
    if (type && std::string_view(type) == PW_TYPE_INTERFACE_Metadata && properties) {
        const char* name = spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
        if (name && std::string_view(name) == "default" && !state.metadata) {
            state.metadata = static_cast<pw_metadata*>(pw_registry_bind(state.registry, id,
                PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (state.metadata)
                pw_metadata_add_listener(state.metadata, &state.metadata_listener, &metadata_events, &state);
        }
        return;
    }
    if (!type || std::string_view(type) != PW_TYPE_INTERFACE_Node || !properties) return;
    const char* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (media_class && std::string_view(media_class) == "Audio/Source" &&
        state.input_source_ids.insert(id).second) notify_if_ready(state);
}

void on_global_remove(void* data, const std::uint32_t id) {
    auto& state = *static_cast<MonitorRuntime*>(data);
    if (state.input_source_ids.erase(id) != 0) notify_if_ready(state);
}

const pw_registry_events registry_events{
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

void on_core_done(void* data, const std::uint32_t id, const int sequence) {
    auto& state = *static_cast<MonitorRuntime*>(data);
    if (id == PW_ID_CORE && sequence == state.sync_sequence) {
        state.ready.store(true, std::memory_order_release);
        if (state.on_change) (*state.on_change)();
    }
}

void on_core_error(void* data, const std::uint32_t id, const int, const int, const char*) {
    auto& state = *static_cast<MonitorRuntime*>(data);
    if (id == PW_ID_CORE && state.loop) {
        notify_if_ready(state);
        pw_main_loop_quit(state.loop);
    }
}

const pw_core_events core_events{
    .version = PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

void on_stop_event(void* data, std::uint64_t) {
    auto& state = *static_cast<MonitorRuntime*>(data);
    if (state.loop) pw_main_loop_quit(state.loop);
}

void destroy_runtime(MonitorRuntime& state) {
    if (state.metadata) {
        spa_hook_remove(&state.metadata_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.metadata));
    }
    if (state.registry_listener_added) spa_hook_remove(&state.registry_listener);
    if (state.core_listener_added) spa_hook_remove(&state.core_listener);
    if (state.registry) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry));
    if (state.core) pw_core_disconnect(state.core);
    if (state.context) pw_context_destroy(state.context);
    if (state.loop) {
        if (state.stop_event) pw_loop_destroy_source(pw_main_loop_get_loop(state.loop), state.stop_event);
        pw_main_loop_destroy(state.loop);
    }
}
}

struct DeviceMonitor::Impl {
    explicit Impl(std::function<void()> callback) : on_change(std::move(callback)), worker([this] { run(); }) {}

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        {
            std::lock_guard lock(runtime_mutex);
            if (runtime_loop && runtime_stop_event)
                pw_loop_signal_event(pw_main_loop_get_loop(runtime_loop), runtime_stop_event);
        }
        if (worker.joinable()) worker.join();
    }

    void run() {
        pw_init(nullptr, nullptr);
        while (!stopping.load(std::memory_order_acquire)) {
            run_session();
            for (int attempt = 0; attempt < 10 && !stopping.load(std::memory_order_acquire); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void run_session() {
        MonitorRuntime state{};
        state.on_change = &on_change;
        state.loop = pw_main_loop_new(nullptr);
        if (!state.loop) return;
        state.context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
        if (!state.context) { destroy_runtime(state); return; }
        state.core = pw_context_connect(state.context, nullptr, 0);
        if (!state.core) { destroy_runtime(state); return; }
        pw_core_add_listener(state.core, &state.core_listener, &core_events, &state);
        state.core_listener_added = true;
        state.registry = pw_core_get_registry(state.core, PW_VERSION_REGISTRY, 0);
        if (!state.registry) { destroy_runtime(state); return; }
        pw_registry_add_listener(state.registry, &state.registry_listener, &registry_events, &state);
        state.registry_listener_added = true;
        state.sync_sequence = pw_core_sync(state.core, PW_ID_CORE, 0);
        if (state.sync_sequence < 0) { destroy_runtime(state); return; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto* loop = pw_main_loop_get_loop(state.loop);
        while (!state.ready.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            if (pw_loop_iterate(loop, 100) < 0) break;
        }
        if (!state.ready.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire)) {
            destroy_runtime(state);
            return;
        }
        state.stop_event = pw_loop_add_event(loop, on_stop_event, &state);
        if (!state.stop_event) { destroy_runtime(state); return; }
        {
            std::lock_guard lock(runtime_mutex);
            runtime_loop = state.loop;
            runtime_stop_event = state.stop_event;
            if (stopping.load(std::memory_order_acquire)) pw_loop_signal_event(loop, state.stop_event);
        }
        pw_main_loop_run(state.loop);
        {
            std::lock_guard lock(runtime_mutex);
            runtime_loop = nullptr;
            runtime_stop_event = nullptr;
        }
        destroy_runtime(state);
    }

    std::function<void()> on_change;
    std::atomic_bool stopping{};
    std::mutex runtime_mutex;
    pw_main_loop* runtime_loop{};
    spa_source* runtime_stop_event{};
    std::thread worker;
};

DeviceMonitor::DeviceMonitor(std::function<void()> on_change)
    : impl_(std::make_unique<Impl>(std::move(on_change))) {}

DeviceMonitor::~DeviceMonitor() = default;
}
