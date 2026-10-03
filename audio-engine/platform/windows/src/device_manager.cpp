#include "clearmic/platform/windows/device_manager.hpp"

#include <windows.h>
#include <audioclient.h>
#include <cfgmgr32.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winioctl.h>
#include <poclass.h>
#include <propvarutil.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef __IAudioMeterInformation_INTERFACE_DEFINED__
#define __IAudioMeterInformation_INTERFACE_DEFINED__
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* channels) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT channels, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* support) = 0;
};
#endif

namespace clearmic::platform::windows {
namespace {
constexpr IID audio_meter_information_iid{
    0xc02216f6, 0x8c67, 0x4b5b, {0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64}};
constexpr GUID battery_device_interface_guid{
    0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};
constexpr DEVPROPKEY device_container_id_key{
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
constexpr GUID no_container_id{
    0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
constexpr PROPERTYKEY device_friendly_name_key{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
template <typename T> struct ComRelease { void operator()(T* value) const noexcept { if (value) value->Release(); } };
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease<T>>;

void check_hresult(const HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}

std::string to_utf8(const wchar_t* value) {
    if (!value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), size, nullptr, nullptr);
    output.pop_back();
    return output;
}

std::wstring to_wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), size) != size) return {};
    return output;
}

std::string audio_function_kind(const std::wstring& instance_id, const std::string& name) {
    const auto upper_name = [&] {
        std::string value = name;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }();
    if (upper_name.find("HEADSET") != std::string::npos || upper_name.find("HANDS-FREE") != std::string::npos)
        return "bluetooth-headset-function";
    if (instance_id.rfind(L"BTHENUM\\", 0) == 0 && upper_name.find("HEADPHONE") == std::string::npos)
        return "bluetooth-audio-function";
    return "audio-function";
}

