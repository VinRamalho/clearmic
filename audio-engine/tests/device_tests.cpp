#include "clearmic/audio/device.hpp"
#ifdef __linux__
#include "upower_battery.hpp"
#endif

#include <optional>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

void run_wav_tests();
void run_processor_chain_tests();
#ifdef __linux__
void run_linux_diagnostics_tests();
#endif

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
    if (parse_device_identifier("0x046d") != 0x046d || parse_device_identifier("65535") != 65535 ||
        parse_device_identifier("0x10000") || parse_device_identifier("-1") ||
        parse_device_identifier("0x12xz") || parse_device_identifier("")) return 16;
    AudioDevice device;
    if (device.device_kind != "microphone" || !device.selectable) return 8;
    std::vector<AudioDevice> available_inputs(3);
    available_inputs[0].id = "unavailable-function";
    available_inputs[0].selectable = false;
    available_inputs[0].is_default = true;
    available_inputs[1].id = "usb-microphone";
    available_inputs[2].id = "built-in-microphone";
    available_inputs[2].is_default = true;
    const auto preferred_usb = preferred_input_device_index(available_inputs, "usb-microphone");
    const auto missing_default = preferred_input_device_index(available_inputs, "missing-microphone");
    const auto unavailable_default = preferred_input_device_index(available_inputs, "unavailable-function");
    const auto empty_default = preferred_input_device_index(available_inputs, "");
    if (!preferred_usb || *preferred_usb != 1 || !missing_default || *missing_default != 2 ||
        !unavailable_default || *unavailable_default != 2 || !empty_default || *empty_default != 2) return 23;
    available_inputs[2].is_default = false;
    const auto first_fallback = preferred_input_device_index(available_inputs, "missing-microphone");
    if (!first_fallback || *first_fallback != 1) return 24;
    available_inputs[1].selectable = false;
    const auto unavailable_fallback = preferred_input_device_index(available_inputs, "missing-microphone");
    const std::vector<AudioDevice> no_inputs;
    const auto empty_fallback = preferred_input_device_index(no_inputs, "missing-microphone");
    if (unavailable_fallback.has_value() || empty_fallback.has_value()) return 25;
    if (device.usb_vendor_id || device.usb_product_id) return 17;
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
#ifdef __linux__
    using clearmic::platform::pipewire::upower_battery_from_values;
    using clearmic::platform::pipewire::upower_address_matches;
    if (!upower_address_matches("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", "", "aa:bb:cc:dd:ee:ff") ||
        !upower_address_matches("", "AA-BB-CC-DD-EE-FF", "aa:bb:cc:dd:ee:ff") ||
        upower_address_matches("/org/bluez/hci0/dev_11_22_33_44_55_66", "11:22:33:44:55:66", "aa:bb:cc:dd:ee:ff")) return 22;
    const auto charging_battery = upower_battery_from_values(false, true, 17, 82.6, 1U, 1U);
    if (!charging_battery || charging_battery->percentage != 83 ||
        charging_battery->charging != ChargingState::charging) return 18;
    const auto full_battery = upower_battery_from_values(false, true, 19, 100.0, 1U, 4U);
    if (!full_battery || full_battery->charging != ChargingState::full) return 19;
    if (upower_battery_from_values(true, true, 17, 82.0, 1U, 1U) ||
        upower_battery_from_values(false, false, 17, 82.0, 1U, 1U) ||
        upower_battery_from_values(false, true, 11, 82.0, 1U, 1U) ||
        upower_battery_from_values(false, true, 17, 82.0, 4U, 1U) ||
        upower_battery_from_values(false, true, 17, 101.0, 1U, 1U) ||
        upower_battery_from_values(false, true, 17, std::numeric_limits<double>::quiet_NaN(), 1U, 1U)) return 20;
    const auto unknown_state = upower_battery_from_values(false, true, 28, 0.0, std::nullopt, 0U);
    if (!unknown_state || unknown_state->percentage != 0 ||
        unknown_state->charging != ChargingState::unknown) return 21;
#endif
    if (is_valid_battery_percentage(101)) return 6;
    try {
        run_wav_tests();
        run_processor_chain_tests();
#ifdef __linux__
        run_linux_diagnostics_tests();
#endif
    } catch (const std::exception& error) {
        const std::string message = "ClearMic test failure: " + std::string(error.what());
        std::cerr << message << '\n';
        if (std::getenv("GITHUB_ACTIONS") != nullptr)
            std::cout << "::error file=audio-engine/tests/wav_tests.cpp::" << message << '\n';
        return 7;
    }
    return 0;
}
