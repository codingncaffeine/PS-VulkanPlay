#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "check.h"
#include "media/scaler.h"

using namespace psvp::media;

namespace {
bool close(double got, double want, double within) { return std::abs(got - want) <= within; }
}  // namespace

// Bessel's J1 against published values (Abramowitz and Stegun, Table 9.1), and jinc's zeros where
// the polar kernels and their window end.
PSVP_TEST(scaler_bessel_j1_and_jinc_zeros) {
    CHECK(close(bessel_j1(0.0), 0.0, 1e-15));
    CHECK(close(bessel_j1(1.0), 0.4400505857449335, 1e-13));
    CHECK(close(bessel_j1(5.0), -0.3275791375914652, 1e-12));
    CHECK(close(bessel_j1(10.0), 0.0434727461688614, 1e-11));
    CHECK(close(jinc(0.0), 1.0, 0.0));
    CHECK(close(jinc(kJincZero1), 0.0, 1e-11));
    CHECK(close(jinc(kJincZero3), 0.0, 1e-11));
    CHECK(jinc(2.0) < 0.0);  // between the first and second zeros: the first negative lobe
}

// The separable kernels pass through every sample (weight 1 at 0, 0 at the other whole numbers),
// and the tent and Catmull-Rom give weights that add up to 1 wherever a place falls.
PSVP_TEST(scaler_separable_kernels_interpolate) {
    for (const Scaler scaler : {Scaler::bilinear, Scaler::catmull_rom, Scaler::lanczos}) {
        CHECK(!kernel_polar(scaler));
        CHECK(close(kernel_weight(scaler, 0.0), 1.0, 1e-15));
        for (const double whole : {1.0, 2.0, 3.0}) CHECK(close(kernel_weight(scaler, whole), 0.0, 1e-15));
        CHECK(close(kernel_weight(scaler, kernel_radius(scaler)), 0.0, 0.0));
        for (const double f : {0.0, 0.1, 0.25, 0.5, 0.77}) {
            double sum = 0.0;
            for (int k = -3; k <= 3; ++k) sum += kernel_weight(scaler, f + k);
            CHECK(close(sum, 1.0, scaler == Scaler::lanczos ? 0.02 : 1e-12));
        }
    }
    // Catmull-Rom's two pieces meet at 1, with the same slope (-0.5 both).
    CHECK(close(kernel_weight(Scaler::catmull_rom, 1.0 - 1e-9), kernel_weight(Scaler::catmull_rom, 1.0 + 1e-9), 1e-8));
    CHECK(close(kernel_weight(Scaler::catmull_rom, 1.5), -0.0625, 1e-15));
    CHECK(kernel_polar(Scaler::ewa_lanczos) && kernel_polar(Scaler::ewa_lanczos_sharp));
    CHECK(kernel_radius(Scaler::ewa_lanczos_sharp) < kernel_radius(Scaler::ewa_lanczos));
}

// The tables read with linear steps are the kernels within the error of the steps.
PSVP_TEST(scaler_tables_follow_the_kernels) {
    const std::vector<float> tables = weight_tables();
    CHECK(tables.size() == kScalers.size() * kTableEntries);
    for (const Scaler scaler : kScalers) {
        double worst = 0.0;
        const double radius = kernel_radius(scaler);
        for (int i = 0; i <= 5000; ++i) {
            const double distance = radius * 1.05 * i / 5000.0;
            worst = std::max(worst, std::abs(static_cast<double>(table_weight(tables, scaler, static_cast<float>(distance))) - kernel_weight(scaler, distance)));
        }
        CHECK(worst < 3e-5);
        CHECK(table_weight(tables, scaler, static_cast<float>(radius) + 0.01F) == 0.0F);
    }
}

PSVP_TEST(scaler_names_go_both_ways) {
    for (const Scaler scaler : kScalers) CHECK(scaler_from_name(scaler_name(scaler)) == scaler);
    CHECK(!scaler_from_name("nearest"));
    CHECK(scaler_name(kDefaultScaler) == "ewa-lanczos");
}

// A picture drawn by each scaler: an even area stays even, larger and smaller; as large, it is the
// picture itself; a ramp stays the ramp through the tent and Catmull-Rom (both reproduce straight lines).
PSVP_TEST(scaler_scale_plane_keeps_even_areas_and_ramps) {
    const std::vector<float> tables = weight_tables();
    const std::vector<float> even(40 * 30, 0.37F);
    std::vector<float> ramp(40 * 30);
    for (std::uint32_t y = 0; y < 30; ++y) {
        for (std::uint32_t x = 0; x < 40; ++x) ramp[y * 40 + x] = 0.1F + 0.02F * static_cast<float>(x);
    }
    for (const Scaler scaler : kScalers) {
        for (const auto& [w, h] : {std::pair{71U, 53U}, std::pair{40U, 30U}, std::pair{17U, 13U}}) {
            const std::vector<float> out = scale_plane(even, 40, 30, w, h, scaler, tables);
            CHECK(out.size() == std::size_t{w} * h);
            CHECK(std::ranges::all_of(out, [](float v) { return close(static_cast<double>(v), 0.37, 1e-5); }));
        }
        CHECK(scale_plane(ramp, 40, 30, 40, 30, scaler, tables) == ramp);
    }
    for (const Scaler scaler : {Scaler::bilinear, Scaler::catmull_rom}) {
        const std::uint32_t w = 100, h = 30;
        const std::vector<float> out = scale_plane(ramp, 40, 30, w, h, scaler, tables);
        // Away from the edges (where they are repeated): the ramp at the place each pixel falls on.
        for (std::uint32_t x = 10; x < 90; ++x) {
            const float place = (static_cast<float>(x) + 0.5F) * 0.4F - 0.5F;
            CHECK(close(static_cast<double>(out[15 * w + x]), 0.1 + 0.02 * static_cast<double>(place), 1e-4));
        }
    }
}

