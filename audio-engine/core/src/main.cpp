#include "clearmic/audio/device.hpp"
#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/processor_chain.hpp"
#include "clearmic/audio/wav.hpp"

#include <iostream>
#include <memory>
#include <exception>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>

#ifdef _WIN32
#include "clearmic/platform/windows/device_manager.hpp"
#elif defined(__linux__)
#include "clearmic/platform/linux/device_manager.hpp"
#endif

namespace {
bool set_processing_option(const std::string_view argument, clearmic::audio::ProcessingSettings& settings) {
    auto set_toggle = [&](const std::string_view name, bool& value) {
        const std::string prefix = std::string(name) + "=";
        if (!argument.starts_with(prefix)) return false;
        const auto enabled = argument.substr(prefix.size());
        if (enabled != "on" && enabled != "off")
            throw std::invalid_argument("Processing switches must use on or off");
        value = enabled == "on";
        return true;
    };
    if (set_toggle("--enhancement", settings.enhancement_enabled) ||
        set_toggle("--noise-suppression", settings.noise_suppression_enabled) ||
        set_toggle("--noise-gate", settings.noise_gate_enabled) ||
        set_toggle("--automatic-gain", settings.automatic_gain_enabled) ||
        set_toggle("--compressor", settings.compressor_enabled)) return true;
    if (argument.starts_with("--input-gain-db=")) {
        std::size_t parsed = 0;
        const auto value = std::stof(std::string(argument.substr(16)), &parsed);
        if (parsed != argument.size() - 16 || !std::isfinite(value) || value < -12.0F || value > 12.0F)
            throw std::invalid_argument("Input gain must be between -12 and 12 dB");
        settings.input_gain_db = value;
        return true;
    }
    if (argument.starts_with("--")) throw std::invalid_argument("Unknown processing option: " + std::string(argument));
    return false;
}

bool set_preset(const std::string_view argument, clearmic::audio::Preset& preset,
                clearmic::audio::ProcessingSettings& settings) {
    if (argument != "natural" && argument != "meeting" && argument != "strong") return false;
    preset = argument == "natural" ? clearmic::audio::Preset::natural
        : argument == "meeting" ? clearmic::audio::Preset::meeting
        : clearmic::audio::Preset::strong_noise_reduction;
    settings = clearmic::audio::settings_for_preset(preset);
    return true;
}

void print_usage() {
    std::cout << "ClearMic audio tools\n\n"
                 "Usage:\n"
                 "  clearmic-cli devices\n"
                 "  clearmic-cli process <input.wav> <processed.wav>\n"
#if defined(__linux__) || defined(_WIN32)
                 "  clearmic-cli record-test <seconds> <original.wav> <processed.wav> [device-id] [preset] [processing options]\n"
#endif
#ifdef __linux__
                 "  clearmic-cli serve [device-id] [natural|meeting|strong] [--enhancement=on|off] [--noise-suppression=on|off] [--noise-gate=on|off] [--automatic-gain=on|off] [--compressor=on|off] [--input-gain-db=-12..12]\n"
                 "  clearmic-cli gui\n"
#endif
                 ;
}
}

int main(const int argc, char** argv) {
#ifdef __linux__
    if (argc == 2 && std::string_view(argv[1]) == "gui")
        return clearmic::platform::pipewire::run_desktop_application(argv[0]);
    if (argc >= 2 && argc <= 10 && std::string_view(argv[1]) == "serve") {
        try {
            std::string device_id;
            clearmic::audio::Preset preset = clearmic::audio::Preset::natural;
            bool preset_selected = false;
            auto settings = clearmic::audio::settings_for_preset(preset);
            for (int index = 2; index < argc; ++index) {
                const std::string_view argument(argv[index]);
                if (argument == "natural" || argument == "meeting" || argument == "strong") {
                    if (preset_selected) throw std::invalid_argument("Specify one DSP preset");
                    set_preset(argument, preset, settings);
                    preset_selected = true;
                    continue;
                }
                if (set_processing_option(argument, settings)) continue;
                if (!device_id.empty()) throw std::invalid_argument("Specify one microphone device ID");
                device_id = argument;
            }
            clearmic::platform::pipewire::run_realtime_microphone(device_id, settings);
        } catch (const std::exception& error) {
            std::cerr << "ClearMic audio service stopped: " << error.what() << "\n";
            return 1;
        }
        return 0;
    }
#endif
#if defined(__linux__) || defined(_WIN32)
    if (argc >= 5 && argc <= 14 && std::string_view(argv[1]) == "record-test") {
        try {
            const auto seconds = static_cast<std::uint32_t>(std::stoul(argv[2]));
            std::string device_id;
            auto preset = clearmic::audio::Preset::natural;
            auto settings = clearmic::audio::settings_for_preset(preset);
            bool preset_selected = false;
            for (int index = 5; index < argc; ++index) {
                const std::string_view argument(argv[index]);
                if (argument == "natural" || argument == "meeting" || argument == "strong") {
                    if (preset_selected) throw std::invalid_argument("Specify one DSP preset");
                    set_preset(argument, preset, settings);
                    preset_selected = true;
                } else if (!set_processing_option(argument, settings)) {
                    if (!device_id.empty()) throw std::invalid_argument("Specify one microphone device ID");
                    device_id = argument;
                }
            }
#ifdef _WIN32
            auto comparison = clearmic::platform::windows::capture_processed_audio(device_id, seconds, settings);
#else
            auto comparison = clearmic::platform::pipewire::capture_processed_audio(device_id, seconds, settings);
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
            std::cout << (device.is_default ? "* " : "  ") << device.name << "\n    ID: " << device.id
                      << "\n    Kind: " << device.device_kind
                      << "\n    Selectable: " << (device.selectable ? "yes" : "no") << "\n";
            if (device.sample_rate_hz) std::cout << "    Sample rate: " << *device.sample_rate_hz << " Hz\n";
            if (device.channels) std::cout << "    Channels: " << *device.channels << "\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "Could not enumerate input devices: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
