#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
    DeviceCapabilities capabilities;
};

class IAudioDeviceManager {
public:
    virtual ~IAudioDeviceManager() = default;
    [[nodiscard]] virtual std::vector<AudioDevice> input_devices() = 0;
};

[[nodiscard]] bool is_valid_battery_percentage(unsigned int percentage) noexcept;

} // namespace clearmic::audio
