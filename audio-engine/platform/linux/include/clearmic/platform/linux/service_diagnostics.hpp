#pragma once

#include <cstdio>

namespace clearmic::platform::pipewire {

struct ServiceDiagnostics {
    float max_dsp_ms{};
    float max_dsp_budget_percent{};
    float max_dsp_thread_cpu_ms{};
    unsigned long long capture_overruns{};
    unsigned long long source_underruns{};
    unsigned long long processed_seconds{};
    float capture_graph_ms{};
    float capture_queue_ms{};
    float capture_buffered_ms{};
    float source_graph_ms{};
    float source_queue_ms{};
    float source_buffered_ms{};
};

[[nodiscard]] inline bool parse_service_diagnostics(const char* line, ServiceDiagnostics& diagnostics) noexcept {
    return line && std::sscanf(line, "DIAG %f %f %f %llu %llu %llu %f %f %f %f %f %f",
        &diagnostics.max_dsp_ms, &diagnostics.max_dsp_budget_percent, &diagnostics.max_dsp_thread_cpu_ms,
        &diagnostics.capture_overruns, &diagnostics.source_underruns, &diagnostics.processed_seconds,
        &diagnostics.capture_graph_ms, &diagnostics.capture_queue_ms, &diagnostics.capture_buffered_ms,
        &diagnostics.source_graph_ms, &diagnostics.source_queue_ms, &diagnostics.source_buffered_ms) == 12;
}

} // namespace clearmic::platform::pipewire
