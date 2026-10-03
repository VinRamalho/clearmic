#include "clearmic/platform/linux/device_manager.hpp"

#include <pipewire/pipewire.h>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace clearmic::platform::pipewire {
namespace {
struct Enumeration {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_registry* registry{};
    spa_hook listener{};
    std::vector<audio::AudioDevice> devices;
    int sync_sequence{};
    bool synchronized{false};
};

void on_global(void* data, const std::uint32_t, const std::uint32_t, const char* type,
               const std::uint32_t, const spa_dict* properties) {
    if (std::string_view(type) != PW_TYPE_INTERFACE_Node || !properties) return;
    const char* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (!media_class || std::string_view(media_class) != "Audio/Source") return;
    const char* name = spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
    if (!name) name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    const char* id = spa_dict_lookup(properties, PW_KEY_OBJECT_SERIAL);
    audio::AudioDevice device;
    device.id = id ? id : (name ? name : "unknown");
    device.name = name ? name : "Unnamed PipeWire source";
    device.connection = audio::ConnectionState::connected;
    static_cast<Enumeration*>(data)->devices.push_back(std::move(device));
}
const pw_registry_events registry_events{.version = PW_VERSION_REGISTRY_EVENTS, .global = on_global};
void on_done(void* data, const std::uint32_t id, const int sequence) {
    auto& state = *static_cast<Enumeration*>(data);
    if (id == PW_ID_CORE && sequence == state.sync_sequence) state.synchronized = true;
}
const pw_core_events core_events{.version = PW_VERSION_CORE_EVENTS, .done = on_done};
}

std::vector<audio::AudioDevice> DeviceManager::input_devices() {
    pw_init(nullptr, nullptr);
    Enumeration state;
    state.loop = pw_main_loop_new(nullptr);
    if (!state.loop) throw std::runtime_error("Could not create PipeWire main loop");
    state.context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
    if (!state.context) { pw_main_loop_destroy(state.loop); throw std::runtime_error("Could not create PipeWire context"); }
    state.core = pw_context_connect(state.context, nullptr, 0);
    if (!state.core) {
        pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
        throw std::runtime_error("Could not connect to PipeWire");
    }
    spa_hook core_listener{};
    pw_core_add_listener(state.core, &core_listener, &core_events, &state);
    state.registry = pw_core_get_registry(state.core, PW_VERSION_REGISTRY, 0);
    if (!state.registry) {
        spa_hook_remove(&core_listener); pw_core_disconnect(state.core); pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
        throw std::runtime_error("Could not access PipeWire registry");
    }
    pw_registry_add_listener(state.registry, &state.listener, &registry_events, &state);
    state.sync_sequence = pw_core_sync(state.core, PW_ID_CORE, 0);
    for (int attempt = 0; attempt < 30 && !state.synchronized; ++attempt)
        pw_loop_iterate(pw_main_loop_get_loop(state.loop), 100);
    if (!state.synchronized) {
        spa_hook_remove(&state.listener); spa_hook_remove(&core_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry)); pw_core_disconnect(state.core);
        pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
        throw std::runtime_error("Timed out while enumerating PipeWire sources");
    }
    spa_hook_remove(&state.listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry));
    pw_core_disconnect(state.core); pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
    return state.devices;
}
}