std::vector<audio::AudioDevice> bluetooth_audio_functions() {
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return {};
    struct DeviceSetGuard { HDEVINFO value; ~DeviceSetGuard() { SetupDiDestroyDeviceInfoList(value); } } guard{devices};
    std::vector<audio::AudioDevice> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(devices, index, &data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        std::array<wchar_t, 512> id{};
        if (CM_Get_Device_IDW(data.DevInst, id.data(), static_cast<ULONG>(id.size()), 0) != CR_SUCCESS) continue;
        std::array<wchar_t, 256> friendly{};
        DWORD property_type{};
        const auto bytes = static_cast<DWORD>(friendly.size() * sizeof(wchar_t));
        auto name = SetupDiGetDeviceRegistryPropertyW(devices, &data, SPDRP_FRIENDLYNAME, &property_type,
                                                       reinterpret_cast<PBYTE>(friendly.data()), bytes, nullptr)
            ? to_utf8(friendly.data()) : std::string{};
        if (name.empty()) {
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &data, SPDRP_DEVICEDESC, &property_type,
                                                   reinterpret_cast<PBYTE>(friendly.data()), bytes, nullptr)) continue;
            name = to_utf8(friendly.data());
        }
        if (name.empty() || name.find("Audio") == std::string::npos && name.find("Hands-Free") == std::string::npos &&
            name.find("Headset") == std::string::npos && name.find("Headphone") == std::string::npos) continue;
        ULONG status = 0;
        ULONG problem = 0;
        audio::AudioDevice info;
        info.id = to_utf8(id.data());
        info.name = std::move(name);
        info.device_kind = audio_function_kind(id.data(), info.name);
        info.selectable = false;
        if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS)
            info.connection = problem == 0 ? audio::ConnectionState::connected : audio::ConnectionState::disconnected;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<GUID> endpoint_container_ids(const std::wstring& endpoint_id) {
    if (endpoint_id.empty()) return {};
    std::wstring instance_id = L"SWD\\MMDEVAPI\\";
    instance_id += endpoint_id;
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, instance_id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return {};

    // MMDevice endpoints and their physical devices may have different
    // ContainerIds. Keep every real ID in the ancestry for strict matching.
    std::vector<GUID> containers;
    for (unsigned int depth = 0; depth < 16; ++depth) {
        GUID container{};
        DEVPROPTYPE property_type{};
        ULONG size = sizeof(container);
        if (CM_Get_DevNode_PropertyW(node, &device_container_id_key, &property_type,
                                     reinterpret_cast<PBYTE>(&container), &size, 0) == CR_SUCCESS &&
            property_type == DEVPROP_TYPE_GUID && !IsEqualGUID(container, no_container_id) &&
            std::none_of(containers.begin(), containers.end(), [&](const GUID& existing) {
                return IsEqualGUID(existing, container) != FALSE;
            })) containers.push_back(container);
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return containers;
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> endpoint_usb_ids(const std::wstring& endpoint_id) {
    if (endpoint_id.empty()) return std::nullopt;
    std::wstring endpoint_instance = L"SWD\\MMDEVAPI\\";
    endpoint_instance += endpoint_id;
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, endpoint_instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return std::nullopt;

    for (unsigned int depth = 0; depth < 16; ++depth) {
        std::array<wchar_t, 512> instance_id{};
        if (CM_Get_Device_IDW(node, instance_id.data(), static_cast<ULONG>(instance_id.size()), 0) == CR_SUCCESS) {
            const std::wstring_view value(instance_id.data());
            if (value.starts_with(L"USB\\")) {
                const auto vendor_position = value.find(L"VID_");
                const auto product_position = value.find(L"&PID_");
                if (vendor_position != std::wstring_view::npos && product_position != std::wstring_view::npos &&
                    value.size() >= vendor_position + 8 && value.size() >= product_position + 9) {
                    auto parse_component = [&](const std::size_t position) -> std::optional<std::uint16_t> {
                        std::string hexadecimal;
                        hexadecimal.reserve(4);
                        for (std::size_t index = position; index < position + 4; ++index) {
                            if (value[index] > 0x7f) return std::nullopt;
                            hexadecimal.push_back(static_cast<char>(value[index]));
                        }
                        return audio::parse_device_identifier("0x" + hexadecimal);
                    };
                    auto vendor = parse_component(vendor_position + 4);
                    auto product = parse_component(product_position + 5);
                    if (vendor && product) return std::pair{*vendor, *product};
                }
            }
        }
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return std::nullopt;
}

std::optional<audio::BatteryInfo> query_battery_interface(const wchar_t* path) {
    HANDLE battery = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (battery == INVALID_HANDLE_VALUE) return std::nullopt;
    struct HandleGuard { HANDLE value; ~HandleGuard() { CloseHandle(value); } } guard{battery};

    ULONG timeout_ms = 0;
    ULONG tag = 0;
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_TAG, &timeout_ms, sizeof(timeout_ms),
                         &tag, sizeof(tag), &bytes_returned, nullptr) || tag == 0)
        return std::nullopt;

    BATTERY_QUERY_INFORMATION information_query{tag, BatteryInformation, 0};
    BATTERY_INFORMATION information{};
    if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &information_query,
                         sizeof(information_query), &information, sizeof(information),
                         &bytes_returned, nullptr)) return std::nullopt;
    if ((information.Capabilities & BATTERY_SYSTEM_BATTERY) != 0) return std::nullopt;

    BATTERY_WAIT_STATUS status_query{tag, 0, 0, 0, 0};
    BATTERY_STATUS status{};
    if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_STATUS, &status_query, sizeof(status_query),
                         &status, sizeof(status), &bytes_returned, nullptr)) return std::nullopt;
    const auto percentage = audio::battery_percentage_from_capacity(status.Capacity,
                                                                      information.FullChargedCapacity);
    if (!percentage) return std::nullopt;

    auto charging = audio::ChargingState::unknown;
    if ((status.PowerState & BATTERY_DISCHARGING) != 0)
        charging = audio::ChargingState::not_charging;
    else if (*percentage == 100)
        charging = audio::ChargingState::full;
    else if ((status.PowerState & BATTERY_CHARGING) != 0)
        charging = audio::ChargingState::charging;
    return audio::BatteryInfo{*percentage, charging};
}

