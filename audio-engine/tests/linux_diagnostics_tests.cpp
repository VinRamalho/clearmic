#include "clearmic/platform/linux/service_diagnostics.hpp"

#include <stdexcept>

void run_linux_diagnostics_tests() {
    using clearmic::platform::pipewire::ServiceDiagnostics;
    using clearmic::platform::pipewire::parse_service_diagnostics;

    ServiceDiagnostics diagnostics;
    if (!parse_service_diagnostics("DIAG 1.25 5.9 0.75 2 3 42 1.0 2.0 3.0 4.0 5.0 6.0", diagnostics))
        throw std::runtime_error("Complete PipeWire service diagnostics should parse");
    if (diagnostics.max_dsp_ms != 1.25F || diagnostics.max_dsp_budget_percent != 5.9F ||
        diagnostics.max_dsp_thread_cpu_ms != 0.75F || diagnostics.capture_overruns != 2 ||
        diagnostics.source_underruns != 3 || diagnostics.processed_seconds != 42 ||
        diagnostics.capture_graph_ms != 1.0F || diagnostics.capture_queue_ms != 2.0F ||
        diagnostics.capture_buffered_ms != 3.0F || diagnostics.source_graph_ms != 4.0F ||
        diagnostics.source_queue_ms != 5.0F || diagnostics.source_buffered_ms != 6.0F)
        throw std::runtime_error("PipeWire diagnostics fields should retain their emitted values");
    if (!parse_service_diagnostics("DIAG 1 2 3 0 0 1 -1 -1 -1 -1 -1 -1", diagnostics))
        throw std::runtime_error("Unavailable PipeWire latency values should remain valid diagnostics");
    if (parse_service_diagnostics("DIAG 1 2 3 0 0 1 0 0 0 0 0", diagnostics) ||
        parse_service_diagnostics("METER 0.5 0.4", diagnostics) || parse_service_diagnostics(nullptr, diagnostics))
        throw std::runtime_error("Truncated, unrelated, and null service output must not parse as diagnostics");
}
