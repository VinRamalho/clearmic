#include "clearmic/platform/windows/device_manager.hpp"

#include <windows.h>
#include <audioclient.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace clearmic::platform::windows {
namespace {
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
}
