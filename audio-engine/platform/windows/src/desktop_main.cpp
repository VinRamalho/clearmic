#include "clearmic/platform/windows/device_manager.hpp"

#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return clearmic::platform::windows::run_desktop_application();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return clearmic::platform::windows::run_desktop_application();
}