// Drawn at half the size, the finest detail (a checkerboard of single samples) averages out to gray
// because the kernel is stretched; unstretched it would come out as stripes of black and white.
PSVP_TEST(scaler_stretches_the_kernel_when_drawn_smaller) {
    CHECK(stretch_for(1920, 3440) == 1.0F);
    CHECK(stretch_for(1920, 960) == 2.0F);
    CHECK(stretch_for(4000, 10) == kMostStretch);
    const std::vector<float> tables = weight_tables();
    std::vector<float> board(64 * 64);
    for (std::uint32_t y = 0; y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) board[y * 64 + x] = ((x + y) % 2 == 0) ? 1.0F : 0.0F;
    }
    for (const Scaler scaler : kScalers) {
        const std::vector<float> out = scale_plane(board, 64, 64, 27, 27, scaler, tables);
        float worst = 0.0F;
        for (std::uint32_t y = 4; y < 23; ++y) {
            for (std::uint32_t x = 4; x < 23; ++x) worst = std::max(worst, std::abs(out[y * 27 + x] - 0.5F));
        }
        CHECK(worst < 0.08F);
    }
}

// Debanding: the random numbers stay inside their bounds and are the same each time; an even area
// is left alone; a ramp cut into bands gets values between the bands; a hard edge is kept.
PSVP_TEST(scaler_deband_smooths_bands_and_keeps_edges) {
    const Deband deband;
    for (std::uint32_t i = 0; i < 2000; ++i) {
        const DebandOffset o = deband_offset(place_hash(i % 50, i / 50, 7), deband.radius);
        CHECK(std::abs(o.x) <= deband.radius && std::abs(o.y) <= deband.radius);
        const float g = deband_grain(place_hash(i, 3, 9), deband.grain);
        CHECK(g >= -deband.grain / 2 && g <= deband.grain / 2);
    }
    CHECK(place_hash(3, 4, 5) == place_hash(3, 4, 5) && place_hash(3, 4, 5) != place_hash(4, 3, 5) && place_hash(3, 4, 5) != place_hash(3, 4, 6));

    const std::uint32_t w = 128, h = 32;
    const std::vector<float> even(w * h, 0.5F);
    CHECK(deband_plane(even, w, h, deband, 1) == even);
    std::vector<float> bands(w * h), edge(w * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            bands[y * w + x] = (100.0F + static_cast<float>(x >> 4U)) / 255.0F;
            edge[y * w + x] = x < w / 2 ? 0.2F : 0.8F;
        }
    }
    const std::vector<float> smoothed = deband_plane(bands, w, h, deband, 1);
    const std::set<float> before(bands.begin(), bands.end()), after(smoothed.begin(), smoothed.end());
    CHECK(before.size() == 8);
    CHECK(after.size() > 20);
    CHECK(deband_plane(edge, w, h, deband, 1) == edge);
}

// Sharpening: nothing moves where it is even; an edge gets steeper, without going past black or
// white; more strength, more; the shader's arithmetic is this one (render-check compares them).
PSVP_TEST(sharpen_is_contrast_adaptive) {
    using psvp::media::sharpen_sample;
    CHECK(std::abs(sharpen_sample(0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 1.0F) - 0.5F) < 1e-6F);
    CHECK(sharpen_sample(0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F) == 0.0F);
    // A sample brighter than its neighbors gets brighter, more so at full strength.
    const float soft = sharpen_sample(0.6F, 0.5F, 0.5F, 0.5F, 0.5F, 0.0F), hard = sharpen_sample(0.6F, 0.5F, 0.5F, 0.5F, 0.5F, 1.0F);
    CHECK(soft > 0.6F && hard > soft && hard < 1.0F);
    // Next to black or white there is no room: it is left as it is.
    CHECK(sharpen_sample(0.6F, 0.0F, 0.5F, 0.5F, 0.5F, 1.0F) == 0.6F);
    CHECK(sharpen_sample(0.6F, 1.0F, 0.5F, 0.5F, 0.5F, 1.0F) == 0.6F);
    // A plane: the edges repeated, an even plane unchanged.
    const std::vector<float> even(12, 0.4F);
    const std::vector<float> still = psvp::media::sharpen_plane(even, 4, 3, 0.5F);
    CHECK(std::ranges::all_of(still, [](float v) { return std::abs(v - 0.4F) < 1e-6F; }));
    CHECK(psvp::media::sharpen_strength(1) == 0.0F && psvp::media::sharpen_strength(100) == 1.0F);
}
