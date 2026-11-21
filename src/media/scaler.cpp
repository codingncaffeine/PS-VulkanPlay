#include "media/scaler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace psvp::media {

namespace {

double sinc(double x) {
    if (x == 0.0) return 1.0;
    const double a = std::numbers::pi * x;
    return std::sin(a) / a;
}

std::size_t table_index(Scaler scaler) {
    return static_cast<std::size_t>(std::ranges::find(kScalers, scaler) - kScalers.begin());
}

}  // namespace

std::string_view scaler_name(Scaler scaler) {
    switch (scaler) {
    case Scaler::bilinear: return "bilinear";
    case Scaler::catmull_rom: return "catmull-rom";
    case Scaler::lanczos: return "lanczos";
    case Scaler::ewa_lanczos: return "ewa-lanczos";
    case Scaler::ewa_lanczos_sharp: return "ewa-lanczos-sharp";
    }
    return "?";
}

std::optional<Scaler> scaler_from_name(std::string_view name) {
    for (const Scaler scaler : kScalers) {
        if (scaler_name(scaler) == name) return scaler;
    }
    return std::nullopt;
}

double bessel_j1(double x) {
    // The series sum over k of (-1)^k (x/2)^(2k+1) / (k! (k+1)!). Its largest term at x = 10.2 (the
    // furthest the kernels ask) is about 700: some 13 of double's 16 digits are left.
    const double half = x / 2.0, square = half * half;
    double term = half, sum = half;
    for (int k = 1; k < 60; ++k) {
        term *= -square / (static_cast<double>(k) * static_cast<double>(k + 1));
        sum += term;
        if (std::abs(term) < 1e-18 * std::max(1.0, std::abs(sum))) break;
    }
    return sum;
}

double jinc(double x) {
    if (x == 0.0) return 1.0;
    const double a = std::numbers::pi * x;
    return 2.0 * bessel_j1(a) / a;
}

double kernel_radius(Scaler scaler) {
    switch (scaler) {
    case Scaler::bilinear: return 1.0;
    case Scaler::catmull_rom: return 2.0;
    case Scaler::lanczos: return 3.0;
    case Scaler::ewa_lanczos: return kJincZero3;
    case Scaler::ewa_lanczos_sharp: return kJincZero3 * kEwaSharpBlur;
    }
    return 1.0;
}

bool kernel_polar(Scaler scaler) { return scaler == Scaler::ewa_lanczos || scaler == Scaler::ewa_lanczos_sharp; }

double kernel_weight(Scaler scaler, double distance) {
    const double t = std::abs(distance);
    if (t >= kernel_radius(scaler)) return 0.0;
    switch (scaler) {
    case Scaler::bilinear: return 1.0 - t;
    case Scaler::catmull_rom:
        return t < 1.0 ? (1.5 * t - 2.5) * t * t + 1.0 : ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
    case Scaler::lanczos: return sinc(t) * sinc(t / 3.0);
    case Scaler::ewa_lanczos: return jinc(t) * jinc(t * kJincZero1 / kJincZero3);
    case Scaler::ewa_lanczos_sharp: {
        const double u = t / kEwaSharpBlur;
        return jinc(u) * jinc(u * kJincZero1 / kJincZero3);
    }
    }
    return 0.0;
}

std::vector<float> weight_tables() {
    std::vector<float> tables;
    tables.reserve(kScalers.size() * kTableEntries);
    for (const Scaler scaler : kScalers) {
        const double radius = kernel_radius(scaler);
        for (std::uint32_t i = 0; i < kTableEntries; ++i) {
            // The last entry is the radius itself: weight 0.
            const double distance = radius * static_cast<double>(i) / static_cast<double>(kTableEntries - 1);
            tables.push_back(i + 1 == kTableEntries ? 0.0F : static_cast<float>(kernel_weight(scaler, distance)));
        }
    }
    return tables;
}

float table_weight(const std::vector<float>& tables, Scaler scaler, float distance) {
    // As the shader: the radius and entries per sample are floats it is given.
    const auto radius = static_cast<float>(kernel_radius(scaler));
    if (!(distance < radius)) return 0.0F;
    const float per_sample = static_cast<float>(kTableEntries - 1) / radius;
    const float at = distance * per_sample;
    const auto i = std::min(static_cast<std::size_t>(at), std::size_t{kTableEntries - 2});
    const float f = at - static_cast<float>(i);
    const std::size_t base = table_index(scaler) * kTableEntries;
    return tables[base + i] * (1.0F - f) + tables[base + i + 1] * f;
}

