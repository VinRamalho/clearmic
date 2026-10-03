#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clearmic::audio {

enum class ConnectionState { connected, disconnected, unknown };
enum class ChargingState { charging, not_charging, full, unknown };

struct BatteryInfo {
    std::optional<unsigned int> percentage;
    ChargingState charging{ChargingState::unknown};
};

struct DeviceCapabilities {
    std::optional<BatteryInfo> battery;
    std::optional<BatteryInfo> transmitter_battery;
    std::optional<BatteryInfo> receiver_battery;
};

struct AudioDevice {
    std::string id;
    std::string name;
    ConnectionState connection{ConnectionState::unknown};
    bool is_default{false};
    std::optional<std::uint32_t> sample_rate_hz;
    std::optional<std::uint32_t> channels;
    std::optional<std::uint16_t> usb_vendor_id;
    std::optional<std::uint16_t> usb_product_id;
    DeviceCapabilities capabilities;
    std::string device_kind{"microphone"};
    bool selectable{true};
    std::optional<std::string> bluetooth_address;
    std::optional<std::string> bluetooth_profile;
    std::optional<std::string> bluetooth_codec;
};

class IAudioDeviceManager {
public:
    virtual ~IAudioDeviceManager() = default;
    [[nodiscard]] virtual std::vector<AudioDevice> input_devices() = 0;
};

[[nodiscard]] bool is_valid_battery_percentage(unsigned int percentage) noexcept;
[[nodiscard]] std::optional<unsigned int> battery_percentage_from_capacity(
    std::uint32_t current_capacity, std::uint32_t full_capacity) noexcept;
[[nodiscard]] std::optional<std::uint16_t> parse_device_identifier(std::string_view value) noexcept;

} // namespace clearmic::audio
