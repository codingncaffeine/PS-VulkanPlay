// Fuzz target for the reader of an H.264 stream's frames (media/h264_refs.h): any
// bytes as a stream's parameter sets, as a frame under a layout taken from the
// first three bytes, and as a run of frames for the tracker. A frame that is read
// uses frames within its frame numbers, each once, and an intra frame none; the
// tracker never hands out bytes to decode in place of a frame.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "media/h264_refs.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::media;
    const std::span<const std::uint8_t> all(data, size);
    (void)read_h264_layout(all);
    if (size < 3) return 0;

    H264Layout layout;
    layout.frame_number_bits = 4U + (data[0] & 0x0fU) % 13U;
    layout.order_type = ((data[0] >> 4) & 0x03U) % 3U;
    layout.order_deltas_zero = (data[0] & 0x40U) != 0;
    layout.colour_planes_apart = (data[0] & 0x80U) != 0;
    layout.order_bits = 4U + (data[1] & 0x0fU) % 13U;
    layout.bottom_order_present = (data[1] & 0x10U) != 0;
    layout.redundant_counts = (data[1] & 0x20U) != 0;
    layout.default_used = 1U + (data[2] & 0x1fU);
    layout.frames_kept = (data[2] >> 5) * 2U + 1U;
    const std::span<const std::uint8_t> frame = all.subspan(3);

    if (const auto read = read_h264_frame(layout, frame)) {
        const unsigned max_number = 1U << layout.frame_number_bits;
        if (read->number >= max_number || (read->intra && !read->used.empty())) std::abort();
        for (std::size_t i = 0; i < read->used.size(); ++i) {
            if (read->used[i] == 0 || read->used[i] >= max_number) std::abort();
            if (std::count(read->used.begin(), read->used.end(), read->used[i]) != 1) std::abort();
        }
    }

    // The same bytes as a run of frames, cut where the third byte says, behind any header.
    H264Tracker tracker(all);
    std::uint16_t index = 1;
    const std::size_t piece = 8U + data[2];
    for (std::size_t at = 0; at < frame.size(); at += piece) {
        const auto part = frame.subspan(at, std::min(piece, frame.size() - at));
        const auto verdict = tracker.look(index, part);
        if (!verdict.changed.empty()) std::abort();
        tracker.decoded(index, (data[0] & 1U) == 0 || index % 3 != 0);
        index = static_cast<std::uint16_t>(index + 1 + (part.empty() ? 0 : part[0] & 3));
    }
    return 0;
}
