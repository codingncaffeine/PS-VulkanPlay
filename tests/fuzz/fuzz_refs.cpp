// Fuzz target for the reader of an HEVC stream's frames (media/frame_refs.h):
// any bytes as a stream's parameter sets, as a frame under any layout, and as a
// run of frames for the tracker. A frame that is read and written again with
// the frames it uses comes back byte for byte; written with other ones, it
// reads as using those.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "media/frame_refs.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::media;
    const std::span<const std::uint8_t> all(data, size);
    (void)read_hevc_layout(all);
    (void)carries_hevc_parameter_sets(all);
    if (size < 3) return 0;

    HevcLayout layout;
    layout.order_bits = 4U + (data[0] & 0x0fU) % 13U;
    layout.dependent_slices = (data[0] & 0x10U) != 0;
    layout.output_flag = (data[0] & 0x20U) != 0;
    layout.color_planes_apart = (data[0] & 0x40U) != 0;
    layout.lists_changeable = (data[0] & 0x80U) != 0;
    layout.address_bits = data[1] & 0x0fU;
    layout.extra_bits = (data[1] >> 4) & 0x07U;
    const std::uint8_t toggles = data[2];
    const std::span<const std::uint8_t> frame = all.subspan(3);

    if (const auto read = read_hevc_frame(layout, frame)) {
        std::vector<bool> used, other;
        for (std::size_t i = 0; i < read->named.size(); ++i) {
            used.push_back(read->named[i].used);
            other.push_back(read->named[i].used != (((toggles >> (i % 8)) & 1U) != 0));
        }
        if (const auto same = with_used_frames(layout, frame, used)) {
            if (same->size() != frame.size() || !std::equal(same->begin(), same->end(), frame.begin())) std::abort();
            const auto changed = with_used_frames(layout, frame, other);
            if (!changed) std::abort();
            const auto again = read_hevc_frame(layout, *changed);
            if (!again || again->order != read->order || again->named.size() != other.size() || again->restart != read->restart) std::abort();
            for (std::size_t i = 0; i < other.size(); ++i) {
                if (again->named[i].used != other[i] || again->named[i].back != read->named[i].back) std::abort();
            }
        }
    }

    // The same bytes as a run of frames, cut where the toggles say, behind any header.
    ReferenceTracker tracker(all);
    std::uint16_t index = 1;
    const std::size_t piece = 8U + toggles;
    for (std::size_t at = 0; at < frame.size(); at += piece) {
        const auto part = frame.subspan(at, std::min(piece, frame.size() - at));
        const auto verdict = tracker.look(index, part);
        if (verdict.way == ReferenceTracker::Way::changed && verdict.changed.empty()) std::abort();
        tracker.decoded(index, (toggles & 1U) == 0 || index % 3 != 0);
        index = static_cast<std::uint16_t>(index + 1 + (part.empty() ? 0 : part[0] & 3));
    }
    return 0;
}
