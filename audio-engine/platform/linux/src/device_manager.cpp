#include "clearmic/platform/linux/device_manager.hpp"
#include "bluez_battery.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/json.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace clearmic::platform::pipewire {
namespace {
struct Enumeration {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_registry* registry{};
    pw_metadata* metadata{};
    spa_hook listener{};
    spa_hook metadata_listener{};
    std::vector<audio::AudioDevice> devices;
    std::unordered_map<std::string, std::string> node_names;
    std::unordered_map<std::string, std::string> node_device_ids;
    std::unordered_map<std::string, std::pair<std::optional<std::uint16_t>, std::optional<std::uint16_t>>> device_usb_ids;
    std::string default_source_name;
    int sync_sequence{};
    bool synchronized{false};
};

int on_metadata_property(void* data, const std::uint32_t, const char* key, const char* type,
                         const char* value) {
    if (!key || !type || !value || std::string_view(key) != "default.audio.source" ||
        std::string_view(type) != "Spa:String:JSON") return 0;
    std::array<char, 1024> name{};
    spa_json object{};
    spa_json iterator{};
    spa_json_init(&object, value, std::char_traits<char>::length(value));
    if (spa_json_enter_object(&object, &iterator) <= 0) return 0;
    const char* token = nullptr;
    int length = 0;
    while ((length = spa_json_next(&iterator, &token)) > 0) {
        if (length == 6 && std::string_view(token, static_cast<std::size_t>(length)) == "\"name\"") {
            if (spa_json_get_string(&iterator, name.data(), static_cast<int>(name.size())) > 0)
                static_cast<Enumeration*>(data)->default_source_name = name.data();
            break;
        }
        if (spa_json_next(&iterator, &token) <= 0) break;
    }
    return 0;
}
const pw_metadata_events metadata_events{.version = PW_VERSION_METADATA_EVENTS, .property = on_metadata_property};

void on_global(void* data, const std::uint32_t global_id, const std::uint32_t, const char* type,
               const std::uint32_t, const spa_dict* properties) {
    auto& state = *static_cast<Enumeration*>(data);
    if (std::string_view(type) == PW_TYPE_INTERFACE_Device && properties) {
        std::optional<std::uint16_t> vendor_id;
        std::optional<std::uint16_t> product_id;
        if (const char* value = spa_dict_lookup(properties, PW_KEY_DEVICE_VENDOR_ID))
            vendor_id = audio::parse_device_identifier(value);
        if (const char* value = spa_dict_lookup(properties, PW_KEY_DEVICE_PRODUCT_ID))
            product_id = audio::parse_device_identifier(value);
        state.device_usb_ids[std::to_string(global_id)] = {vendor_id, product_id};
        return;
    }
    if (std::string_view(type) == PW_TYPE_INTERFACE_Metadata && properties) {
        const char* metadata_name = spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
        if (metadata_name && std::string_view(metadata_name) == "default") {
            state.metadata = static_cast<pw_metadata*>(pw_registry_bind(state.registry, global_id,
                PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (state.metadata)
                pw_metadata_add_listener(state.metadata, &state.metadata_listener, &metadata_events, &state);
        }
        return;
    }
    if (std::string_view(type) != PW_TYPE_INTERFACE_Node || !properties) return;
    const char* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (!media_class || std::string_view(media_class) != "Audio/Source") return;
    const char* name = spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
    if (!name) name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    audio::AudioDevice device;
    const char* serial = spa_dict_lookup(properties, PW_KEY_OBJECT_SERIAL);
    device.id = serial ? serial : std::to_string(global_id);
    device.name = name ? name : "Unnamed PipeWire source";
    const char* node_name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (node_name) state.node_names[node_name] = device.id;
    if (const char* parent_device_id = spa_dict_lookup(properties, PW_KEY_DEVICE_ID))
        state.node_device_ids[device.id] = parent_device_id;
    device.connection = audio::ConnectionState::connected;
    if (const char* vendor_id = spa_dict_lookup(properties, PW_KEY_DEVICE_VENDOR_ID))
        device.usb_vendor_id = audio::parse_device_identifier(vendor_id);
    if (const char* product_id = spa_dict_lookup(properties, PW_KEY_DEVICE_PRODUCT_ID))
        device.usb_product_id = audio::parse_device_identifier(product_id);
    const char* bluetooth_address = spa_dict_lookup(properties, "api.bluez5.address");
    if (!bluetooth_address) bluetooth_address = spa_dict_lookup(properties, "bluez5.address");
    if (bluetooth_address && *bluetooth_address) device.bluetooth_address = bluetooth_address;
    const char* bluetooth_profile = spa_dict_lookup(properties, "api.bluez5.profile");
    if (bluetooth_profile && *bluetooth_profile) device.bluetooth_profile = bluetooth_profile;
    const char* bluetooth_codec = spa_dict_lookup(properties, "api.bluez5.codec");
    if (bluetooth_codec && *bluetooth_codec) device.bluetooth_codec = bluetooth_codec;
    state.devices.push_back(std::move(device));
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
    if (state.synchronized && state.metadata) {
        state.synchronized = false;
        state.sync_sequence = pw_core_sync(state.core, PW_ID_CORE, 0);
        for (int attempt = 0; attempt < 30 && !state.synchronized; ++attempt)
            pw_loop_iterate(pw_main_loop_get_loop(state.loop), 100);
    }
    if (!state.synchronized) {
        if (state.metadata) {
            spa_hook_remove(&state.metadata_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.metadata));
        }
        spa_hook_remove(&state.listener); spa_hook_remove(&core_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry)); pw_core_disconnect(state.core);
        pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
        throw std::runtime_error("Timed out while enumerating PipeWire sources");
    }
    const auto default_device = state.node_names.find(state.default_source_name);
    if (default_device != state.node_names.end()) {
        for (auto& device : state.devices) device.is_default = device.id == default_device->second;
    }
    for (auto& device : state.devices) {
        const auto parent_id = state.node_device_ids.find(device.id);
        if (parent_id == state.node_device_ids.end()) continue;
        const auto ids = state.device_usb_ids.find(parent_id->second);
        if (ids == state.device_usb_ids.end()) continue;
        if (!device.usb_vendor_id) device.usb_vendor_id = ids->second.first;
        if (!device.usb_product_id) device.usb_product_id = ids->second.second;
    }
    if (state.metadata) {
        spa_hook_remove(&state.metadata_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.metadata));
    }
    spa_hook_remove(&state.listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(state.registry));
    pw_core_disconnect(state.core); pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
    populate_bluez_battery(state.devices);
    return state.devices;
}
}
