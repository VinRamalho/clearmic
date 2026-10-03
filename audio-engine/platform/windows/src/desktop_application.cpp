#include "clearmic/platform/windows/device_manager.hpp"
#include "clearmic/platform/windows/virtual_cable.hpp"

#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/wav.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace clearmic::platform::windows {
namespace {
constexpr wchar_t window_class[] = L"ClearMicDesktopWindow";
constexpr UINT capture_complete_message = WM_APP + 1;
constexpr int microphone_combo = 100;
constexpr int preset_combo = 101;
constexpr int noise_suppression_check = 102;
constexpr int noise_gate_check = 103;
constexpr int automatic_gain_check = 104;
constexpr int compressor_check = 105;
constexpr int input_gain_slider = 106;
constexpr int record_button = 107;
constexpr int play_original_button = 108;
constexpr int play_processed_button = 109;
constexpr int stop_playback_button = 110;
constexpr int device_status_label = 111;
constexpr int battery_status_label = 112;
constexpr int input_meter = 113;
constexpr int output_meter = 114;
constexpr int status_label = 115;
constexpr int enhancement_status_label = 116;
constexpr int gain_value_label = 117;
constexpr int device_id_label = 118;
constexpr int enhancement_check = 119;
constexpr int render_output_combo = 120;
constexpr int live_route_button = 121;
constexpr int virtual_cable_help_button = 122;
constexpr int input_meter_caption = 123;
constexpr UINT live_route_complete_message = WM_APP + 2;
constexpr UINT live_route_started_message = WM_APP + 3;
constexpr UINT tray_callback_message = WM_APP + 4;
constexpr UINT device_refresh_timer = 3;
constexpr UINT input_meter_timer = 4;
constexpr int tray_open_command = 201;
constexpr int tray_route_command = 202;
constexpr int tray_quit_command = 203;
constexpr UINT tray_icon_id = 1;

struct RecordCompletion {
    bool success{};
    std::wstring message;
    double input_rms{};
    double output_rms{};
};

struct LiveRouteCompletion {
    std::wstring message;
};

struct Application {
    HWND window{};
    HWND microphone{};
    HWND preset{};
    HWND noise_suppression{};
    HWND noise_gate{};
    HWND automatic_gain{};
    HWND compressor{};
    HWND input_gain{};
    HWND record{};
    HWND play_original{};
    HWND play_processed{};
    HWND stop_playback{};
    HWND device_status{};
    HWND battery_status{};
    HWND input_level{};
    HWND output_level{};
    HWND status{};
    HWND enhancement_status{};
    HWND gain_value{};
    HWND device_id{};
    HWND input_level_caption{};
    HWND enhancement{};
    HWND render_output{};
    HWND live_route{};
    HWND virtual_cable_help{};
    std::vector<audio::AudioDevice> devices;
    std::vector<audio::AudioDevice> render_devices;
    std::wstring settings_file;
    std::filesystem::path original_file;
    std::filesystem::path processed_file;
    std::thread capture_thread;
    std::thread live_route_thread;
    std::atomic_bool shutting_down{};
    std::atomic_bool stop_live_route{};
    LiveProcessingMetrics live_metrics;
    bool recording{};
    bool live_routing{};
    bool refreshing_devices{};
    bool samples_ready{};
    bool tray_icon_added{};
};

void toggle_live_route(Application& app);

void show_window(Application& app) {
    ShowWindow(app.window, SW_SHOW);
    ShowWindow(app.window, SW_RESTORE);
    SetForegroundWindow(app.window);
}

void add_tray_icon(Application& app) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app.window;
    icon.uID = tray_icon_id;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = tray_callback_message;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(icon.szTip, L"ClearMic", static_cast<int>(std::size(icon.szTip)));
    app.tray_icon_added = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

void remove_tray_icon(Application& app) {
    if (!app.tray_icon_added) return;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app.window;
    icon.uID = tray_icon_id;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    app.tray_icon_added = false;
}

void show_tray_menu(Application& app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, tray_open_command, L"Open ClearMic");
    AppendMenuW(menu, MF_STRING, tray_route_command,
                app.live_routing ? L"Stop live processing" : L"Start live processing");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, tray_quit_command, L"Quit ClearMic");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(app.window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    PostMessageW(app.window, WM_NULL, 0, 0);
    if (command == tray_open_command) {
        show_window(app);
    } else if (command == tray_route_command) {
        toggle_live_route(app);
        if (!app.live_routing) show_window(app);
    } else if (command == tray_quit_command) {
        SendMessageW(app.window, WM_CLOSE, 1, 0);
    }
}

std::wstring known_folder(const KNOWNFOLDERID& folder) {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(folder, KF_FLAG_CREATE, nullptr, &raw_path);
    if (FAILED(result)) return {};
    std::wstring path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

std::wstring configuration_file() {
    const auto root = known_folder(FOLDERID_RoamingAppData);
    if (root.empty()) return {};
    const auto directory = std::filesystem::path(root) / L"ClearMic";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / L"settings.ini").wstring();
}

std::filesystem::path recordings_directory() {
    const auto root = known_folder(FOLDERID_LocalAppData);
    if (root.empty()) return {};
    return std::filesystem::path(root) / L"ClearMic" / L"ab-tests";
}

std::wstring read_setting(const Application& app, const wchar_t* section, const wchar_t* key,
                          const wchar_t* fallback = L"") {
    std::array<wchar_t, 2048> value{};
    GetPrivateProfileStringW(section, key, fallback, value.data(), static_cast<DWORD>(value.size()),
                             app.settings_file.c_str());
    return value.data();
}

