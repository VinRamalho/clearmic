#include "clearmic/audio/device.hpp"

#include <optional>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <string>

void run_wav_tests();
void run_processor_chain_tests();

int main() {
    using namespace clearmic::audio;
    if (!is_valid_battery_percentage(0) || !is_valid_battery_percentage(100) || is_valid_battery_percentage(101)) return 1;
    if (battery_percentage_from_capacity(0, 100) != 0 ||
        battery_percentage_from_capacity(1, 3) != 33 ||
        battery_percentage_from_capacity(100, 100) != 100 ||
        battery_percentage_from_capacity(120, 100) != 100 ||
        battery_percentage_from_capacity(10, 0) ||
        battery_percentage_from_capacity(0xffffffffU, 100) ||
        battery_percentage_from_capacity(10, 0xffffffffU)) return 15;
    AudioDevice device;
    if (device.device_kind != "microphone" || !device.selectable) return 8;
    AudioDevice headset_function;
    headset_function.device_kind = "bluetooth-headset-function";
    headset_function.selectable = false;
    headset_function.connection = ConnectionState::connected;
    if (headset_function.selectable || headset_function.connection != ConnectionState::connected ||
        headset_function.device_kind != "bluetooth-headset-function") return 9;
    if (device.bluetooth_profile || device.bluetooth_codec) return 13;
    device.bluetooth_profile = "hfp-hf";
    device.bluetooth_codec = "msbc";
    if (!device.bluetooth_profile || *device.bluetooth_profile != "hfp-hf" ||
        !device.bluetooth_codec || *device.bluetooth_codec != "msbc") return 14;
    if (device.capabilities.battery != std::nullopt) return 2;
    if (device.capabilities.transmitter_battery != std::nullopt) return 3;
    if (device.capabilities.receiver_battery != std::nullopt) return 4;
    const BatteryInfo unavailable_battery;
    if (unavailable_battery.percentage.has_value() ||
        unavailable_battery.charging != ChargingState::unknown) return 10;
    const BatteryInfo reported_zero_battery{0, ChargingState::not_charging};
    if (!reported_zero_battery.percentage.has_value() || *reported_zero_battery.percentage != 0 ||
        !is_valid_battery_percentage(*reported_zero_battery.percentage) ||
        reported_zero_battery.charging != ChargingState::not_charging) return 11;
    device.capabilities.battery = reported_zero_battery;
    if (!device.capabilities.battery || *device.capabilities.battery->percentage != 0) return 12;
    if (is_valid_battery_percentage(101)) return 6;
    try {
        run_wav_tests();
        run_processor_chain_tests();
    } catch (const std::exception& error) {
        const std::string message = "ClearMic test failure: " + std::string(error.what());
        std::cerr << message << '\n';
        if (std::getenv("GITHUB_ACTIONS") != nullptr)
            std::cout << "::error file=audio-engine/tests/wav_tests.cpp::" << message << '\n';
        return 7;
    }
    return 0;
}