std::optional<audio::BatteryInfo> endpoint_battery(const std::wstring& endpoint_id) {
    const auto containers = endpoint_container_ids(endpoint_id);
    if (containers.empty()) return std::nullopt;

    HDEVINFO batteries = SetupDiGetClassDevsW(&battery_device_interface_guid, nullptr, nullptr,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (batteries == INVALID_HANDLE_VALUE) return std::nullopt;
    struct DeviceSetGuard { HDEVINFO value; ~DeviceSetGuard() { SetupDiDestroyDeviceInfoList(value); } } guard{batteries};

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(batteries, nullptr, &battery_device_interface_guid,
                                         index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD required_size = 0;
        SetupDiGetDeviceInterfaceDetailW(batteries, &interface_data, nullptr, 0, &required_size, nullptr);
        if (required_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> detail_storage(required_size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA device_data{};
        device_data.cbSize = sizeof(device_data);
        if (!SetupDiGetDeviceInterfaceDetailW(batteries, &interface_data, detail, required_size,
                                              nullptr, &device_data)) continue;

        GUID battery_container{};
        DEVPROPTYPE property_type{};
        ULONG property_size = sizeof(battery_container);
        if (CM_Get_DevNode_PropertyW(device_data.DevInst, &device_container_id_key, &property_type,
                                     reinterpret_cast<PBYTE>(&battery_container), &property_size, 0) != CR_SUCCESS ||
            property_type != DEVPROP_TYPE_GUID ||
            std::none_of(containers.begin(), containers.end(), [&](const GUID& container) {
                return IsEqualGUID(battery_container, container) != FALSE;
            })) continue;
        if (auto reading = query_battery_interface(detail->DevicePath)) return reading;
    }
    return std::nullopt;
}
}

std::vector<audio::AudioDevice> DeviceManager::input_devices() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) check_hresult(initialized, "CoInitializeEx");
    struct ApartmentGuard { bool active; ~ApartmentGuard() { if (active) ::CoUninitialize(); } } apartment{SUCCEEDED(initialized)};

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator)),
                  "Create audio device enumerator");
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);

    IMMDevice* raw_default = nullptr;
    const HRESULT default_hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &raw_default);
    ComPtr<IMMDevice> default_device(SUCCEEDED(default_hr) ? raw_default : nullptr);
    LPWSTR default_id = nullptr;
    if (default_device) default_device->GetId(&default_id);
    const std::wstring default_id_value = default_id ? default_id : L"";
    CoTaskMemFree(default_id);

    IMMDeviceCollection* raw_collection = nullptr;
    check_hresult(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &raw_collection), "Enumerate capture devices");
    ComPtr<IMMDeviceCollection> collection(raw_collection);
    UINT count = 0;
    check_hresult(collection->GetCount(&count), "Read capture device count");

    std::vector<audio::AudioDevice> result;
    result.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* raw_device = nullptr;
        if (FAILED(collection->Item(index, &raw_device))) continue;
        ComPtr<IMMDevice> device(raw_device);
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        const std::wstring id_value = id ? id : L"";
        audio::AudioDevice info;
        info.id = to_utf8(id);
        info.is_default = id_value == default_id_value;
        try { info.capabilities.battery = endpoint_battery(id_value); } catch (...) {}
        if (const auto usb_ids = endpoint_usb_ids(id_value)) {
            info.usb_vendor_id = usb_ids->first;
            info.usb_product_id = usb_ids->second;
        }
        CoTaskMemFree(id);

        DWORD state = 0;
        if (SUCCEEDED(device->GetState(&state)))
            info.connection = (state & DEVICE_STATE_ACTIVE) ? audio::ConnectionState::connected : audio::ConnectionState::disconnected;

        IPropertyStore* raw_properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &raw_properties))) {
            ComPtr<IPropertyStore> properties(raw_properties);
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(device_friendly_name_key, &name)) && name.vt == VT_LPWSTR)
                info.name = to_utf8(name.pwszVal);
            PropVariantClear(&name);
        }

        IAudioClient* raw_client = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&raw_client)))) {
            ComPtr<IAudioClient> client(raw_client);
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&format)) && format) {
                info.sample_rate_hz = format->nSamplesPerSec;
                info.channels = format->nChannels;
                CoTaskMemFree(format);
            }
        }
        result.push_back(std::move(info));
    }
    auto bluetooth = bluetooth_audio_functions();
    result.insert(result.end(), std::make_move_iterator(bluetooth.begin()), std::make_move_iterator(bluetooth.end()));
    return result;
}

