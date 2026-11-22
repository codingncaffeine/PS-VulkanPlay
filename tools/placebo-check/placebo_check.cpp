// I5, the picture reference (PLAN §7): the test picture (media/test_chart.h) drawn by libplacebo, the
// renderer the reference client is built on, and compared pixel by pixel with our own drawing of it
// (psvp_render_check --chart-out). A test tool: libplacebo is never part of the program.
//
//   psvp_placebo_check OURS.f32 [--hdr] [--peak NITS] [--plant]
//
// Both draw the picture as large as it is, color samples between brightness samples by a straight
// line, nothing else (no debanding, dithering, scaling, peak measuring, color adjustment).
//   SDR (BT.709, limited range): libplacebo keeps the picture's own transfer (BT.1886 in and out), as we
//     do: every channel within 1/255 in 99.9% of the pixels.
//   HDR (BT.2020, PQ) tone mapped to SDR: BT.2390 (its knee as the report has it) from NITS to SDR white
//     (203 cd/m²), gamut by clipping,
//     sRGB out (D35). The grays (the gradient and the gray patches) are judged by CIEDE2000 <= 1; the
//     colors are said, not judged: libplacebo maps them in another space than ours (D35 says why ours).
// --plant draws libplacebo's picture with BT.601's matrix instead (the control: the SDR check must fail).
// Exit 0 when within, 1 when not, 2 for a wrong use, 3 when nothing could be drawn.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>

#include "media/delta_e.h"
#include "media/test_chart.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: psvp_placebo_check OURS.f32 [--hdr] [--peak NITS] [--plant]\n");
    return 2;
}

