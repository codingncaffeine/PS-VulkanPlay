#include <optional>

#include "app/connection_view.h"
#include "check.h"

using namespace psvp::app;
using psvp::proto::ConnectionQuality;

namespace {

ConnectionQuality view(std::uint32_t bitrate, double trip, std::uint64_t loss) {
    ConnectionQuality quality;
    quality.target_bitrate = bitrate;
    quality.rtt = trip;
    quality.loss = loss;
    return quality;
}

}  // namespace

PSVP_TEST(the_consoles_first_view_of_the_connection_is_said) {
    CHECK(connection_view_changed(std::nullopt, view(10'000'000, 80.0, 0)));
    CHECK(connection_view_changed(std::nullopt, ConnectionQuality{}));
}

PSVP_TEST(a_view_that_stays_about_the_same_is_not_said_again) {
    const std::optional said = view(10'000'000, 80.0, 0);
    CHECK(!connection_view_changed(said, view(10'000'000, 80.0, 0)));
    // The small movements of every second.
    CHECK(!connection_view_changed(said, view(11'900'000, 80.0, 0)));
    CHECK(!connection_view_changed(said, view(8'100'000, 80.0, 0)));
    CHECK(!connection_view_changed(said, view(10'000'000, 115.0, 0)));
    CHECK(!connection_view_changed(said, view(10'000'000, 45.0, 0)));
    // A small round-trip value that doubles has still moved by very little.
    CHECK(!connection_view_changed(std::optional(view(10'000'000, 10.0, 0)), view(10'000'000, 25.0, 0)));
}

PSVP_TEST(a_view_that_moved_is_said) {
    const std::optional said = view(10'000'000, 80.0, 0);
    CHECK(connection_view_changed(said, view(12'100'000, 80.0, 0)));
    CHECK(connection_view_changed(said, view(7'900'000, 80.0, 0)));
    CHECK(connection_view_changed(said, view(10'000'000, 121.0, 0)));
    CHECK(connection_view_changed(said, view(10'000'000, 39.0, 0)));
    CHECK(connection_view_changed(said, view(10'000'000, 80.0, 1)));
    // From nothing to something.
    CHECK(connection_view_changed(std::optional(view(0, 0.0, 0)), view(5'000'000, 0.0, 0)));
    CHECK(connection_view_changed(std::optional(view(0, 0.0, 0)), view(0, 30.0, 0)));
}
