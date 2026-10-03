#include "clearmic/audio/device.hpp"
#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/wav.hpp"

#include <iostream>
#include <memory>
#include <exception>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
#include "clearmic/platform/windows/device_manager.hpp"
#elif defined(__linux__)
#include "clearmic/platform/linux/device_manager.hpp"
#endif

namespace {
void print_usage() {
    std::cout << "ClearMic audio tools\n\n"
                 "Usage:\n"
                 "  clearmic-cli devices\n"
                 "  clearmic-cli process <input.wav> <processed.wav>\n"
#if defined(__linux__) || defined(_WIN32)
                 "  clearmic-cli record-test <seconds> <original.wav> <processed.wav> [device-id]\n"
#endif
                 ;
}
}

int main(const int argc, char** argv) {
#if defined(__linux__) || defined(_WIN32)
    if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "record-test") {
        try {
            const auto seconds = static_cast<std::uint32_t>(std::stoul(argv[2]));
            const std::string device_id = argc == 6 ? argv[5] : "";
#ifdef _WIN32
            auto comparison = clearmic::platform::windows::capture_processed_audio(device_id, seconds);
#else
            auto comparison = clearmic::platform::pipewire::capture_processed_audio(device_id, seconds);
#endif
            clearmic::audio::write_pcm16_wav(argv[3], comparison.original);
            clearmic::audio::write_pcm16_wav(argv[4], comparison.processed);
#ifdef _WIN32
            std::cout << "Recorded " << seconds << " seconds from the selected Windows microphone.\n"
#else
            std::cout << "Recorded " << seconds << " seconds from the selected PipeWire microphone.\n"
#endif
                      << "Original: " << argv[3] << "\nProcessed: " << argv[4] << "\n";
        } catch (const std::exception& error) {
            std::cerr << "Could not record microphone test: " << error.what() << "\n";
            return 1;
        }
        return 0;
    }
#endif
    if (argc == 4 && std::string_view(argv[1]) == "process") {
        try {
            const auto original = clearmic::audio::read_pcm16_wav(argv[2]);
            const auto processed = clearmic::audio::suppress_noise(original);
            clearmic::audio::write_pcm16_wav(argv[3], processed);
            std::cout << "Processed " << original.frame_count() << " frames at " << original.sample_rate_hz
                      << " Hz and wrote " << argv[3] << "\n";
        } catch (const std::exception& error) {
            std::cerr << "Could not process audio: " << error.what() << "\n";
            return 1;
        }
        return 0;
    }
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
