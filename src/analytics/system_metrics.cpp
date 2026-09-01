#include "trading_engine/analytics/system_metrics.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::analytics {

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
        case Stage::Ingest:      return "ingest";
        case Stage::Strategy:    return "strategy";
        case Stage::Risk:        return "risk";
        case Stage::Execution:   return "execution";
        case Stage::Portfolio:   return "portfolio";
        case Stage::Persistence: return "persistence";
    }
    return "invalid";
}

SystemMetrics::SystemMetrics() = default;
SystemMetrics::~SystemMetrics() = default;

void SystemMetrics::record_latency(Stage /*stage*/, common::Duration /*elapsed*/) {
    throw common::NotImplemented("SystemMetrics::record_latency");
}

void SystemMetrics::set_queue_depth(std::string_view /*queue_name*/,
                                    std::size_t /*depth*/) {
    throw common::NotImplemented("SystemMetrics::set_queue_depth");
}

void SystemMetrics::increment_events(Stage /*stage*/, std::uint64_t /*n*/) {
    throw common::NotImplemented("SystemMetrics::increment_events");
}

void SystemMetrics::increment_errors(Stage /*stage*/, std::uint64_t /*n*/) {
    throw common::NotImplemented("SystemMetrics::increment_errors");
}

SystemMetrics::Snapshot SystemMetrics::snapshot() const {
    throw common::NotImplemented("SystemMetrics::snapshot");
}

}  // namespace trading_engine::analytics
