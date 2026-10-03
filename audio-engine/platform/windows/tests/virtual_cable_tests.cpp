#include "clearmic/platform/windows/virtual_cable.hpp"

#include <string_view>

int main() {
    using clearmic::platform::windows::is_virtual_cable_capture_name;
    using clearmic::platform::windows::is_virtual_cable_output_name;
    using clearmic::platform::windows::paired_capture_name;
    if (!is_virtual_cable_output_name(L"Virtual Audio Driver") ||
        !is_virtual_cable_capture_name(L"Virtual Mic Driver") ||
        paired_capture_name(L"Virtual Audio Driver") != L"virtual mic driver" ||
        !paired_capture_name(L"Realtek Speakers").empty() ||
        !is_virtual_cable_output_name(L"CABLE Input (VB-Audio Virtual Cable)") ||
        paired_capture_name(L"CABLE INPUT").find(L"cable output") == std::wstring_view::npos)
        return 1;
    return 0;
}