float stretch_for(std::uint32_t from, std::uint32_t to) {
    if (to == 0 || from <= to) return 1.0F;
    return std::min(static_cast<float>(from) / static_cast<float>(to), kMostStretch);
}

std::vector<float> scale_plane(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, std::uint32_t to_width, std::uint32_t to_height,
                               Scaler scaler, const std::vector<float>& tables) {
    if (to_width == width && to_height == height) return plane;
    std::vector<float> out(std::size_t{to_width} * to_height);
    const bool polar = kernel_polar(scaler);
    float stretch_x = stretch_for(width, to_width), stretch_y = stretch_for(height, to_height);
    if (polar) stretch_x = stretch_y = std::max(stretch_x, stretch_y);
    const auto radius = static_cast<float>(kernel_radius(scaler));
    const int last_x = static_cast<int>(width) - 1, last_y = static_cast<int>(height) - 1;
    for (std::uint32_t oy = 0; oy < to_height; ++oy) {
        for (std::uint32_t ox = 0; ox < to_width; ++ox) {
            // The place in the picture, its samples' middles at whole numbers.
            const float sx = (static_cast<float>(ox) + 0.5F) / static_cast<float>(to_width) * static_cast<float>(width) - 0.5F;
            const float sy = (static_cast<float>(oy) + 0.5F) / static_cast<float>(to_height) * static_cast<float>(height) - 0.5F;
            const int x0 = static_cast<int>(std::ceil(sx - radius * stretch_x)), x1 = static_cast<int>(std::floor(sx + radius * stretch_x));
            const int y0 = static_cast<int>(std::ceil(sy - radius * stretch_y)), y1 = static_cast<int>(std::floor(sy + radius * stretch_y));
            float sum = 0.0F, total = 0.0F;
            for (int y = y0; y <= y1; ++y) {
                const float dy = (static_cast<float>(y) - sy) / stretch_y;
                const float wy = polar ? 1.0F : table_weight(tables, scaler, std::abs(dy));
                if (wy == 0.0F) continue;
                const std::size_t row = static_cast<std::size_t>(std::clamp(y, 0, last_y)) * width;
                for (int x = x0; x <= x1; ++x) {
                    const float dx = (static_cast<float>(x) - sx) / stretch_x;
                    const float w = polar ? table_weight(tables, scaler, std::sqrt(dx * dx + dy * dy)) : wy * table_weight(tables, scaler, std::abs(dx));
                    sum += w * plane[row + static_cast<std::size_t>(std::clamp(x, 0, last_x))];
                    total += w;
                }
            }
            out[std::size_t{oy} * to_width + ox] = total != 0.0F ? sum / total : 0.0F;
        }
    }
    return out;
}

DebandOffset deband_offset(std::uint32_t place, int radius) {
    const auto span = static_cast<std::uint32_t>(2 * radius + 1);
    const std::uint32_t first = hash32(place), second = hash32(first);
    return {static_cast<int>(first % span) - radius, static_cast<int>(second % span) - radius};
}

float deband_grain(std::uint32_t place, float grain) {
    const std::uint32_t third = hash32(hash32(hash32(place)));
    return (static_cast<float>(third >> 8) * (1.0F / 16777216.0F) - 0.5F) * grain;
}

std::vector<float> deband_plane(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, const Deband& deband, std::uint32_t seed) {
    std::vector<float> out(plane.size());
    const int last_x = static_cast<int>(width) - 1, last_y = static_cast<int>(height) - 1;
    const auto at = [&](int x, int y) { return plane[static_cast<std::size_t>(std::clamp(y, 0, last_y)) * width + static_cast<std::size_t>(std::clamp(x, 0, last_x))]; };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const DebandOffset o = deband_offset(place_hash(x, y, seed), deband.radius);
            const int px = static_cast<int>(x), py = static_cast<int>(y);
            const float mean = (at(px + o.x, py + o.y) + at(px - o.x, py - o.y) + at(px - o.y, py + o.x) + at(px + o.y, py - o.x)) * 0.25F;
            const float here = at(px, py);
            out[std::size_t{y} * width + x] = std::abs(here - mean) >= deband.threshold ? here : mean;
        }
    }
    return out;
}

}  // namespace psvp::media
