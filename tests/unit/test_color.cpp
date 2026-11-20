#include <array>
#include <cstdlib>

#include "check.h"
#include "media/color.h"

using namespace psvp::media;

namespace {
bool near(const std::array<int, 3>& got, int red, int green, int blue) {
    return std::abs(got[0] - red) <= 1 && std::abs(got[1] - green) <= 1 && std::abs(got[2] - blue) <= 1;
}
}  // namespace

// BT.709 in the limited range, the way a console's stream is: black at 16, white at 235,
// and color bars of three quarters strength come out as 191 (0.75 * 255) in their channels.
PSVP_TEST(color_bt709_limited_range) {
    const ColorTransform t = color_transform(ColorMatrix::bt709, false, 8);
    CHECK(near(screen_color(t, 8, 16, 128, 128), 0, 0, 0));
    CHECK(near(screen_color(t, 8, 235, 128, 128), 255, 255, 255));
    CHECK(near(screen_color(t, 8, 126, 128, 128), 128, 128, 128));  // (126 - 16) / 219 = 0.502
    CHECK(near(screen_color(t, 8, 51, 109, 212), 191, 0, 0));       // red
    CHECK(near(screen_color(t, 8, 133, 63, 52), 0, 191, 0));        // green
    CHECK(near(screen_color(t, 8, 28, 212, 120), 0, 0, 191));       // blue
    CHECK(near(screen_color(t, 8, 168, 44, 136), 191, 191, 0));     // yellow
    // Below black and above white there is nothing more.
    CHECK(near(screen_color(t, 8, 0, 128, 128), 0, 0, 0));
    CHECK(near(screen_color(t, 8, 255, 128, 128), 255, 255, 255));
}

PSVP_TEST(color_other_ranges_and_matrices) {
    // The full range uses every value.
    const ColorTransform full = color_transform(ColorMatrix::bt709, true, 8);
    CHECK(near(screen_color(full, 8, 0, 128, 128), 0, 0, 0));
    CHECK(near(screen_color(full, 8, 255, 128, 128), 255, 255, 255));
    CHECK(near(screen_color(full, 8, 128, 128, 128), 128, 128, 128));
    // Red is brightness 0.2126 and, of the red difference, (1 - 0.2126) / 1.5748 = 0.5 above the middle.
    CHECK(near(screen_color(full, 8, 54, 99, 255), 254, 0, 0));
    // Ten bits, limited: black at 64, white at 940, the middle of the differences at 512.
    const ColorTransform ten = color_transform(ColorMatrix::bt2020, false, 10);
    CHECK(near(screen_color(ten, 10, 64, 512, 512), 0, 0, 0));
    CHECK(near(screen_color(ten, 10, 940, 512, 512), 255, 255, 255));
    // The same samples mean another color under another standard: BT.709's red read as BT.601.
    const ColorTransform old = color_transform(ColorMatrix::bt601, false, 8);
    CHECK(near(screen_color(old, 8, 235, 128, 128), 255, 255, 255));
    CHECK(!near(screen_color(old, 8, 51, 109, 212), 191, 0, 0));
    // Red 40.8 + 1.596 * 84 = 174.8; green 40.8 + 0.392 * 19 - 0.813 * 84 = -20.1, so none; blue 40.8 - 2.017 * 19 = 2.4.
    CHECK(near(screen_color(old, 8, 51, 109, 212), 175, 0, 2));
    // BT.601's own red: brightness 0.299 * 0.75.
    CHECK(near(screen_color(old, 8, 65, 100, 212), 191, 0, 0));
}

PSVP_TEST(picture_is_placed_as_large_as_it_fits) {
    CHECK(place_picture(1920, 1080, 1280, 720) == (Placement{0, 0, 1280, 720}));
    CHECK(place_picture(416, 240, 1280, 720) == (Placement{16, 0, 1248, 720}));    // a little less wide: bars at the sides
    CHECK(place_picture(1920, 1080, 1280, 1024) == (Placement{0, 152, 1280, 720})); // bars above and below
    CHECK(place_picture(640, 480, 1920, 1080) == (Placement{240, 0, 1440, 1080}));
    CHECK(place_picture(1920, 1080, 1, 1) == (Placement{0, 0, 1, 1}));
    CHECK(place_picture(0, 1080, 1280, 720) == Placement{});
    CHECK(place_picture(1920, 1080, 0, 720) == Placement{});
}
