#include "clearmic/platform/linux/device_manager.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;

void handle_signal(int) { stopping = 1; }
}

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::atomic<unsigned int> change_count{};
    clearmic::platform::pipewire::DeviceMonitor monitor([&] {
        const auto sequence = change_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::cout << "CHANGE " << sequence << '\n' << std::flush;
    });
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}
