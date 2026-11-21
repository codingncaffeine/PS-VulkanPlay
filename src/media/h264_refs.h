// Which earlier frames a frame of an H.264 stream is predicted from, read from
// the frame itself (ITU-T H.264, 7.3.2.1 to 7.3.3.1 and 8.2.4), and a tracker
// that says for each frame whether its picture will be the console's. Unlike an
// HEVC frame, an H.264 frame is not changed: a decoder fills the place of a
// frame it never got with the frame it got last (FFmpeg does: "Frame num gap"),
// which is what this client does by hand for HEVC. What the tracker adds is
// knowing which pictures are built on such a filled-in frame, so that they are
// counted, and when the console's repair makes the picture exact again.
// No decoder and no I/O in here.
#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "media/frame_refs.h"

namespace psvp::media {

// What a stream's parameter sets say that the first bits of a slice depend on.
struct H264Layout {
    unsigned sequence_set = 0;
    unsigned picture_set = 0;
    unsigned frame_number_bits = 0;
    unsigned order_type = 0;
    unsigned order_bits = 0;              // order_type 0
    bool order_deltas_zero = false;       // order_type 1
    bool frames_only = true;              // no fields
    bool colour_planes_apart = false;
    bool bottom_order_present = false;
    bool redundant_counts = false;
    unsigned default_used = 1;            // num_ref_idx_l0_default_active_minus1 + 1
    unsigned frames_kept = 0;             // max_num_ref_frames
    friend bool operator==(const H264Layout&, const H264Layout&) = default;
};

// From bytes that hold a stream's sequence and picture parameter sets. Streams of a shape this
// reader does not follow are refused with the reason (fields, slice groups, more than one
// parameter set of a kind).
[[nodiscard]] std::expected<H264Layout, std::string> read_h264_layout(std::span<const std::uint8_t> bytes);

struct H264Frame {
    bool restart = false;          // an IDR picture: nothing before it is needed any more
    bool intra = false;            // every slice predicted from no other frame
    bool reference = false;        // kept for later frames to be predicted from
    unsigned number = 0;           // frame_num
    // The frames it is predicted from, as how many frame numbers back each is; nearest list place first.
    std::vector<unsigned> used;
};

// One frame as a console sends it: the NAL units of one picture, each behind a start code.
[[nodiscard]] std::expected<H264Frame, std::string> read_h264_frame(const H264Layout& layout, std::span<const std::uint8_t> frame);

// Follows which frames a decoder holds and says for each frame that arrives whether its
// picture will be the console's. The verdict is the HEVC tracker's (frame_refs.h), with
// `changed` meaning that the decoder itself takes the frame it got last in place of a frame
// this one uses and never got; the verdict's bytes stay empty: the frame is decoded as sent.
class H264Tracker {
public:
    using Verdict = ReferenceTracker::Verdict;
    using Way = ReferenceTracker::Way;

    // `header` is what the console sent before the first frame.
    explicit H264Tracker(std::span<const std::uint8_t> header);
    // To be called for every frame, in the stream's order, with its number in the stream.
    [[nodiscard]] Verdict look(std::uint16_t index, std::span<const std::uint8_t> frame);
    // After look(): whether the decoder gave a picture for that frame.
    void decoded(std::uint16_t index, bool gave_picture);

    [[nodiscard]] const std::string& unread_reason() const noexcept { return off_; }

private:
    struct Held {
        unsigned number = 0;
        bool exact = true;
        bool filled = false;  // a lost frame's place, filled by the decoder with the frame it got last
    };
    struct Pending {
        std::uint16_t index = 0;
        unsigned number = 0;
        bool exact = true;
        bool restart = false;
        bool reference = false;
    };
    Verdict unread(std::string why);

    std::optional<H264Layout> layout_;
    std::string off_;            // not empty: nothing is read until an IDR picture
    std::deque<Held> held_;      // the reference frames the decoder holds as sent, newest last
    std::optional<Pending> pending_;
};

}  // namespace psvp::media