std::vector<audio::AudioDevice> DeviceManager::output_devices() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) check_hresult(initialized, "CoInitializeEx");
    struct ApartmentGuard { bool active; ~ApartmentGuard() { if (active) ::CoUninitialize(); } } apartment{SUCCEEDED(initialized)};

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator)),
                  "Create audio device enumerator");
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);

    IMMDevice* raw_default = nullptr;
    const HRESULT default_hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &raw_default);
    ComPtr<IMMDevice> default_device(SUCCEEDED(default_hr) ? raw_default : nullptr);
    LPWSTR default_id = nullptr;
    if (default_device) default_device->GetId(&default_id);
    const std::wstring default_id_value = default_id ? default_id : L"";
    CoTaskMemFree(default_id);

    IMMDeviceCollection* raw_collection = nullptr;
    check_hresult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_collection), "Enumerate playback devices");
    ComPtr<IMMDeviceCollection> collection(raw_collection);
    UINT count = 0;
    check_hresult(collection->GetCount(&count), "Read playback device count");

    std::vector<audio::AudioDevice> result;
    result.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* raw_device = nullptr;
        if (FAILED(collection->Item(index, &raw_device))) continue;
        ComPtr<IMMDevice> device(raw_device);
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        const std::wstring id_value = id ? id : L"";
        audio::AudioDevice info;
        info.id = to_utf8(id);
        info.is_default = id_value == default_id_value;
        info.device_kind = "playback-endpoint";
        CoTaskMemFree(id);

        DWORD state = 0;
        if (SUCCEEDED(device->GetState(&state)))
            info.connection = (state & DEVICE_STATE_ACTIVE) ? audio::ConnectionState::connected : audio::ConnectionState::disconnected;

        IPropertyStore* raw_properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &raw_properties))) {
            ComPtr<IPropertyStore> properties(raw_properties);
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(device_friendly_name_key, &name)) && name.vt == VT_LPWSTR)
                info.name = to_utf8(name.pwszVal);
            PropVariantClear(&name);
        }

        IAudioClient* raw_client = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&raw_client)))) {
            ComPtr<IAudioClient> client(raw_client);
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&format)) && format) {
                info.sample_rate_hz = format->nSamplesPerSec;
                info.channels = format->nChannels;
                CoTaskMemFree(format);
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::optional<float> DeviceManager::input_peak_level(const std::string_view device_id) {
    if (device_id.empty()) return std::nullopt;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return std::nullopt;
    struct ApartmentGuard { bool active; ~ApartmentGuard() { if (active) ::CoUninitialize(); } } apartment{SUCCEEDED(initialized)};

    IMMDeviceEnumerator* raw_enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enumerator))))
        return std::nullopt;
    ComPtr<IMMDeviceEnumerator> enumerator(raw_enumerator);
    const auto wide_id = to_wide(device_id);
    if (wide_id.empty()) return std::nullopt;
    IMMDevice* raw_device = nullptr;
    if (FAILED(enumerator->GetDevice(wide_id.c_str(), &raw_device))) return std::nullopt;
    ComPtr<IMMDevice> device(raw_device);
    IAudioMeterInformation* raw_meter = nullptr;
    if (FAILED(device->Activate(audio_meter_information_iid, CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&raw_meter)))) return std::nullopt;
    ComPtr<IAudioMeterInformation> meter(raw_meter);
    float peak = 0.0F;
    if (FAILED(meter->GetPeakValue(&peak)) || !std::isfinite(peak)) return std::nullopt;
    return std::clamp(peak, 0.0F, 1.0F);
}
}
