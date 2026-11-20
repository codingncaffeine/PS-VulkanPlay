// The console says about every second how it sees the connection: the bitrate it aims the
// picture at, a round-trip value, the loss it counts. This decides when that is worth a line in
// the log: when it has changed enough to explain what a person saw.
#pragma once

#include <optional>

#include "proto/stream_message.h"

namespace psvp::app {

// True for the first view, and then when the target bitrate has moved by a fifth, the round-trip
// value by half (and by 20 at least), or the loss count at all, against the view said last.
[[nodiscard]] bool connection_view_changed(const std::optional<proto::ConnectionQuality>& said, const proto::ConnectionQuality& now) noexcept;

}  // namespace psvp::app
