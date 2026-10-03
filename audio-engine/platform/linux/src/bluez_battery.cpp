#include "bluez_battery.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace clearmic::platform::pipewire {
namespace {
std::string bluez_device_suffix(const std::string& address) {
    std::string suffix = "dev_";
    suffix.reserve(4 + address.size());
    for (const unsigned char character : address) {
        if (character == ':' || character == '-') suffix.push_back('_');
        else suffix.push_back(static_cast<char>(std::toupper(character)));
    }
    return suffix;
}

using BatteryValues = std::unordered_map<std::string, unsigned int>;
struct CachedBattery {
    unsigned int percentage{};
    std::chrono::steady_clock::time_point observed_at;
};

struct Query {
    std::vector<std::string> addresses;
};

std::mutex battery_mutex;
std::unordered_map<std::string, CachedBattery> battery_cache;
std::chrono::steady_clock::time_point last_query{};
bool query_in_flight{};

void collect_battery_reports(GVariant* objects, const std::vector<std::string>& addresses, BatteryValues& result) {
    GVariantIter iterator;
    g_variant_iter_init(&iterator, objects);
    const gchar* object_path = nullptr;
    GVariant* interfaces = nullptr;
    while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &object_path, &interfaces)) {
        const auto path = std::string(object_path);
        for (const auto& address : addresses) {
            if (!path.ends_with(bluez_device_suffix(address))) continue;
            GVariant* properties = g_variant_lookup_value(interfaces, "org.bluez.Battery1", G_VARIANT_TYPE("a{sv}"));
            if (!properties) continue;
            GVariant* percentage_value = g_variant_lookup_value(properties, "Percentage", G_VARIANT_TYPE_BYTE);
            if (percentage_value) {
                const auto percentage = static_cast<unsigned int>(g_variant_get_byte(percentage_value));
                if (audio::is_valid_battery_percentage(percentage)) result[address] = percentage;
                g_variant_unref(percentage_value);
            }
            g_variant_unref(properties);
        }
        g_variant_unref(interfaces);
    }
}

void on_battery_query(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<Query> query(static_cast<Query*>(user_data));
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (response) {
        GVariant* objects = g_variant_get_child_value(response, 0);
        BatteryValues values;
        collect_battery_reports(objects, query->addresses, values);
        {
            std::lock_guard lock(battery_mutex);
            for (const auto& address : query->addresses) battery_cache.erase(address);
            const auto observed_at = std::chrono::steady_clock::now();
            for (const auto& [address, percentage] : values)
                battery_cache[address] = CachedBattery{percentage, observed_at};
            query_in_flight = false;
        }
        g_variant_unref(objects);
        g_variant_unref(response);
    } else {
        std::lock_guard lock(battery_mutex);
        query_in_flight = false;
    }
    g_clear_error(&error);
}

void on_bus_ready(GObject*, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<Query> query(static_cast<Query*>(user_data));
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_finish(result, &error);
    if (!connection) {
        std::lock_guard lock(battery_mutex);
        query_in_flight = false;
        g_clear_error(&error);
        return;
    }
    auto* pending = query.release();
    g_dbus_connection_call(connection, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 700, nullptr,
        on_battery_query, pending);
    g_object_unref(connection);
}
}

void populate_bluez_battery(std::vector<audio::AudioDevice>& devices) noexcept {
    std::vector<std::string> addresses;
    for (const auto& device : devices) {
        if (device.bluetooth_address) addresses.push_back(*device.bluetooth_address);
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
                device.capabilities.battery = audio::BatteryInfo{found->second.percentage, audio::ChargingState::unknown};
        }
        if (!query_in_flight && now - last_query >= std::chrono::seconds(30)) {
            query_in_flight = true;
            last_query = now;
            request = true;
        }
    }
    if (!request) return;
    auto* query = new Query{std::move(addresses)};
    g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, on_bus_ready, query);
}
}
