#include "clearmic/audio/device.hpp"

#include <iostream>
#include <memory>
#include <exception>
#include <string_view>

#ifdef _WIN32
#include "clearmic/platform/windows/device_manager.hpp"
#elif defined(__linux__)
#include "clearmic/platform/linux/device_manager.hpp"
#endif

namespace {
void print_usage() { std::cout << "ClearMic audio tools\n\nUsage: clearmic-cli devices\n"; }
}

int main(const int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "devices") {
        print_usage();
        return argc == 1 ? 0 : 2;
    }
    std::unique_ptr<clearmic::audio::IAudioDeviceManager> manager;
#ifdef _WIN32
    manager = std::make_unique<clearmic::platform::windows::DeviceManager>();
#elif defined(__linux__)
    manager = std::make_unique<clearmic::platform::pipewire::DeviceManager>();
#else
    std::cerr << "This platform has no ClearMic audio backend.\n";
    return 1;
#endif
    try {
        const auto devices = manager->input_devices();
        if (devices.empty()) { std::cout << "No input devices found.\n"; return 0; }
        std::cout << "ClearMic input devices\n";
        for (const auto& device : devices) {
            std::cout << (device.is_default ? "* " : "  ") << device.name << "\n    ID: " << device.id << "\n";
            if (device.sample_rate_hz) std::cout << "    Sample rate: " << *device.sample_rate_hz << " Hz\n";
            if (device.channels) std::cout << "    Channels: " << *device.channels << "\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "Could not enumerate input devices: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
