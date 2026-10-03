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
    AudioDevice device;
    if (device.device_kind != "microphone" || !device.selectable) return 8;
    AudioDevice headset_function;
    headset_function.device_kind = "bluetooth-headset-function";
    headset_function.selectable = false;
    headset_function.connection = ConnectionState::connected;
    if (headset_function.selectable || headset_function.connection != ConnectionState::connected ||
        headset_function.device_kind != "bluetooth-headset-function") return 9;
    if (device.capabilities.battery != std::nullopt) return 2;
    if (device.capabilities.transmitter_battery != std::nullopt) return 3;
    if (device.capabilities.receiver_battery != std::nullopt) return 4;
    BatteryInfo empty_battery;
    empty_battery.percentage = 0;
    if (!is_valid_battery_percentage(*empty_battery.percentage)) return 5;
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