void write_setting(const Application& app, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, app.settings_file.c_str());
}

void write_integer_setting(const Application& app, const wchar_t* key, const int value) {
    const auto text = std::to_wstring(value);
    write_setting(app, L"processing", key, text.c_str());
}

bool setting_enabled(const Application& app, const wchar_t* key, const bool fallback) {
    const auto value = read_setting(app, L"processing", key, fallback ? L"1" : L"0");
    return value == L"1" || value == L"true";
}

std::wstring to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), count);
    result.pop_back();
    return result;
}

void add_control(Application& app, const wchar_t* class_name, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, const int id = 0, const DWORD extended_style = 0) {
    CreateWindowExW(extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
                    x, y, width, height, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
}

void add_label(Application& app, const wchar_t* text, int x, int y, int width = 670, int height = 22,
               const int id = 0) {
    add_control(app, L"STATIC", text, SS_LEFT, x, y, width, height, id);
}

int selected_device_index(const Application& app) {
    const LRESULT row = SendMessageW(app.microphone, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return -1;
    const LRESULT index = SendMessageW(app.microphone, CB_GETITEMDATA, static_cast<WPARAM>(row), 0);
    return index >= 0 && static_cast<std::size_t>(index) < app.devices.size() ? static_cast<int>(index) : -1;
}

int selected_render_device_index(const Application& app) {
    const LRESULT row = SendMessageW(app.render_output, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return -1;
    const LRESULT index = SendMessageW(app.render_output, CB_GETITEMDATA, static_cast<WPARAM>(row), 0);
    return index >= 0 && static_cast<std::size_t>(index) < app.render_devices.size() ? static_cast<int>(index) : -1;
}

bool is_virtual_cable_output(const audio::AudioDevice& device) {
    return is_virtual_cable_output_name(to_wide(device.name));
}

bool is_virtual_cable_capture(const audio::AudioDevice& device) {
    return is_virtual_cable_capture_name(to_wide(device.name));
}

bool has_paired_virtual_capture(const Application& app, const audio::AudioDevice& output) {
    const auto paired_name = paired_capture_name(normalize_endpoint_name(to_wide(output.name)));
    if (paired_name.empty()) return false;
    return std::any_of(app.devices.begin(), app.devices.end(), [&](const auto& device) {
        return device.connection == audio::ConnectionState::connected &&
               normalize_endpoint_name(to_wide(device.name)).find(paired_name) != std::wstring::npos;
    });
}

void update_device_status(Application& app) {
    const int index = selected_device_index(app);
    if (index < 0) {
        SetWindowTextW(app.device_status, L"No active Windows microphone endpoint is available.");
        SetWindowTextW(app.battery_status, L"Battery: Not available");
        SetWindowTextW(app.device_id, L"Diagnostics · Backend: WASAPI · Device, format, and latency unavailable");
        return;
    }
    const auto& device = app.devices[static_cast<std::size_t>(index)];
    std::wstring details = device.connection == audio::ConnectionState::connected ? L"Connected"
        : (device.connection == audio::ConnectionState::disconnected ? L"Disconnected" : L"Status unknown");
    if (device.is_default) details += L" · Default microphone";
    if (device.sample_rate_hz) details += L" · " + std::to_wstring(*device.sample_rate_hz) + L" Hz";
    if (device.channels) details += L" · " + std::to_wstring(*device.channels) + L" ch";
    SetWindowTextW(app.device_status, details.c_str());

    auto battery_text = [](const wchar_t* label, const std::optional<audio::BatteryInfo>& battery) {
        if (!battery || !battery->percentage) return std::wstring{};
        std::wstring value = std::wstring(label) + L": " + std::to_wstring(*battery->percentage) + L"%";
        if (battery->charging == audio::ChargingState::charging) value += L" (charging)";
        else if (battery->charging == audio::ChargingState::full) value += L" (full)";
        else if (battery->charging == audio::ChargingState::not_charging) value += L" (not charging)";
        return value;
    };
    std::wstring batteries;
    for (const auto& value : {battery_text(L"Battery", device.capabilities.battery),
                              battery_text(L"Transmitter", device.capabilities.transmitter_battery),
                              battery_text(L"Receiver", device.capabilities.receiver_battery)}) {
        if (value.empty()) continue;
        if (!batteries.empty()) batteries += L"   ";
        batteries += value;
    }
    if (batteries.empty()) batteries = L"Battery: Not available";
    SetWindowTextW(app.battery_status, batteries.c_str());
    const auto id = to_wide(device.id);
    std::wstring diagnostic = L"Diagnostics · Backend: WASAPI";
    diagnostic += L" · Device ID: " + id;
    diagnostic += L" · Mix format: ";
    diagnostic += device.sample_rate_hz ? std::to_wstring(*device.sample_rate_hz) + L" Hz" : L"unavailable";
    diagnostic += L" / ";
    diagnostic += device.channels ? std::to_wstring(*device.channels) + L" channels" : L"channel count unavailable";
    if (device.usb_vendor_id && device.usb_product_id) {
        std::wostringstream usb_ids;
        usb_ids << L" · USB VID:PID " << std::uppercase << std::hex << std::setfill(L'0')
                << std::setw(4) << *device.usb_vendor_id << L':' << std::setw(4) << *device.usb_product_id;
        diagnostic += usb_ids.str();
    }
    diagnostic += L" · Buffer/latency: not measured";
    SetWindowTextW(app.device_id, diagnostic.c_str());
}

void update_live_diagnostics(Application& app) {
    const int index = selected_device_index(app);
    if (index < 0) return;
    const auto& device = app.devices[static_cast<std::size_t>(index)];
    std::wstring diagnostic = L"Diagnostics · Backend: WASAPI · Device ID: " + to_wide(device.id);
    diagnostic += L" · Mix format: ";
    diagnostic += device.sample_rate_hz ? std::to_wstring(*device.sample_rate_hz) + L" Hz" : L"unavailable";
    diagnostic += L" / ";
    diagnostic += device.channels ? std::to_wstring(*device.channels) + L" channels" : L"channel count unavailable";
    const auto capture_frames = app.live_metrics.capture_buffer_frames.load(std::memory_order_relaxed);
    const auto render_frames = app.live_metrics.render_buffer_frames.load(std::memory_order_relaxed);
    diagnostic += L" · Buffers: capture " + std::to_wstring(capture_frames) + L" / render " +
                  std::to_wstring(render_frames) + L" frames";
    diagnostic += L" · WASAPI latency: capture ";
    diagnostic += app.live_metrics.capture_latency_available.load(std::memory_order_relaxed)
        ? std::to_wstring(app.live_metrics.capture_latency_ms.load(std::memory_order_relaxed)) + L" ms"
        : L"unavailable";
    diagnostic += L" / render ";
    diagnostic += app.live_metrics.render_latency_available.load(std::memory_order_relaxed)
        ? std::to_wstring(app.live_metrics.render_latency_ms.load(std::memory_order_relaxed)) + L" ms"
        : L"unavailable";
    diagnostic += L" · Max DSP processing per capture packet: ";
    diagnostic += app.live_metrics.processing_time_available.load(std::memory_order_relaxed)
        ? std::to_wstring(app.live_metrics.max_processing_packet_ms.load(std::memory_order_relaxed)) + L" ms"
        : L"measuring";
    SetWindowTextW(app.device_id, diagnostic.c_str());
}

void update_idle_input_meter(Application& app) {
    const int index = selected_device_index(app);
    std::optional<float> peak;
    try {
        if (index >= 0) peak = DeviceManager{}.input_peak_level(app.devices[static_cast<std::size_t>(index)].id);
    } catch (...) {
        peak.reset();
    }
    if (!peak) {
        SetWindowTextW(app.input_level_caption, L"Input level unavailable");
        SendMessageW(app.input_level, PBM_SETPOS, 0, 0);
        return;
    }
    SetWindowTextW(app.input_level_caption, L"Input level");
    SendMessageW(app.input_level, PBM_SETPOS,
                 static_cast<WPARAM>(std::clamp(*peak * 100.0F, 0.0F, 100.0F)), 0);
}

void update_controls(Application& app) {
    const bool has_device = selected_device_index(app) >= 0;
    const int render_index = selected_render_device_index(app);
    const bool has_virtual_output = render_index >= 0 &&
        is_virtual_cable_output(app.render_devices[static_cast<std::size_t>(render_index)]) &&
        has_paired_virtual_capture(app, app.render_devices[static_cast<std::size_t>(render_index)]);
    const bool idle = !app.recording && !app.live_routing;
    const bool has_virtual_cable = std::any_of(app.render_devices.begin(), app.render_devices.end(),
        [](const auto& device) { return is_virtual_cable_output(device); });
    EnableWindow(app.record, has_device && idle);
    EnableWindow(app.microphone, idle);
    EnableWindow(app.render_output, idle);
    EnableWindow(app.preset, idle);
    EnableWindow(app.noise_suppression, idle);
    EnableWindow(app.noise_gate, idle);
    EnableWindow(app.automatic_gain, idle);
    EnableWindow(app.compressor, idle);
    EnableWindow(app.enhancement, idle);
    EnableWindow(app.input_gain, idle);
    EnableWindow(app.play_original, idle && app.samples_ready);
    EnableWindow(app.play_processed, idle && app.samples_ready);
    SetWindowTextW(app.live_route, app.live_routing ? L"Stop live routing" : L"Start live routing");
    EnableWindow(app.live_route, has_device && (app.live_routing ||
        (!app.recording && has_virtual_output)));
    EnableWindow(app.virtual_cable_help, idle && !has_virtual_cable);
}

void refresh_devices(Application& app) {
    if (app.recording || app.live_routing || app.refreshing_devices) return;
    app.refreshing_devices = true;
    const auto preferred = read_setting(app, L"audio", L"input-device");
    const auto preferred_render = read_setting(app, L"audio", L"render-device");
    try {
        app.devices = DeviceManager{}.input_devices();
        app.render_devices = DeviceManager{}.output_devices();
    } catch (const std::exception& error) {
        SetWindowTextW(app.status, to_wide(error.what()).c_str());
        app.devices.clear();
        app.render_devices.clear();
    }
    SendMessageW(app.microphone, CB_RESETCONTENT, 0, 0);
    int preferred_row = -1;
    int default_row = -1;
    int row = 0;
    for (std::size_t index = 0; index < app.devices.size(); ++index) {
        const auto& device = app.devices[index];
        if (!device.selectable || is_virtual_cable_capture(device)) continue;
        std::wstring label = to_wide(device.name);
        if (device.is_default) label += L" (default)";
        const LRESULT added = SendMessageW(app.microphone, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(label.c_str()));
        if (added == CB_ERR || added == CB_ERRSPACE) continue;
        SendMessageW(app.microphone, CB_SETITEMDATA, static_cast<WPARAM>(added), static_cast<LPARAM>(index));
        if (to_wide(device.id) == preferred) preferred_row = row;
        if (device.is_default) default_row = row;
        ++row;
    }
    const int active_row = preferred_row >= 0 ? preferred_row : (default_row >= 0 ? default_row : (row > 0 ? 0 : -1));
    if (active_row >= 0) {
        SendMessageW(app.microphone, CB_SETCURSEL, static_cast<WPARAM>(active_row), 0);
        if (preferred_row < 0) {
            const LRESULT index = SendMessageW(app.microphone, CB_GETITEMDATA,
                                                static_cast<WPARAM>(active_row), 0);
            if (index >= 0 && static_cast<std::size_t>(index) < app.devices.size()) {
                const auto id = to_wide(app.devices[static_cast<std::size_t>(index)].id);
                write_setting(app, L"audio", L"input-device", id.c_str());
            }
        }
    }

    SendMessageW(app.render_output, CB_RESETCONTENT, 0, 0);
    SendMessageW(app.render_output, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Choose a virtual-cable playback endpoint"));
    SendMessageW(app.render_output, CB_SETITEMDATA, 0, static_cast<LPARAM>(-1));
    int preferred_render_row = -1;
    int virtual_output_count = 0;
    for (std::size_t index = 0; index < app.render_devices.size(); ++index) {
        const auto& device = app.render_devices[index];
        const bool virtual_cable = is_virtual_cable_output(device);
        if (!virtual_cable) continue;
        ++virtual_output_count;
        std::wstring label = to_wide(device.name);
        if (device.is_default) label += L" (default playback)";
        const LRESULT added = SendMessageW(app.render_output, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>(label.c_str()));
        if (added == CB_ERR || added == CB_ERRSPACE) continue;
        SendMessageW(app.render_output, CB_SETITEMDATA, static_cast<WPARAM>(added), static_cast<LPARAM>(index));
        if (virtual_cable && to_wide(device.id) == preferred_render) preferred_render_row = static_cast<int>(added);
    }
    SendMessageW(app.render_output, CB_SETCURSEL,
                 static_cast<WPARAM>(preferred_render_row >= 0 ? preferred_render_row : 0), 0);
    app.refreshing_devices = false;
    SetWindowTextW(app.enhancement_status, virtual_output_count > 0
        ? L"Virtual cable detected. Its paired recording endpoint must also be active before routing."
        : L"No compatible virtual audio driver. VB-CABLE setup needs admin rights and a Windows restart.");
    update_device_status(app);
    update_controls(app);
}

audio::Preset selected_preset(const Application& app) {
    const LRESULT row = SendMessageW(app.preset, CB_GETCURSEL, 0, 0);
    if (row == 1) return audio::Preset::meeting;
    if (row == 2) return audio::Preset::strong_noise_reduction;
    return audio::Preset::natural;
}

audio::ProcessingSettings current_settings(const Application& app) {
    auto settings = audio::settings_for_preset(selected_preset(app));
    settings.enhancement_enabled = SendMessageW(app.enhancement, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.noise_suppression_enabled = SendMessageW(app.noise_suppression, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.noise_gate_enabled = SendMessageW(app.noise_gate, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.automatic_gain_enabled = SendMessageW(app.automatic_gain, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.compressor_enabled = SendMessageW(app.compressor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.input_gain_db = static_cast<float>(SendMessageW(app.input_gain, TBM_GETPOS, 0, 0));
    return settings;
}

void save_processing_settings(Application& app) {
    const auto settings = current_settings(app);
    write_setting(app, L"processing", L"preset",
        selected_preset(app) == audio::Preset::meeting ? L"meeting" :
        (selected_preset(app) == audio::Preset::strong_noise_reduction ? L"strong" : L"natural"));
    write_setting(app, L"processing", L"noise-suppression", settings.noise_suppression_enabled ? L"1" : L"0");
    write_setting(app, L"processing", L"noise-gate", settings.noise_gate_enabled ? L"1" : L"0");
    write_setting(app, L"processing", L"automatic-gain", settings.automatic_gain_enabled ? L"1" : L"0");
    write_setting(app, L"processing", L"compressor", settings.compressor_enabled ? L"1" : L"0");
    write_setting(app, L"processing", L"enhancement-enabled", settings.enhancement_enabled ? L"1" : L"0");
    write_integer_setting(app, L"input-gain-db", static_cast<int>(settings.input_gain_db));
}

void begin_recording(Application& app) {
    const int index = selected_device_index(app);
    if (index < 0 || app.recording) return;
    const auto device_id = app.devices[static_cast<std::size_t>(index)].id;
    const auto settings = current_settings(app);
    const auto directory = recordings_directory();
    app.original_file = directory / L"original.wav";
    app.processed_file = directory / L"processed.wav";
    app.samples_ready = false;
    app.recording = true;
    SetWindowTextW(app.status, L"Recording five seconds. Speak into the selected microphone...");
    update_controls(app);
    const HWND window = app.window;
    try {
        app.capture_thread = std::thread([&app, window, device_id, settings, directory] {
            auto* completion = new RecordCompletion;
            try {
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                if (error) throw std::system_error(error, "Create local A/B recording folder");
                CaptureDiagnostics diagnostics;
                auto comparison = capture_processed_audio(device_id, 5, settings, &diagnostics);
                audio::write_pcm16_wav(app.original_file, comparison.original);
                audio::write_pcm16_wav(app.processed_file, comparison.processed);
                completion->input_rms = audio::rms_normalized(comparison.original);
                completion->output_rms = audio::rms_normalized(comparison.processed);
                completion->success = true;
                const auto input_percent = std::to_wstring(completion->input_rms * 100.0);
                const auto output_percent = std::to_wstring(completion->output_rms * 100.0);
                auto trim_percent = [](std::wstring value) {
                    const auto decimal = value.find(L'.');
                    if (decimal != std::wstring::npos) value.resize(decimal + 2);
                    return value + L"%";
                };
                completion->message = completion->input_rms < 0.001
                    ? L"Little or no microphone signal was detected, so these diagnostic samples may be silent or too quiet for a useful A/B comparison. Check mute, Windows microphone privacy, and the selected input."
                    : L"A/B sample ready. Microphone activity was detected.";
                completion->message += L" Input RMS: " + trim_percent(input_percent) +
                    L" · processed RMS: " + trim_percent(output_percent);
                if (diagnostics.buffer_frames)
                    completion->message += L" · WASAPI buffer: " + std::to_wstring(*diagnostics.buffer_frames) + L" frames";
                if (diagnostics.stream_latency_ms) {
                    auto latency = std::to_wstring(*diagnostics.stream_latency_ms);
                    const auto decimal = latency.find(L'.');
                    if (decimal != std::wstring::npos) latency.resize(decimal + 2);
                    completion->message += L" · WASAPI stream latency: " + latency + L" ms";
                } else completion->message += L" · WASAPI stream latency unavailable";
                if (diagnostics.processing_wall_time_ms) {
                    auto processing = std::to_wstring(*diagnostics.processing_wall_time_ms);
                    const auto decimal = processing.find(L'.');
                    if (decimal != std::wstring::npos) processing.resize(decimal + 2);
                    completion->message += L" · A/B DSP time for 5 seconds: " + processing + L" ms";
                }
                completion->message += L". Use the playback buttons to compare the recordings.";
            } catch (const std::exception& error) {
                completion->message = to_wide(error.what());
            }
            if (app.shutting_down.load(std::memory_order_relaxed) ||
                !PostMessageW(window, capture_complete_message, 0, reinterpret_cast<LPARAM>(completion)))
                delete completion;
        });
    } catch (const std::exception& error) {
        app.recording = false;
        SetWindowTextW(app.status, to_wide(error.what()).c_str());
        update_controls(app);
    }
}

void toggle_live_route(Application& app) {
    if (app.live_routing) {
        app.stop_live_route.store(true, std::memory_order_relaxed);
        SetWindowTextW(app.status, L"Stopping the live microphone route...");
        return;
    }
    if (app.recording) return;
    const int input_index = selected_device_index(app);
    const int output_index = selected_render_device_index(app);
    if (input_index < 0 || output_index < 0) {
        SetWindowTextW(app.status, L"Select a microphone and a compatible virtual playback endpoint first.");
        return;
    }
    const auto input_id = app.devices[static_cast<std::size_t>(input_index)].id;
    const auto output_id = app.render_devices[static_cast<std::size_t>(output_index)].id;
    const auto output_name = to_wide(app.render_devices[static_cast<std::size_t>(output_index)].name);
    if (!is_virtual_cable_output(app.render_devices[static_cast<std::size_t>(output_index)])) {
        SetWindowTextW(app.status,
            L"Choose a compatible virtual playback endpoint to avoid sending processed microphone audio to speakers.");
        return;
    }
    if (!has_paired_virtual_capture(app, app.render_devices[static_cast<std::size_t>(output_index)])) {
        SetWindowTextW(app.status,
            L"The selected virtual driver has no active paired recording endpoint. Check the driver and refresh devices.");
        return;
    }
    const auto settings = current_settings(app);
    app.stop_live_route.store(false, std::memory_order_relaxed);
    app.live_metrics.input_rms.store(0.0F, std::memory_order_relaxed);
    app.live_metrics.output_rms.store(0.0F, std::memory_order_relaxed);
    app.live_metrics.capture_buffer_frames.store(0, std::memory_order_relaxed);
    app.live_metrics.render_buffer_frames.store(0, std::memory_order_relaxed);
    app.live_metrics.capture_latency_available.store(false, std::memory_order_relaxed);
    app.live_metrics.render_latency_available.store(false, std::memory_order_relaxed);
    app.live_metrics.capture_latency_ms.store(0.0F, std::memory_order_relaxed);
    app.live_metrics.render_latency_ms.store(0.0F, std::memory_order_relaxed);
    app.live_metrics.processing_time_available.store(false, std::memory_order_relaxed);
    app.live_metrics.max_processing_packet_ms.store(0.0F, std::memory_order_relaxed);
    update_live_diagnostics(app);
    app.live_routing = true;
    SendMessageW(app.input_level, PBM_SETPOS, 0, 0);
    SendMessageW(app.output_level, PBM_SETPOS, 0, 0);
    SetWindowTextW(app.status, (L"Starting live processing to " + output_name +
        L". Other apps must select its paired virtual microphone endpoint.").c_str());
    update_controls(app);
    const HWND window = app.window;
    try {
        app.live_route_thread = std::thread([&app, window, input_id, output_id, settings] {
            auto* completion = new LiveRouteCompletion;
            try {
                run_live_processing(input_id, output_id, settings, app.stop_live_route, app.live_metrics, [window] {
                    PostMessageW(window, live_route_started_message, 0, 0);
                });
                completion->message = L"Live microphone processing stopped.";
            } catch (const std::exception& error) {
                completion->message = to_wide(error.what());
            }
            if (app.shutting_down.load(std::memory_order_relaxed) ||
                !PostMessageW(window, live_route_complete_message, 0, reinterpret_cast<LPARAM>(completion)))
                delete completion;
        });
    } catch (const std::exception& error) {
        app.live_routing = false;
        SetWindowTextW(app.status, to_wide(error.what()).c_str());
        update_controls(app);
    }
}

void set_profile_controls(Application& app, const int index) {
    const auto settings = audio::settings_for_preset(index == 1 ? audio::Preset::meeting
        : (index == 2 ? audio::Preset::strong_noise_reduction : audio::Preset::natural));
    SendMessageW(app.noise_suppression, BM_SETCHECK, settings.noise_suppression_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.noise_gate, BM_SETCHECK, settings.noise_gate_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.automatic_gain, BM_SETCHECK, settings.automatic_gain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.compressor, BM_SETCHECK, settings.compressor_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.enhancement, BM_SETCHECK, settings.enhancement_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto gain = static_cast<int>(settings.input_gain_db);
    SendMessageW(app.input_gain, TBM_SETPOS, TRUE, gain);
    SetWindowTextW(app.gain_value, (std::to_wstring(gain) + L" dB").c_str());
}

void load_settings(Application& app) {
    const auto preset = read_setting(app, L"processing", L"preset", L"natural");
    const int preset_index = preset == L"meeting" ? 1 : (preset == L"strong" ? 2 : 0);
    SendMessageW(app.preset, CB_SETCURSEL, static_cast<WPARAM>(preset_index), 0);
    set_profile_controls(app, preset_index);
    SendMessageW(app.noise_suppression, BM_SETCHECK,
        setting_enabled(app, L"noise-suppression", preset_index == 0 || preset_index == 1 || preset_index == 2) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.noise_gate, BM_SETCHECK,
        setting_enabled(app, L"noise-gate", preset_index != 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.automatic_gain, BM_SETCHECK,
        setting_enabled(app, L"automatic-gain", preset_index == 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.compressor, BM_SETCHECK,
        setting_enabled(app, L"compressor", preset_index == 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.enhancement, BM_SETCHECK,
        setting_enabled(app, L"enhancement-enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto gain_text = read_setting(app, L"processing", L"input-gain-db", L"0");
    const int gain = std::clamp(static_cast<int>(std::wcstol(gain_text.c_str(), nullptr, 10)), -12, 12);
    SendMessageW(app.input_gain, TBM_SETPOS, TRUE, gain);
    SetWindowTextW(app.gain_value, (std::to_wstring(gain) + L" dB").c_str());
    refresh_devices(app);
}

void initialize_controls(Application& app) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    add_label(app, L"ClearMic", 24, 18, 670, 34);
    add_label(app, L"Microphone", 24, 60, 140);
    add_control(app, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 82, 680, 240,
                microphone_combo, WS_EX_CLIENTEDGE);
    app.microphone = GetDlgItem(app.window, microphone_combo);
    add_label(app, L"Device status", 24, 126, 680, 24, device_status_label);
    app.device_status = GetDlgItem(app.window, device_status_label);
    add_label(app, L"Battery: Not available", 24, 151, 680, 24, battery_status_label);
    app.battery_status = GetDlgItem(app.window, battery_status_label);
    add_label(app, L"Diagnostics · WASAPI format and latency are available only when measured", 24, 174, 680, 34, device_id_label);
    app.device_id = GetDlgItem(app.window, device_id_label);
    add_label(app, L"Processed output device", 24, 210, 240);
    add_control(app, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 232, 500, 240,
                render_output_combo, WS_EX_CLIENTEDGE);
    app.render_output = GetDlgItem(app.window, render_output_combo);
    add_control(app, L"BUTTON", L"Download VB-CABLE", BS_PUSHBUTTON, 540, 232, 164, 30,
                virtual_cable_help_button);
    app.virtual_cable_help = GetDlgItem(app.window, virtual_cable_help_button);
    add_control(app, L"BUTTON", L"Start live routing", BS_PUSHBUTTON, 24, 270, 180, 34, live_route_button);
    app.live_route = GetDlgItem(app.window, live_route_button);
    add_label(app, L"Requires a virtual cable driver. Choose its playback endpoint; voice apps use its paired microphone.",
              220, 273, 484, 36);

    add_control(app, L"BUTTON", L"Apply enhancement to processed A/B sample", BS_AUTOCHECKBOX, 24, 312, 400, 26, enhancement_check);
    app.enhancement = GetDlgItem(app.window, enhancement_check);
    add_label(app, L"Processing profile", 24, 346, 180);
    add_control(app, L"COMBOBOX", L"", CBS_DROPDOWNLIST, 24, 368, 320, 120, preset_combo);
    app.preset = GetDlgItem(app.window, preset_combo);
    SendMessageW(app.preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Natural"));
    SendMessageW(app.preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Meeting"));
    SendMessageW(app.preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Strong Noise Reduction"));

    add_control(app, L"BUTTON", L"Noise suppression", BS_AUTOCHECKBOX, 24, 410, 190, 26, noise_suppression_check);
    add_control(app, L"BUTTON", L"Noise gate", BS_AUTOCHECKBOX, 230, 410, 170, 26, noise_gate_check);
    add_control(app, L"BUTTON", L"Automatic gain", BS_AUTOCHECKBOX, 420, 410, 170, 26, automatic_gain_check);
    add_control(app, L"BUTTON", L"Compressor", BS_AUTOCHECKBOX, 24, 440, 170, 26, compressor_check);
    app.noise_suppression = GetDlgItem(app.window, noise_suppression_check);
    app.noise_gate = GetDlgItem(app.window, noise_gate_check);
    app.automatic_gain = GetDlgItem(app.window, automatic_gain_check);
    app.compressor = GetDlgItem(app.window, compressor_check);

    add_label(app, L"Input gain", 24, 478, 100);
    add_label(app, L"0 dB", 110, 478, 45, 22, gain_value_label);
    app.gain_value = GetDlgItem(app.window, gain_value_label);
    add_control(app, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | TBS_HORZ, 160, 472, 360, 34, input_gain_slider);
    app.input_gain = GetDlgItem(app.window, input_gain_slider);
    SendMessageW(app.input_gain, TBM_SETRANGE, TRUE, MAKELONG(-12, 12));
    SendMessageW(app.input_gain, TBM_SETTICFREQ, 3, 0);
    SendMessageW(app.input_gain, TBM_SETPOS, TRUE, 0);

    add_label(app, L"Input level", 24, 520, 130, 22, input_meter_caption);
    add_control(app, PROGRESS_CLASSW, L"", PBS_SMOOTH, 160, 520, 544, 20, input_meter);
    app.input_level = GetDlgItem(app.window, input_meter);
    app.input_level_caption = GetDlgItem(app.window, input_meter_caption);
    add_label(app, L"Processed output", 24, 548, 130);
    add_control(app, PROGRESS_CLASSW, L"", PBS_SMOOTH, 160, 548, 544, 20, output_meter);
    app.output_level = GetDlgItem(app.window, output_meter);
    SendMessageW(app.input_level, PBM_SETRANGE32, 0, 100);
    SendMessageW(app.output_level, PBM_SETRANGE32, 0, 100);

    add_control(app, L"BUTTON", L"Record A/B test (5 seconds)", BS_PUSHBUTTON, 24, 588, 260, 34, record_button);
    add_control(app, L"BUTTON", L"Play original", BS_PUSHBUTTON, 300, 588, 120, 34, play_original_button);
    add_control(app, L"BUTTON", L"Play processed", BS_PUSHBUTTON, 430, 588, 140, 34, play_processed_button);
    add_control(app, L"BUTTON", L"Stop playback", BS_PUSHBUTTON, 580, 588, 124, 34, stop_playback_button);
    app.record = GetDlgItem(app.window, record_button);
    app.play_original = GetDlgItem(app.window, play_original_button);
    app.play_processed = GetDlgItem(app.window, play_processed_button);
    app.stop_playback = GetDlgItem(app.window, stop_playback_button);
    add_label(app, L"", 24, 632, 680, 38, status_label);
    app.status = GetDlgItem(app.window, status_label);
    add_label(app, L"Live processing is available through the selected playback endpoint; a compatible virtual cable driver is required.", 24, 678, 680, 28,
              enhancement_status_label);
    app.enhancement_status = GetDlgItem(app.window, enhancement_status_label);

    app.settings_file = configuration_file();
    load_settings(app);
    SetTimer(app.window, 1, 3000, nullptr);
    SetTimer(app.window, 2, 80, nullptr);
    SetTimer(app.window, input_meter_timer, 250, nullptr);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<Application*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    static const UINT taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == taskbar_created_message) {
        app->tray_icon_added = false;
        add_tray_icon(*app);
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        initialize_controls(*app);
        add_tray_icon(*app);
        return 0;
    case WM_TIMER:
        if (wparam == 1) refresh_devices(*app);
        else if (wparam == 2 && app->live_routing) {
            const auto input = app->live_metrics.input_rms.load(std::memory_order_relaxed);
            const auto output = app->live_metrics.output_rms.load(std::memory_order_relaxed);
            SendMessageW(app->input_level, PBM_SETPOS,
                         static_cast<WPARAM>(std::clamp(input * 100.0F, 0.0F, 100.0F)), 0);
            SendMessageW(app->output_level, PBM_SETPOS,
                         static_cast<WPARAM>(std::clamp(output * 100.0F, 0.0F, 100.0F)), 0);
            update_live_diagnostics(*app);
        } else if (wparam == input_meter_timer && !app->live_routing) {
            update_idle_input_meter(*app);
        } else if (wparam == device_refresh_timer) {
            KillTimer(window, device_refresh_timer);
            refresh_devices(*app);
        }
        return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED || wparam == DBT_DEVICEARRIVAL ||
            wparam == DBT_DEVICEREMOVECOMPLETE) {
            // Device and endpoint changes can arrive in bursts; refresh once after they settle.
            SetTimer(window, device_refresh_timer, 300, nullptr);
            return TRUE;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == app->input_gain) {
            SetWindowTextW(app->gain_value,
                (std::to_wstring(SendMessageW(app->input_gain, TBM_GETPOS, 0, 0)) + L" dB").c_str());
            if (LOWORD(wparam) == TB_ENDTRACK) save_processing_settings(*app);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case microphone_combo:
            if (HIWORD(wparam) == CBN_SELCHANGE && !app->refreshing_devices) {
                const int index = selected_device_index(*app);
                if (index >= 0) {
                    const auto id = to_wide(app->devices[static_cast<std::size_t>(index)].id);
                    write_setting(*app, L"audio", L"input-device", id.c_str());
                }
                update_device_status(*app);
                update_controls(*app);
                update_idle_input_meter(*app);
            }
            return 0;
        case render_output_combo:
            if (HIWORD(wparam) == CBN_SELCHANGE && !app->refreshing_devices) {
                const int index = selected_render_device_index(*app);
                if (index >= 0) {
                    const auto id = to_wide(app->render_devices[static_cast<std::size_t>(index)].id);
                    write_setting(*app, L"audio", L"render-device", id.c_str());
                } else write_setting(*app, L"audio", L"render-device", L"");
                update_controls(*app);
            }
            return 0;
        case virtual_cable_help_button:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", L"https://vb-audio.com/Cable/",
                                                        nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                SetWindowTextW(app->status, L"Open https://vb-audio.com/Cable/ to download the official VB-CABLE driver.");
            }
            return 0;
        case preset_combo:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                set_profile_controls(*app, static_cast<int>(SendMessageW(app->preset, CB_GETCURSEL, 0, 0)));
                save_processing_settings(*app);
            }
            return 0;
        case noise_suppression_check:
        case noise_gate_check:
        case automatic_gain_check:
        case compressor_check:
        case enhancement_check:
            if (HIWORD(wparam) == BN_CLICKED) save_processing_settings(*app);
            return 0;
        case record_button:
            begin_recording(*app);
            return 0;
        case live_route_button:
            toggle_live_route(*app);
            return 0;
        case play_original_button:
            if (PlaySoundW(app->original_file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                SetWindowTextW(app->status, L"Playing original recording.");
            else SetWindowTextW(app->status, L"Windows could not play the original recording.");
            return 0;
        case play_processed_button:
            if (PlaySoundW(app->processed_file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                SetWindowTextW(app->status, L"Playing processed recording.");
            else SetWindowTextW(app->status, L"Windows could not play the processed recording.");
            return 0;
        case stop_playback_button:
            PlaySoundW(nullptr, nullptr, 0);
            return 0;
        default:
            break;
        }
        break;
    case capture_complete_message: {
        std::unique_ptr<RecordCompletion> completion(reinterpret_cast<RecordCompletion*>(lparam));
        if (app->capture_thread.joinable()) app->capture_thread.join();
        app->recording = false;
        app->samples_ready = completion->success;
        SetWindowTextW(app->status, completion->message.c_str());
        if (completion->success) {
            SendMessageW(app->input_level, PBM_SETPOS,
                static_cast<WPARAM>(std::clamp(completion->input_rms * 100.0, 0.0, 100.0)), 0);
            SendMessageW(app->output_level, PBM_SETPOS,
                static_cast<WPARAM>(std::clamp(completion->output_rms * 100.0, 0.0, 100.0)), 0);
        }
        update_controls(*app);
        return 0;
    }
    case live_route_complete_message: {
        std::unique_ptr<LiveRouteCompletion> completion(reinterpret_cast<LiveRouteCompletion*>(lparam));
        if (app->live_route_thread.joinable()) app->live_route_thread.join();
        app->live_routing = false;
        app->stop_live_route.store(false, std::memory_order_relaxed);
        SetWindowTextW(app->status, completion->message.c_str());
        update_controls(*app);
        refresh_devices(*app);
        update_device_status(*app);
        return 0;
    }
    case live_route_started_message:
        if (app->live_routing && !app->stop_live_route.load(std::memory_order_relaxed))
            SetWindowTextW(app->status,
                L"Live microphone processing is active. Voice apps must select the paired virtual microphone endpoint.");
        return 0;
    case WM_CLOSE:
        if (app->live_routing && app->tray_icon_added && wparam == 0) {
            ShowWindow(window, SW_HIDE);
            SetWindowTextW(app->status, L"ClearMic is processing in the system tray.");
            return 0;
        }
        app->shutting_down.store(true, std::memory_order_relaxed);
        app->stop_live_route.store(true, std::memory_order_relaxed);
        if (app->live_route_thread.joinable()) app->live_route_thread.join();
        if (app->capture_thread.joinable()) app->capture_thread.join();
        PlaySoundW(nullptr, nullptr, 0);
        KillTimer(window, 1);
        KillTimer(window, 2);
        KillTimer(window, input_meter_timer);
        KillTimer(window, device_refresh_timer);
        remove_tray_icon(*app);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    if (message == tray_callback_message && app->tray_icon_added) {
        if (lparam == WM_LBUTTONUP) show_window(*app);
        else if (lparam == WM_RBUTTONUP) show_tray_menu(*app);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int run_desktop_application() {
    Application app;
    WNDCLASSEXW window_class_info{sizeof(window_class_info)};
    window_class_info.lpfnWndProc = window_procedure;
    window_class_info.hInstance = GetModuleHandleW(nullptr);
    window_class_info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class_info.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class_info.lpszClassName = window_class;
    if (!RegisterClassExW(&window_class_info) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
    RECT bounds{0, 0, 740, 722};
    AdjustWindowRect(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowExW(0, window_class, L"ClearMic", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), &app);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
}
