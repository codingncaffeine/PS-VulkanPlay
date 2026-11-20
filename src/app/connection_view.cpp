#include "app/connection_view.h"

#include <cmath>

namespace psvp::app {

bool connection_view_changed(const std::optional<proto::ConnectionQuality>& said, const proto::ConnectionQuality& now) noexcept {
    if (!said) return true;
    const double bitrate_before = said->target_bitrate.value_or(0), bitrate = now.target_bitrate.value_or(0);
    if (std::abs(bitrate - bitrate_before) > 0.2 * bitrate_before) return true;
    const double trip_before = said->rtt.value_or(0.0), trip = now.rtt.value_or(0.0);
    if (std::abs(trip - trip_before) > 0.5 * trip_before && std::abs(trip - trip_before) >= 20.0) return true;
    return said->loss.value_or(0) != now.loss.value_or(0);
}

}  // namespace psvp::app