std::uint8_t byte_of(float share) { return static_cast<std::uint8_t>(std::lround(std::clamp(share, 0.0F, 1.0F) * 255.0F)); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string ours_file = argv[1];
    bool hdr = false, plant = false;
    float peak = 1000.0F;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--hdr") hdr = true;
        else if (arg == "--plant") plant = true;
        else if (arg == "--peak" && i + 1 < argc) peak = static_cast<float>(std::atof(argv[++i]));
        else return usage();
    }
    const psvp::media::TestChart chart = psvp::media::make_test_chart(hdr);
    const int w = static_cast<int>(chart.width), h = static_cast<int>(chart.height);
    std::vector<float> ours(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    {
        std::ifstream in(ours_file, std::ios::binary);
        in.read(reinterpret_cast<char*>(ours.data()), static_cast<std::streamsize>(ours.size() * sizeof(float)));
        if (!in) {
            std::fprintf(stderr, "placebo-check: %s is not a drawing of the test picture (psvp_render_check --chart-out)\n", ours_file.c_str());
            return 2;
        }
    }

    // (libplacebo's parameter macros are C's compound literals: named structs here.)
    struct pl_log_params log_params = pl_log_default_params;
    log_params.log_cb = pl_log_simple;
    log_params.log_level = PL_LOG_WARN;
    pl_log log = pl_log_create(PL_API_VER, &log_params);
    struct pl_vulkan_params vulkan_params = pl_vulkan_default_params;
    vulkan_params.allow_software = true;
    pl_vulkan vk = pl_vulkan_create(log, &vulkan_params);
    if (vk == nullptr) {
        std::fprintf(stderr, "placebo-check: libplacebo found no Vulkan device\n");
        pl_log_destroy(&log);
        return 3;
    }
    pl_gpu gpu = vk->gpu;
    int code = 0;
    std::vector<std::uint16_t> theirs(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    {
        pl_tex textures[3] = {};
        struct pl_frame image = {};
        image.num_planes = 3;
        const std::vector<std::uint16_t>* data[3] = {&chart.y, &chart.cb, &chart.cr};
        bool uploaded = true;
        for (int i = 0; i < 3 && uploaded; ++i) {
            const int pw = i == 0 ? w : w / 2, ph = i == 0 ? h : h / 2;
            struct pl_plane_data plane = {};
            plane.type = PL_FMT_UNORM;
            plane.width = pw;
            plane.height = ph;
            plane.component_size[0] = 16;
            plane.component_map[0] = i;
            plane.pixel_stride = 2;
            plane.row_stride = static_cast<std::size_t>(pw) * 2;
            plane.pixels = data[i]->data();
            uploaded = pl_upload_plane(gpu, &image.planes[i], &textures[i], &plane);
        }
        image.repr.sys = plant ? PL_COLOR_SYSTEM_BT_601 : hdr ? PL_COLOR_SYSTEM_BT_2020_NC : PL_COLOR_SYSTEM_BT_709;
        image.repr.levels = PL_COLOR_LEVELS_LIMITED;
        image.repr.bits.sample_depth = 16;
        image.repr.bits.color_depth = 10;
        image.repr.bits.bit_shift = 0;
        image.color.primaries = hdr ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709;
        image.color.transfer = hdr ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_BT_1886;
        if (hdr) {
            image.color.hdr.max_luma = peak;
            image.color.hdr.min_luma = PL_COLOR_HDR_BLACK;
        }
        pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);

        struct pl_tex_params tex_params = {};
        tex_params.w = w;
        tex_params.h = h;
        tex_params.format = pl_find_named_fmt(gpu, "rgba16");
        tex_params.renderable = true;
        tex_params.host_readable = true;
        pl_tex target_tex = tex_params.format != nullptr ? pl_tex_create(gpu, &tex_params) : nullptr;
        struct pl_frame target = {};
        target.num_planes = 1;
        target.planes[0].texture = target_tex;
        target.planes[0].components = 4;
        for (int c = 0; c < 4; ++c) target.planes[0].component_mapping[c] = c;
        target.repr = pl_color_repr_rgb;
        target.color.primaries = PL_COLOR_PRIM_BT_709;
        // SDR: the picture's own transfer out (as ours passes it on); HDR tone mapped: sRGB (as ours encodes it).
        target.color.transfer = hdr ? PL_COLOR_TRC_SRGB : PL_COLOR_TRC_BT_1886;
        target.color.hdr.max_luma = PL_COLOR_SDR_WHITE;
        target.color.hdr.min_luma = PL_COLOR_HDR_BLACK;

        struct pl_color_map_params color_map = pl_color_map_default_params;
        color_map.tone_mapping_function = &pl_tone_map_bt2390;
        // The knee where BT.2390 puts it (0.5: libplacebo's own default, 1.0, is not the report's).
        color_map.tone_constants.knee_offset = 0.5F;
        color_map.gamut_mapping = &pl_gamut_map_clip;
        struct pl_render_params params = pl_render_default_params;
        params.upscaler = &pl_filter_bilinear;
        params.downscaler = &pl_filter_bilinear;
        params.plane_upscaler = &pl_filter_bilinear;
        params.plane_downscaler = &pl_filter_bilinear;
        params.antiringing_strength = 0.0F;
        params.deband_params = nullptr;
        params.sigmoid_params = nullptr;
        params.color_adjustment = nullptr;
        params.peak_detect_params = nullptr;
        params.dither_params = nullptr;
        params.cone_params = nullptr;
        params.color_map_params = &color_map;

        pl_renderer renderer = pl_renderer_create(log, gpu);
        struct pl_tex_transfer_params download = {};
        download.tex = target_tex;
        download.ptr = theirs.data();
        if (!uploaded || target_tex == nullptr || renderer == nullptr || !pl_render_image(renderer, &image, &target, &params) || !pl_tex_download(gpu, &download)) {
            std::fprintf(stderr, "placebo-check: libplacebo could not draw the test picture\n");
            code = 3;
        }
        pl_renderer_destroy(&renderer);
        pl_tex_destroy(gpu, &target_tex);
        for (pl_tex& texture : textures) pl_tex_destroy(gpu, &texture);
    }
    pl_vulkan_destroy(&vk);
    pl_log_destroy(&log);
    if (code != 0) return code;

    const auto theirs_at = [&](int x, int y, int c) { return static_cast<float>(theirs[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c)]) / 65535.0F; };
    const auto ours_at = [&](int x, int y, int c) { return ours[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]; };
    if (!hdr) {
        std::size_t off = 0;
        double worst = 0.0;
        int worst_x = 0, worst_y = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double most = 0.0;
                for (int c = 0; c < 3; ++c) most = std::max(most, std::abs(static_cast<double>(ours_at(x, y, c) - theirs_at(x, y, c))) * 255.0);
                off += most > 1.0 ? 1U : 0U;
                if (most > worst) worst = most, worst_x = x, worst_y = y;
            }
        }
        const double share = static_cast<double>(off) / static_cast<double>(w * h);
        std::printf("placebo-check: SDR: %.3f%% of the pixels more than 1/255 from libplacebo in a channel; the largest %.2f/255 at %d,%d\n", 100.0 * share, worst, worst_x, worst_y);
        if (share > 0.001) code = 1;
    } else {
        // The grays: the gradient's rows and the gray patches' middles (8 pixels in from every edge).
        double worst_gray = 0.0, worst_color = 0.0, sum_gray = 0.0;
        std::size_t grays = 0;
        const auto delta = [&](int x, int y) {
            const auto mine = psvp::media::lab_of_srgb(byte_of(ours_at(x, y, 0)), byte_of(ours_at(x, y, 1)), byte_of(ours_at(x, y, 2)));
            const auto other = psvp::media::lab_of_srgb(byte_of(theirs_at(x, y, 0)), byte_of(theirs_at(x, y, 1)), byte_of(theirs_at(x, y, 2)));
            return psvp::media::delta_e_2000(mine, other);
        };
        for (int y = 8; y < static_cast<int>(psvp::media::kChartGradientRows) - 8; y += 4) {
            for (int x = 0; x < w; x += 2) {
                const double d = delta(x, y);
                worst_gray = std::max(worst_gray, d);
                sum_gray += d;
                ++grays;
            }
        }
        constexpr int kGrays = static_cast<int>(std::size(psvp::media::kChartHdrGrays));
        const int each = w / kGrays / 2 * 2;
        for (int i = 0; i < kGrays; ++i) {
            const double d = delta(i * each + each / 2, static_cast<int>(psvp::media::kChartGraysAt + 90));
            std::printf("placebo-check: HDR gray of %g cd/m²: CIEDE2000 %.2f\n", psvp::media::kChartHdrGrays[i], d);
            worst_gray = std::max(worst_gray, d);
        }
        for (int i = 0; i < 7; ++i) {
            const int x = i * (w / 7 / 2 * 2) + w / 14, y = static_cast<int>(psvp::media::kChartBarsAt + 120);
            const double d = delta(x, y);
            worst_color = std::max(worst_color, d);
            std::printf("placebo-check: HDR BT.709 color %d at SDR white: ours %u,%u,%u, libplacebo's %u,%u,%u: CIEDE2000 %.2f (said, not judged)\n", i,
                        byte_of(ours_at(x, y, 0)), byte_of(ours_at(x, y, 1)), byte_of(ours_at(x, y, 2)), byte_of(theirs_at(x, y, 0)), byte_of(theirs_at(x, y, 1)),
                        byte_of(theirs_at(x, y, 2)), d);
        }
        std::printf("placebo-check: HDR tone mapped from %.0f cd/m²: grays CIEDE2000 %.2f on average, %.2f at most; colors %.2f at most\n", static_cast<double>(peak),
                    sum_gray / static_cast<double>(std::max<std::size_t>(grays, 1)), worst_gray, worst_color);
        if (worst_gray > 1.0) code = 1;
    }
    std::printf(code == 0 ? "placebo-check: within\n" : "placebo-check: NOT within\n");
    return code;
}
