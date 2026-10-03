#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace clearmic::platform::windows {

inline std::wstring normalize_endpoint_name(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return name;
}

inline std::wstring_view paired_capture_name(std::wstring_view output_name) {
    const auto name = normalize_endpoint_name(std::wstring(output_name));
    if (name.find(L"cable input") != std::wstring::npos) return L"cable output";
    if (name.find(L"voicemeeter input") != std::wstring::npos) return L"voicemeeter output";
    if (name.find(L"virtual cable input") != std::wstring::npos) return L"virtual cable output";
    if (name.find(L"virtual audio input") != std::wstring::npos) return L"virtual audio output";
    if (name.find(L"virtual audio driver") != std::wstring::npos) return L"virtual mic driver";
    return {};
}

inline bool is_virtual_cable_output_name(std::wstring_view raw_name) {
    const auto name = normalize_endpoint_name(std::wstring(raw_name));
    return name.find(L"cable input") != std::wstring::npos ||
           name.find(L"voicemeeter input") != std::wstring::npos ||
           name.find(L"virtual cable input") != std::wstring::npos ||
           name.find(L"virtual audio input") != std::wstring::npos ||
           name.find(L"virtual audio driver") != std::wstring::npos;
}

inline bool is_virtual_cable_capture_name(std::wstring_view raw_name) {
    const auto name = normalize_endpoint_name(std::wstring(raw_name));
    return name.find(L"cable output") != std::wstring::npos ||
           name.find(L"voicemeeter output") != std::wstring::npos ||
           name.find(L"virtual cable output") != std::wstring::npos ||
           name.find(L"virtual audio output") != std::wstring::npos ||
           name.find(L"virtual mic driver") != std::wstring::npos;
}

} // namespace clearmic::platform::windows
