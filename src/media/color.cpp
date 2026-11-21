#include "media/color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace psvp::media {

ColorTransform color_transform(ColorMatrix matrix, bool full_range, unsigned bits) {
    // How much of the brightness is red and how much is blue; the rest is green.
    double red = 0.2126, blue = 0.0722;
    if (matrix == ColorMatrix::bt601) {
        red = 0.299;
        blue = 0.114;
    } else if (matrix == ColorMatrix::bt2020) {
        red = 0.2627;
        blue = 0.0593;
    }
    const double green = 1.0 - red - blue;

    const double largest = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    const double step = std::ldexp(1.0, static_cast<int>(bits) - 8);  // one 8-bit step in this depth
    const double black = full_range ? 0.0 : 16.0 * step / largest;
    const double brightness_span = full_range ? 1.0 : 219.0 * step / largest;
    const double middle = 128.0 * step / largest;
    const double color_span = full_range ? 1.0 : 224.0 * step / largest;

    // y' = (y - black) / span, c' = (c - middle) / span, from -0.5 to 0.5; then
    // red = y' + 2(1 - Kr) cr', blue = y' + 2(1 - Kb) cb', and green from what is left.
    const double y = 1.0 / brightness_span;
    const double cr_red = 2.0 * (1.0 - red) / color_span;
    const double cb_blue = 2.0 * (1.0 - blue) / color_span;
    const double cb_green = -2.0 * blue * (1.0 - blue) / green / color_span;
    const double cr_green = -2.0 * red * (1.0 - red) / green / color_span;
    const auto f = [](double value) { return static_cast<float>(value); };
    return ColorTransform{
        f(y), f(y), f(y), 0.0F,                    // times y
        0.0F, f(cb_green), f(cb_blue), 0.0F,       // times cb
        f(cr_red), f(cr_green), 0.0F, 0.0F,        // times cr
        f(-black * y - middle * cr_red), f(-black * y - middle * (cb_green + cr_green)), f(-black * y - middle * cb_blue), 1.0F,
    };
}

std::array<int, 3> screen_color(const ColorTransform& transform, unsigned bits, unsigned y, unsigned cb, unsigned cr, const ToneMap& tone) {
    const double largest = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    const double in[4] = {y / largest, cb / largest, cr / largest, 1.0};
    std::array<double, 3> rgb{};
    for (std::size_t row = 0; row < 3; ++row) {
        double value = 0.0;
        for (std::size_t column = 0; column < 4; ++column) value += static_cast<double>(transform[column * 4 + row]) * in[column];
        rgb[row] = std::clamp(value, 0.0, 1.0);
    }
    if (tone.transfer != ColorTransfer::sdr) {
        rgb = hdr_to_sdr_linear(rgb, tone);
        for (double& value : rgb) value = srgb_encode(value);
    }
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(std::lround(rgb[i] * 255.0));
    return out;
}

Placement place_picture(unsigned picture_width, unsigned picture_height, unsigned target_width, unsigned target_height) {
    if (picture_width == 0 || picture_height == 0 || target_width == 0 || target_height == 0) return {};
    Placement placement;
    // The side that fills the target is the one the picture is relatively longer on.
    if (static_cast<std::uint64_t>(picture_width) * target_height >= static_cast<std::uint64_t>(picture_height) * target_width) {
        placement.width = target_width;
        placement.height = std::max(1U, static_cast<unsigned>((static_cast<std::uint64_t>(picture_height) * target_width + picture_width / 2) / picture_width));
    } else {
        placement.height = target_height;
        placement.width = std::max(1U, static_cast<unsigned>((static_cast<std::uint64_t>(picture_width) * target_height + picture_height / 2) / picture_height));
    }
    placement.x = static_cast<int>((target_width - placement.width) / 2);
    placement.y = static_cast<int>((target_height - placement.height) / 2);
    return placement;
}

}  // namespace psvp::media
