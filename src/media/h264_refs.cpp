#include "media/h264_refs.h"

#include <algorithm>
#include <utility>

#include "media/bit_reader.h"

namespace psvp::media {

namespace {

using detail::Bits;
using detail::split_units;
using detail::unescape;
using detail::Unit;

// NAL unit types of H.264, table 7-1.
constexpr unsigned kSlice = 1, kPartitionA = 2, kIdrSlice = 5, kSequenceSet = 7, kPictureSet = 8;
constexpr unsigned kExtensionSlice = 20, kDepthSlice = 21;
// Enough of a slice for its header up to the reordering of its list: the longest reorderings a
// stream of sixteen frames kept can have.
constexpr std::size_t kSliceBeginning = 160;
constexpr unsigned kMaxUsed = 32;

std::unexpected<std::string> refuse(std::string why) { return std::unexpected(std::move(why)); }

// scaling_list() (7.3.2.1.1.1): only read past.
void skip_scaling_list(Bits& bits, unsigned size) {
    int last = 8, next = 8;
    for (unsigned j = 0; j < size && !bits.failed(); ++j) {
        if (next != 0) next = (last + bits.se_value() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

struct SequenceFacts {
    unsigned id = 0;
    unsigned frame_number_bits = 0;
    unsigned order_type = 0;
    unsigned order_bits = 0;
    bool order_deltas_zero = false;
    bool frames_only = true;
    bool colour_planes_apart = false;
    unsigned frames_kept = 0;
};

std::expected<SequenceFacts, std::string> read_sequence_set(std::span<const std::uint8_t> payload) {
    Bits bits(payload);
    SequenceFacts facts;
    const unsigned profile = bits.u(8);
    bits.skip(16);  // constraint flags, level_idc
    facts.id = bits.ue();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        const unsigned chroma = bits.ue();
        if (chroma == 3) facts.colour_planes_apart = bits.flag();
        (void)bits.ue();  // bit_depth_luma_minus8
        (void)bits.ue();  // bit_depth_chroma_minus8
        bits.skip(1);     // qpprime_y_zero_transform_bypass_flag
        if (bits.flag()) {  // seq_scaling_matrix_present_flag
            for (unsigned i = 0; i < (chroma != 3 ? 8U : 12U); ++i) {
                if (bits.flag()) skip_scaling_list(bits, i < 6 ? 16 : 64);
            }
        }
    }
    facts.frame_number_bits = bits.ue() + 4;
    facts.order_type = bits.ue();
    if (facts.order_type == 0) {
        facts.order_bits = bits.ue() + 4;
    } else if (facts.order_type == 1) {
        facts.order_deltas_zero = bits.flag();
        bits.se();  // offset_for_non_ref_pic
        bits.se();  // offset_for_top_to_bottom_field
        const unsigned cycle = bits.ue();
        if (cycle > 255) return refuse("the sequence parameter set is not readable");
        for (unsigned i = 0; i < cycle; ++i) bits.se();
    } else if (facts.order_type != 2) {
        return refuse("the sequence parameter set is not readable");
    }
    facts.frames_kept = bits.ue();
    bits.skip(1);       // gaps_in_frame_num_value_allowed_flag
    (void)bits.ue();    // pic_width_in_mbs_minus1
    (void)bits.ue();    // pic_height_in_map_units_minus1
    facts.frames_only = bits.flag();
    if (bits.failed() || facts.frame_number_bits > 16 || facts.order_bits > 16 || facts.frames_kept > 16) {
        return refuse("the sequence parameter set is not readable");
    }
    if (!facts.frames_only) return refuse("the stream may carry fields");
    return facts;
}

struct PictureFacts {
    unsigned id = 0;
    unsigned sequence_set = 0;
    bool bottom_order_present = false;
    bool redundant_counts = false;
    unsigned default_used = 1;
};

std::expected<PictureFacts, std::string> read_picture_set(std::span<const std::uint8_t> payload) {
    Bits bits(payload);
    PictureFacts facts;
    facts.id = bits.ue();
    facts.sequence_set = bits.ue();
    bits.skip(1);  // entropy_coding_mode_flag
    facts.bottom_order_present = bits.flag();
    if (bits.ue() != 0) return refuse("the stream's pictures are cut into slice groups");
    facts.default_used = bits.ue() + 1;
    (void)bits.ue();  // num_ref_idx_l1_default_active_minus1
    bits.skip(3);     // weighted_pred_flag, weighted_bipred_idc
    bits.se();        // pic_init_qp_minus26
    bits.se();        // pic_init_qs_minus26
    bits.se();        // chroma_qp_index_offset
    bits.skip(2);     // deblocking_filter_control_present_flag, constrained_intra_pred_flag
    facts.redundant_counts = bits.flag();
    if (bits.failed() || facts.default_used > kMaxUsed) return refuse("the picture parameter set is not readable");
    return facts;
}

unsigned unit_type(std::span<const std::uint8_t> unit) noexcept { return unit[0] & 0x1fU; }

// The frames one slice uses, from its header (7.3.3) and the reordering of its list (7.3.3.1,
// 8.2.4.3.1), as frame numbers back. `default_list` is the list before reordering.
struct Slice {
    unsigned type = 0;
    unsigned number = 0;
    std::vector<unsigned> used;
};
std::expected<Slice, std::string> read_slice(const H264Layout& layout, std::span<const std::uint8_t> unit) {
    const unsigned type_of_unit = unit_type(unit);
    const std::vector<std::uint8_t> payload = unescape(unit.subspan(1, std::min(unit.size() - 1, kSliceBeginning)));
    Bits bits(payload);
    Slice slice;
    (void)bits.ue();  // first_mb_in_slice
    slice.type = bits.ue() % 5;
    if (bits.ue() != layout.picture_set) return refuse("a slice names another picture parameter set");
    if (layout.colour_planes_apart) bits.skip(2);
    const unsigned max_number = 1U << layout.frame_number_bits;
    slice.number = bits.u(layout.frame_number_bits);
    if (type_of_unit == kIdrSlice) (void)bits.ue();  // idr_pic_id
    if (layout.order_type == 0) {
        bits.skip(layout.order_bits);
        if (layout.bottom_order_present) bits.se();
    } else if (layout.order_type == 1 && !layout.order_deltas_zero) {
        bits.se();
        if (layout.bottom_order_present) bits.se();
    }
    if (layout.redundant_counts && bits.ue() != 0) return refuse("the stream carries redundant pictures");
    constexpr unsigned kB = 1, kI = 2, kSi = 4;
    if (slice.type == kB) return refuse("the stream carries B frames");
    if (slice.type == kI || slice.type == kSi) {
        if (bits.failed()) return refuse("a slice header is cut short");
        return slice;
    }
    unsigned used = layout.default_used;
    if (bits.flag()) used = bits.ue() + 1;  // num_ref_idx_active_override_flag
    if (used > kMaxUsed) return refuse("a slice header is not readable");
    // The list before reordering (8.2.4.2.1): the frames held, newest first: 1, 2, 3, ... back.
    std::vector<unsigned> list;
    if (bits.flag()) {  // ref_pic_list_modification_flag_l0
        long back = 0;  // of picNumPred, from the frame's own number
        for (unsigned i = 0; i <= kMaxUsed; ++i) {
            const unsigned idc = bits.ue();
            if (bits.failed()) return refuse("a slice header is cut short");
            if (idc == 3) break;
            if (idc == 2) return refuse("the stream reorders its list with long-term frames");
            if (idc > 3 || i == kMaxUsed) return refuse("a slice header is not readable");
            const long difference = static_cast<long>(bits.ue()) + 1;
            back += idc == 0 ? difference : -difference;
            const long wrapped = ((back % static_cast<long>(max_number)) + static_cast<long>(max_number)) % static_cast<long>(max_number);
            if (wrapped == 0) return refuse("a slice reorders its list to the frame itself");
            const auto entry = static_cast<unsigned>(wrapped);
            std::erase(list, entry);
            list.push_back(entry);
        }
    }
    for (unsigned back = 1; list.size() < used && back < max_number; ++back) {
        if (std::ranges::find(list, back) == list.end()) list.push_back(back);
    }
    list.resize(std::min<std::size_t>(list.size(), used));
    if (bits.failed()) return refuse("a slice header is cut short");
    slice.used = std::move(list);
    return slice;
}

}  // namespace

std::expected<H264Layout, std::string> read_h264_layout(std::span<const std::uint8_t> bytes) {
    std::optional<SequenceFacts> sequence;
    std::optional<PictureFacts> picture;
    for (const Unit& found : split_units(bytes)) {
        const auto unit = bytes.subspan(found.begin, found.end - found.begin);
        if (unit.empty()) continue;
        const unsigned type = unit_type(unit);
        if (type != kSequenceSet && type != kPictureSet) continue;
        const std::vector<std::uint8_t> payload = unescape(unit.subspan(1));
        if (type == kSequenceSet) {
            if (sequence) return refuse("the stream has more than one sequence parameter set");
            auto read = read_sequence_set(payload);
            if (!read) return std::unexpected(read.error());
            sequence = *read;
        } else {
            if (picture) return refuse("the stream has more than one picture parameter set");
            auto read = read_picture_set(payload);
            if (!read) return std::unexpected(read.error());
            picture = *read;
        }
    }
    if (!sequence || !picture) return refuse("the header carries no parameter sets");
    if (picture->sequence_set != sequence->id) return refuse("the picture parameter set names another sequence parameter set");
    H264Layout layout;
    layout.sequence_set = sequence->id;
    layout.picture_set = picture->id;
    layout.frame_number_bits = sequence->frame_number_bits;
    layout.order_type = sequence->order_type;
    layout.order_bits = sequence->order_bits;
    layout.order_deltas_zero = sequence->order_deltas_zero;
    layout.frames_only = sequence->frames_only;
    layout.colour_planes_apart = sequence->colour_planes_apart;
    layout.bottom_order_present = picture->bottom_order_present;
    layout.redundant_counts = picture->redundant_counts;
    layout.default_used = picture->default_used;
    layout.frames_kept = sequence->frames_kept;
    return layout;
}

std::expected<H264Frame, std::string> read_h264_frame(const H264Layout& layout, std::span<const std::uint8_t> frame) {
    H264Frame read;
    bool any = false, all_intra = true;
    for (const Unit& found : split_units(frame)) {
        const auto unit = frame.subspan(found.begin, found.end - found.begin);
        if (unit.empty()) continue;
        const unsigned type = unit_type(unit);
        if (type == kSequenceSet || type == kPictureSet) {
            // A frame that carries parameter sets: they must be the stream's own shape.
            continue;
        }
        if (type == kPartitionA || type == kExtensionSlice || type == kDepthSlice) return refuse("the stream carries slices of another kind");
        if (type != kSlice && type != kIdrSlice) continue;
        auto slice = read_slice(layout, unit);
        if (!slice) return std::unexpected(slice.error());
        const bool reference = (unit[0] & 0x60U) != 0;  // nal_ref_idc
        if (!any) {
            read.number = slice->number;
            read.restart = type == kIdrSlice;
            read.reference = reference;
        } else if (slice->number != read.number || (type == kIdrSlice) != read.restart) {
            return refuse("the slices of a frame do not agree");
        }
        any = true;
        constexpr unsigned kI = 2, kSi = 4;
        if (slice->type != kI && slice->type != kSi) all_intra = false;
        for (const unsigned back : slice->used) {
            if (std::ranges::find(read.used, back) == read.used.end()) read.used.push_back(back);
        }
    }
    if (!any) return refuse("the frame carries no slice");
    read.intra = all_intra;
    if (read.intra) read.used.clear();
    return read;
}

H264Tracker::H264Tracker(std::span<const std::uint8_t> header) {
    auto layout = read_h264_layout(header);
    if (layout) layout_ = *layout;
    else off_ = layout.error();
}

H264Tracker::Verdict H264Tracker::unread(std::string why) {
    off_ = std::move(why);
    held_.clear();
    pending_.reset();
    Verdict verdict;
    verdict.way = Way::unread;
    verdict.exact = false;
    verdict.why = off_;
    return verdict;
}

H264Tracker::Verdict H264Tracker::look(std::uint16_t index, std::span<const std::uint8_t> frame) {
    pending_.reset();
    if (!layout_) return unread(off_);
    auto read = read_h264_frame(*layout_, frame);
    if (!read) return unread(read.error());
    if (!off_.empty() && !read->restart) return unread(off_);
    off_.clear();

    const unsigned max_number = 1U << layout_->frame_number_bits;
    Verdict verdict;
    if (read->restart) {
        held_.clear();
    } else if (!held_.empty()) {
        // Frame numbers skipped since the last frame held: lost frames, whose places the decoder
        // fills with the frame it got last. Only as many as it keeps can matter.
        const unsigned last = held_.back().number;
        unsigned skipped = (read->number + max_number - last - 1) % max_number;
        if (read->number == last) skipped = 0;
        skipped = std::min(skipped, std::max(layout_->frames_kept, 1U));
        for (unsigned i = skipped; i > 0; --i) held_.push_back(Held{(read->number + max_number - i) % max_number, false, true});
        while (held_.size() > std::max(layout_->frames_kept, 1U)) held_.pop_front();
    }

    bool exact = true, filled = false;
    for (const unsigned back : read->used) {
        const unsigned number = (read->number + max_number - (back % max_number)) % max_number;
        const auto at = std::ranges::find_if(held_.rbegin(), held_.rend(), [&](const Held& held) { return held.number == number; });
        if (at == held_.rend()) {
            Verdict none;
            none.way = Way::no_reference;
            none.exact = false;
            return none;
        }
        if (at->filled) filled = true;
        if (!at->exact) exact = false;
    }
    verdict.way = filled ? Way::changed : Way::as_sent;
    verdict.exact = exact;
    pending_ = Pending{index, read->number, exact, read->restart, read->reference};
    return verdict;
}

void H264Tracker::decoded(std::uint16_t index, bool gave_picture) {
    if (!pending_ || pending_->index != index) return;
    const Pending pending = *std::exchange(pending_, std::nullopt);
    if (!gave_picture || !pending.reference) return;
    held_.push_back(Held{pending.number, pending.exact, false});
    while (held_.size() > std::max(layout_->frames_kept, 1U)) held_.pop_front();
}

}  // namespace psvp::media
