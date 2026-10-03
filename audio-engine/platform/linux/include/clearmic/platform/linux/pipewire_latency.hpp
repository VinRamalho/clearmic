#pragma once

#include <cstdint>
#include <optional>

namespace clearmic::platform::pipewire {

struct StreamLatency {
    float graph_ms{};
    float queued_ms{};
    float buffered_ms{};
};

[[nodiscard]] inline std::optional<StreamLatency> stream_latency_ms(const std::int64_t delay_ticks,
    const std::uint64_t queued_samples, const std::uint64_t buffered_samples,
    const std::uint32_t rate_num, const std::uint32_t rate_denom,
    const std::uint32_t sample_rate) noexcept {
    if (rate_num == 0 || rate_denom == 0 || sample_rate == 0) return std::nullopt;
    const auto tick_ms = 1000.0 * static_cast<double>(rate_num) / rate_denom;
    const auto sample_ms = 1000.0 / sample_rate;
    return StreamLatency{
        static_cast<float>(static_cast<double>(delay_ticks) * tick_ms),
        static_cast<float>(static_cast<double>(queued_samples) * sample_ms),
        static_cast<float>(static_cast<double>(buffered_samples) * sample_ms),
    };
}

} // namespace clearmic::platform::pipewire
