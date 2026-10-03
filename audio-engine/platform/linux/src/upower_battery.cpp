#include "upower_battery.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clearmic::platform::pipewire {
namespace {
using BatteryValues = std::unordered_map<std::string, audio::BatteryInfo>;

struct CachedBattery {
    audio::BatteryInfo value;
    std::chrono::steady_clock::time_point observed_at;
};

struct Query {
    std::vector<std::string> addresses;
    BatteryValues values;
    std::unordered_set<std::string> ambiguous_addresses;
    std::size_t pending{};
};

std::mutex battery_mutex;
std::unordered_map<std::string, CachedBattery> battery_cache;
std::chrono::steady_clock::time_point last_query{};
bool query_in_flight{};

std::optional<audio::BatteryInfo> battery_from_properties(GVariant* properties, const std::string& address) {
    gboolean power_supply = TRUE;
    if (!g_variant_lookup(properties, "PowerSupply", "b", &power_supply) || power_supply) return std::nullopt;

    gchar* native_path = nullptr;
    gchar* serial = nullptr;
    const bool has_native_path = g_variant_lookup(properties, "NativePath", "s", &native_path);
    const bool has_serial = g_variant_lookup(properties, "Serial", "s", &serial);
    const bool matched = upower_address_matches(has_native_path && native_path ? native_path : "",
                                                has_serial && serial ? serial : "", address);
    g_free(native_path);
    g_free(serial);
    if (!matched) return std::nullopt;

    guint type = 0;
    gdouble percentage = 0.0;
    guint battery_level = 1U;
    guint state = 0;
    if (!g_variant_lookup(properties, "Type", "u", &type) ||
        !g_variant_lookup(properties, "Percentage", "d", &percentage)) return std::nullopt;
    const bool has_battery_level = g_variant_lookup(properties, "BatteryLevel", "u", &battery_level);
    g_variant_lookup(properties, "State", "u", &state);
    return upower_battery_from_values(false, true, type, percentage,
        has_battery_level ? std::optional<unsigned int>{battery_level} : std::nullopt, state);
}

void finish_query(const std::shared_ptr<Query>& query) {
    std::lock_guard lock(battery_mutex);
    for (const auto& address : query->addresses) battery_cache.erase(address);
    const auto observed_at = std::chrono::steady_clock::now();
    for (const auto& [address, value] : query->values) battery_cache[address] = CachedBattery{value, observed_at};
    query_in_flight = false;
}

void on_device_properties(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<std::pair<std::shared_ptr<Query>, std::string>> context(
        static_cast<std::pair<std::shared_ptr<Query>, std::string>*>(user_data));
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (response) {
        GVariant* properties = g_variant_get_child_value(response, 0);
        if (auto battery = battery_from_properties(properties, context->second)) {
            if (context->first->values.contains(context->second)) {
                context->first->values.erase(context->second);
                context->first->ambiguous_addresses.insert(context->second);
            } else if (!context->first->ambiguous_addresses.contains(context->second)) {
                context->first->values[context->second] = *battery;
            }
        }
        g_variant_unref(properties);
        g_variant_unref(response);
    }
    g_clear_error(&error);
    if (--context->first->pending == 0) finish_query(context->first);
}

void on_devices_enumerated(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<std::shared_ptr<Query>> query_holder(static_cast<std::shared_ptr<Query>*>(user_data));
    auto query = *query_holder;
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!response) {
        g_clear_error(&error);
        finish_query(query);
        return;
    }

    GVariant* paths = g_variant_get_child_value(response, 0);
    GVariantIter iterator;
    g_variant_iter_init(&iterator, paths);
    const gchar* path = nullptr;
    while (g_variant_iter_next(&iterator, "&o", &path)) {
        const std::string object_path(path);
        for (const auto& address : query->addresses) {
            if (!upower_address_matches(object_path, "", address)) continue;
            ++query->pending;
            auto* context = new std::pair<std::shared_ptr<Query>, std::string>{query, address};
            g_dbus_connection_call(G_DBUS_CONNECTION(source), "org.freedesktop.UPower", object_path.c_str(),
                "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", "org.freedesktop.UPower.Device"),
                G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 700, nullptr, on_device_properties, context);
        }
    }
    g_variant_unref(paths);
    g_variant_unref(response);
    if (query->pending == 0) finish_query(query);
}

void on_bus_ready(GObject*, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<std::shared_ptr<Query>> query_holder(static_cast<std::shared_ptr<Query>*>(user_data));
    auto query = *query_holder;
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_finish(result, &error);
    if (!connection) {
        std::lock_guard lock(battery_mutex);
        query_in_flight = false;
        g_clear_error(&error);
        return;
    }
    g_dbus_connection_call(connection, "org.freedesktop.UPower", "/org/freedesktop/UPower",
        "org.freedesktop.UPower", "EnumerateDevices", nullptr, G_VARIANT_TYPE("(ao)"),
        G_DBUS_CALL_FLAGS_NONE, 700, nullptr, on_devices_enumerated,
        new std::shared_ptr<Query>(std::move(query)));
    g_object_unref(connection);
}
}

void populate_upower_battery(std::vector<audio::AudioDevice>& devices) noexcept {
    std::vector<std::string> addresses;
    for (const auto& device : devices) {
        if (device.bluetooth_address &&
            std::find(addresses.begin(), addresses.end(), *device.bluetooth_address) == addresses.end())
            addresses.push_back(*device.bluetooth_address);
    }
    if (addresses.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    bool request = false;
    {
        std::lock_guard lock(battery_mutex);
        for (auto& device : devices) {
            if (!device.bluetooth_address) continue;
            const auto found = battery_cache.find(*device.bluetooth_address);
            if (found != battery_cache.end() && now - found->second.observed_at <= std::chrono::seconds(60))
                device.capabilities.battery = found->second.value;
        }
        if (!query_in_flight && now - last_query >= std::chrono::seconds(30)) {
            query_in_flight = true;
            last_query = now;
            request = true;
        }
    }
    if (!request) return;

    auto query = std::make_shared<Query>();
    query->addresses = std::move(addresses);
    g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, on_bus_ready, new std::shared_ptr<Query>(std::move(query)));
}
}
