#include "media/frame_refs.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace psvp::media {

namespace {

// NAL unit types of H.265, table 7-1.
constexpr unsigned kLastOrdinaryPicture = 9;   // 0 to 9: pictures that follow others
constexpr unsigned kFirstRandomAccess = 16;    // 16 to 21: pictures a decoder can begin with
constexpr unsigned kLastRandomAccess = 21;
constexpr unsigned kIdrFirst = 19, kIdrLast = 20;  // of those, the ones that carry no order count
constexpr unsigned kFirstNonPicture = 32;
constexpr unsigned kSequenceSet = 33, kPictureSet = 34;
// As much of a slice as its first bits can take: 16 named frames with the longest numbers.
constexpr std::size_t kSliceBeginning = 128;
constexpr unsigned kMaxNamed = 16;

// One NAL unit of a byte stream: its header and payload, between the start codes.
struct Unit {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<Unit> split_units(std::span<const std::uint8_t> bytes) {
    std::vector<Unit> units;
    const auto next_start = [&](std::size_t from) {
        for (std::size_t i = from; i + 2 < bytes.size(); ++i) {
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) return i;
        }
        return bytes.size();
    };
    for (std::size_t start = next_start(0); start < bytes.size();) {
        const std::size_t begin = start + 3;
        const std::size_t following = next_start(begin);
        // Zero bytes before the next start code, or at the end, are filling: a unit never ends in one.
        std::size_t end = following;
        while (end > begin && bytes[end - 1] == 0) --end;
        units.push_back(Unit{begin, end});
        start = following;
    }
    return units;
}

unsigned unit_type(std::span<const std::uint8_t> unit) noexcept { return (unit[0] >> 1) & 0x3fU; }
unsigned unit_layer(std::span<const std::uint8_t> unit) noexcept { return ((unit[0] & 1U) << 5) | (unit[1] >> 3); }

// A unit's bytes without the bytes that keep start codes from showing up inside it (a 3 after two zeros).
std::vector<std::uint8_t> unescape(std::span<const std::uint8_t> unit) {
    std::vector<std::uint8_t> out;
    out.reserve(unit.size());
    unsigned zeros = 0;
    for (const std::uint8_t byte : unit) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

// The other way round (H.265, 7.4.2): a 3 goes in before any byte of 0 to 3 that follows two zeros, and after a last zero.
std::vector<std::uint8_t> escape(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + payload.size() / 64 + 2);
    unsigned zeros = 0;
    for (const std::uint8_t byte : payload) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    if (!payload.empty() && payload.back() == 0) out.push_back(3);
    return out;
}

// Reads bits, the first bit of a byte being its highest. Past the end it reads zeros and remembers.
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> data) : data_(data) {}
    std::uint32_t u(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (at_ >= data_.size() * 8) {
                failed_ = true;
                return 0;
            }
            value = (value << 1) | ((static_cast<std::uint32_t>(data_[at_ >> 3]) >> (7 - (at_ & 7))) & 1U);
            ++at_;
        }
        return value;
    }
    bool flag() { return u(1) != 0; }
    void skip(std::size_t count) {
        if (count > data_.size() * 8 - at_) {
            failed_ = true;
            at_ = data_.size() * 8;
        } else {
            at_ += count;
        }
    }
    // An unsigned number of the variable-length kind, ue(v): so many zeros, a one, as many more bits.
    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!failed_ && u(1) == 0) {
            if (++zeros > 31) failed_ = true;
        }
        if (failed_) return 0;
        return ((std::uint32_t{1} << zeros) - 1U) + u(zeros);
    }
    void se() { (void)ue(); }  // a signed one takes the same bits
    [[nodiscard]] std::size_t position() const noexcept { return at_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

std::unexpected<std::string> refuse(std::string why) { return std::unexpected(std::move(why)); }

struct SequenceFacts {
    unsigned id = 0;
    unsigned order_bits = 0;
    unsigned address_bits = 0;
    bool color_planes_apart = false;
};

// profile_tier_level(1, sub_layers), H.265 7.3.3: nothing in it is needed.
void skip_profile_tier_level(Bits& bits, unsigned sub_layers) {
    bits.skip(88);  // the general profile's space, tier, number, compatibility and constraint flags
    bits.skip(8);   // the general level
    bool profile_present[8] = {};
    bool level_present[8] = {};
    for (unsigned i = 0; i < sub_layers; ++i) {
        profile_present[i] = bits.flag();
        level_present[i] = bits.flag();
    }
    if (sub_layers > 0) {
        for (unsigned i = sub_layers; i < 8; ++i) bits.skip(2);
    }
    for (unsigned i = 0; i < sub_layers; ++i) {
        if (profile_present[i]) bits.skip(88);
        if (level_present[i]) bits.skip(8);
    }
}

// seq_parameter_set_rbsp(), H.265 7.3.2.2, as far as the long-term flag.
std::expected<SequenceFacts, std::string> read_sequence_set(std::span<const std::uint8_t> unit) {
    Bits bits(unit);
    bits.skip(16);  // the unit's header
    bits.skip(4);   // the video parameter set's number
    const unsigned sub_layers = bits.u(3);
    bits.skip(1);
    if (sub_layers > 6) return refuse("the sequence parameter set counts more sub-layers than there are");
    skip_profile_tier_level(bits, sub_layers);
    SequenceFacts facts;
    facts.id = bits.ue();
    const unsigned chroma = bits.ue();
    if (chroma > 3) return refuse("the sequence parameter set names no known color layout");
    if (chroma == 3) facts.color_planes_apart = bits.flag();
    const std::uint64_t width = bits.ue();
    const std::uint64_t height = bits.ue();
    if (bits.flag()) {
        for (int i = 0; i < 4; ++i) (void)bits.ue();  // the part of the picture that is shown
    }
    (void)bits.ue();  // sample depths
    (void)bits.ue();
    const unsigned order_bits = bits.ue() + 4;
    const bool ordering_per_layer = bits.flag();
    for (unsigned i = ordering_per_layer ? 0 : sub_layers; i <= sub_layers; ++i) {
        (void)bits.ue();
        (void)bits.ue();
        (void)bits.ue();
    }
    const unsigned smallest_block = bits.ue() + 3;  // as a power of two
    const unsigned block_range = bits.ue();
    for (int i = 0; i < 4; ++i) (void)bits.ue();  // transform block sizes and depths
    if (bits.flag() && bits.flag()) return refuse("the sequence parameter set carries scaling lists");
    bits.skip(2);  // asymmetric partitions, sample adaptive offset
    if (bits.flag()) {  // samples sent as they are
        bits.skip(8);
        (void)bits.ue();
        (void)bits.ue();
        bits.skip(1);
    }
    const unsigned reference_sets = bits.ue();
    if (bits.failed()) return refuse("the sequence parameter set is cut short");
    if (reference_sets != 0) return refuse("the sequence parameter set carries reference sets");
    const bool long_term = bits.flag();
    if (bits.failed()) return refuse("the sequence parameter set is cut short");
    if (long_term) return refuse("the stream keeps long-term frames");
    if (order_bits > 16) return refuse("the sequence parameter set gives the order count more than 16 bits");
    const std::uint64_t block_log2 = std::uint64_t{smallest_block} + block_range;
    if (block_log2 < 4 || block_log2 > 6) return refuse("the sequence parameter set names no known block size");
    if (width == 0 || height == 0 || width > 65535 || height > 65535) return refuse("the sequence parameter set names no picture size");
    const std::uint64_t block = std::uint64_t{1} << block_log2;
    const std::uint64_t blocks = ((width + block - 1) / block) * ((height + block - 1) / block);
    facts.order_bits = order_bits;
    facts.address_bits = static_cast<unsigned>(std::bit_width(blocks - 1));
    return facts;
}

struct PictureFacts {
    unsigned id = 0;
    unsigned sequence_id = 0;
    unsigned extra_bits = 0;
    bool dependent_slices = false;
    bool output_flag = false;
    bool lists_changeable = false;
};

// pic_parameter_set_rbsp(), H.265 7.3.2.3, as far as lists_modification_present_flag.
std::expected<PictureFacts, std::string> read_picture_set(std::span<const std::uint8_t> unit) {
    Bits bits(unit);
    bits.skip(16);
    PictureFacts facts;
    facts.id = bits.ue();
    facts.sequence_id = bits.ue();
    facts.dependent_slices = bits.flag();
    facts.output_flag = bits.flag();
    facts.extra_bits = bits.u(3);
    bits.skip(2);     // sign hiding, a choice of start tables
    (void)bits.ue();  // frames used unless a slice says otherwise, for each list
    (void)bits.ue();
    bits.se();        // the first quantizer
    bits.skip(2);     // constrained intra prediction, transform skip
    if (bits.flag()) (void)bits.ue();  // quantizer changes within a block
    bits.se();        // color quantizer offsets
    bits.se();
    bits.skip(4);     // slice color offsets, weighted prediction of both kinds, bypass
    const bool tiles = bits.flag();
    bits.skip(1);     // rows decoded side by side
    if (tiles) {
        const std::uint32_t columns = bits.ue();
        const std::uint32_t rows = bits.ue();
        if (bits.failed() || columns > 64 || rows > 64) return refuse("the picture parameter set is cut short");
        if (!bits.flag()) {  // not spaced evenly: each one's size
            for (std::uint32_t i = 0; i < columns; ++i) (void)bits.ue();
            for (std::uint32_t i = 0; i < rows; ++i) (void)bits.ue();
        }
        bits.skip(1);  // filtering across tiles
    }
    bits.skip(1);  // filtering across slices
    if (bits.flag()) {  // deblocking control
        bits.skip(1);
        if (!bits.flag()) {
            bits.se();
            bits.se();
        }
    }
    const bool scaling_lists = bits.flag();
    if (bits.failed()) return refuse("the picture parameter set is cut short");
    if (scaling_lists) return refuse("the picture parameter set carries scaling lists");
    facts.lists_changeable = bits.flag();
    if (bits.failed()) return refuse("the picture parameter set is cut short");
    return facts;
}

// The beginning of a slice: slice_segment_header(), H.265 7.3.6.1, as far as the frames it names.
struct Slice {
    bool dependent = false;  // it goes on with another slice's beginning: it names nothing itself
    unsigned type = 0;       // 0 predicted from two frames at once, 1 from one, 2 from none
    unsigned order = 0;
    struct Entry {
        unsigned back = 0;
        bool used = false;
        std::size_t bit = 0;  // where its "used" flag is, in bits from the unit's start
    };
    std::vector<Entry> named;
};

// `unit` begins with the unit's two header bytes and has no emulation prevention bytes.
std::expected<Slice, std::string> read_slice(const HevcLayout& layout, std::span<const std::uint8_t> unit, unsigned type) {
    Bits bits(unit);
    bits.skip(16);
    Slice slice;
    const bool first = bits.flag();
    if (type >= kFirstRandomAccess && type <= 23) bits.skip(1);
    const unsigned parameter_set = bits.ue();
    if (bits.failed()) return refuse("a slice is cut short");
    if (parameter_set != layout.parameter_set) return refuse("a slice names a parameter set that is not the stream's");
    if (!first) {
        if (layout.dependent_slices) slice.dependent = bits.flag();
        bits.skip(layout.address_bits);
    }
    if (slice.dependent) {
        if (bits.failed()) return refuse("a slice is cut short");
        return slice;
    }
    bits.skip(layout.extra_bits);
    slice.type = bits.ue();
    if (bits.failed()) return refuse("a slice is cut short");
    if (slice.type > 2) return refuse("a slice is of no known kind");
    if (layout.output_flag) bits.skip(1);
    if (layout.color_planes_apart) bits.skip(2);
    if (type < kIdrFirst || type > kIdrLast) {
        slice.order = bits.u(layout.order_bits);
        const bool from_parameter_sets = bits.flag();
        if (bits.failed()) return refuse("a slice is cut short");
        if (from_parameter_sets) return refuse("a slice takes its reference set from the parameter sets");
        // st_ref_pic_set(0), H.265 7.3.7: the first set of a stream has no "predicted from another set" flag.
        const std::uint32_t earlier = bits.ue();
        const std::uint32_t later = bits.ue();
        if (bits.failed()) return refuse("a slice is cut short");
        if (earlier > kMaxNamed || later > kMaxNamed) return refuse("a slice names more frames than a decoder keeps");
        if (later != 0) return refuse("a slice names frames that are shown after it");
        unsigned back = 0;
        for (std::uint32_t i = 0; i < earlier; ++i) {
            const std::uint32_t step = bits.ue();
            if (bits.failed() || step > 0x7fff) return refuse("a slice is cut short");
            back += step + 1;
            Slice::Entry entry;
            entry.back = back;
            entry.bit = bits.position();
            entry.used = bits.flag();
            slice.named.push_back(entry);
        }
    }
    if (bits.failed()) return refuse("a slice is cut short");
    return slice;
}

// Whether a unit is a picture's slice of the stream's own layer; refuses unit types that have no meaning yet.
std::expected<bool, std::string> is_slice(std::span<const std::uint8_t> unit) {
    if (unit.size() < 2 || unit_layer(unit) != 0) return false;
    const unsigned type = unit_type(unit);
    if (type >= kFirstNonPicture) return false;
    if (type <= kLastOrdinaryPicture || (type >= kFirstRandomAccess && type <= kLastRandomAccess)) return true;
    return refuse("a picture unit is of a kind that has no meaning yet");
}

bool same_names(const Slice& a, const Slice& b) {
    if (a.order != b.order || a.named.size() != b.named.size()) return false;
    for (std::size_t i = 0; i < a.named.size(); ++i) {
        if (a.named[i].back != b.named[i].back || a.named[i].used != b.named[i].used) return false;
    }
    return true;
}

}  // namespace

bool carries_hevc_parameter_sets(std::span<const std::uint8_t> bytes) noexcept {
    // The same walk as split_units, without its list: this runs on every frame.
    for (std::size_t i = 0; i + 4 < bytes.size(); ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1) continue;
        const std::span<const std::uint8_t> unit = bytes.subspan(i + 3);
        const unsigned type = unit_type(unit);
        if ((type == kSequenceSet || type == kPictureSet) && unit_layer(unit) == 0) return true;
        // A picture's slices are a frame's last units, and the long ones.
        if (type < kFirstNonPicture) return false;
        i += 2;
    }
    return false;
}

std::expected<HevcLayout, std::string> read_hevc_layout(std::span<const std::uint8_t> bytes) {
    std::optional<std::vector<std::uint8_t>> sequence_set, picture_set;
    for (const Unit& at : split_units(bytes)) {
        const std::span<const std::uint8_t> unit = bytes.subspan(at.begin, at.end - at.begin);
        if (unit.size() < 2 || unit_layer(unit) != 0) continue;
        const unsigned type = unit_type(unit);
        if (type != kSequenceSet && type != kPictureSet) continue;
        auto& kept = type == kSequenceSet ? sequence_set : picture_set;
        std::vector<std::uint8_t> payload = unescape(unit);
        if (kept && *kept != payload) return refuse("the stream has more than one parameter set of a kind");
        kept = std::move(payload);
    }
    if (!sequence_set || !picture_set) return refuse("the stream's parameter sets are not all there");
    const auto sequence = read_sequence_set(*sequence_set);
    if (!sequence) return refuse(sequence.error());
    const auto picture = read_picture_set(*picture_set);
    if (!picture) return refuse(picture.error());
    if (picture->sequence_id != sequence->id) return refuse("the picture parameter set belongs to another sequence parameter set");
    HevcLayout layout;
    layout.parameter_set = picture->id;
    layout.order_bits = sequence->order_bits;
    layout.address_bits = sequence->address_bits;
    layout.extra_bits = picture->extra_bits;
    layout.dependent_slices = picture->dependent_slices;
    layout.output_flag = picture->output_flag;
    layout.color_planes_apart = sequence->color_planes_apart;
    layout.lists_changeable = picture->lists_changeable;
    return layout;
}

std::expected<HevcFrame, std::string> read_hevc_frame(const HevcLayout& layout, std::span<const std::uint8_t> frame) {
    std::optional<Slice> first;
    HevcFrame result;
    result.intra = true;
    for (const Unit& at : split_units(frame)) {
        const std::span<const std::uint8_t> unit = frame.subspan(at.begin, at.end - at.begin);
        const auto picture_unit = is_slice(unit);
        if (!picture_unit) return refuse(picture_unit.error());
        if (!*picture_unit) continue;
        const unsigned type = unit_type(unit);
        const std::vector<std::uint8_t> beginning = unescape(unit.first(std::min(unit.size(), kSliceBeginning)));
        const auto slice = read_slice(layout, beginning, type);
        if (!slice) return refuse(slice.error());
        if (slice->dependent) continue;
        const bool restart = type >= kFirstRandomAccess;
        if (!first) {
            first = *slice;
            result.restart = restart;
        } else if (!same_names(*first, *slice) || result.restart != restart) {
            return refuse("the slices of a frame name different frames");
        }
        if (slice->type != 2) result.intra = false;
    }
    if (!first) return refuse("the frame has no picture in it");
    if (result.restart && !result.intra) return refuse("a random access picture is predicted from another frame");
    result.order = first->order;
    result.named.reserve(first->named.size());
    for (const Slice::Entry& entry : first->named) result.named.push_back(NamedFrame{entry.back, entry.used});
    return result;
}

std::expected<std::vector<std::uint8_t>, std::string> with_used_frames(const HevcLayout& layout, std::span<const std::uint8_t> frame,
                                                                       const std::vector<bool>& used) {
    std::vector<std::uint8_t> out;
    out.reserve(frame.size() + 8);
    std::size_t copied = 0;
    bool any = false;
    for (const Unit& at : split_units(frame)) {
        const std::span<const std::uint8_t> unit = frame.subspan(at.begin, at.end - at.begin);
        const auto picture_unit = is_slice(unit);
        if (!picture_unit) return refuse(picture_unit.error());
        if (!*picture_unit) continue;
        std::vector<std::uint8_t> payload = unescape(unit);
        const auto slice = read_slice(layout, payload, unit_type(unit));
        if (!slice) return refuse(slice.error());
        if (slice->dependent) continue;
        if (slice->named.size() != used.size()) return refuse("a slice names another number of frames");
        // Only a unit that comes back byte for byte when nothing is changed is touched.
        const std::vector<std::uint8_t> same = escape(payload);
        if (same.size() != unit.size() || !std::equal(same.begin(), same.end(), unit.begin())) {
            return refuse("a slice's bytes are not laid out by the rules");
        }
        for (std::size_t i = 0; i < used.size(); ++i) {
            const std::size_t bit = slice->named[i].bit;
            const auto mask = static_cast<std::uint8_t>(0x80U >> (bit & 7));
            if (used[i]) payload[bit >> 3] = static_cast<std::uint8_t>(payload[bit >> 3] | mask);
            else payload[bit >> 3] = static_cast<std::uint8_t>(payload[bit >> 3] & ~mask);
        }
        const std::vector<std::uint8_t> escaped = escape(payload);
        out.insert(out.end(), frame.begin() + static_cast<std::ptrdiff_t>(copied), frame.begin() + static_cast<std::ptrdiff_t>(at.begin));
        out.insert(out.end(), escaped.begin(), escaped.end());
        copied = at.end;
        any = true;
    }
    if (!any) return refuse("the frame has no picture in it");
    out.insert(out.end(), frame.begin() + static_cast<std::ptrdiff_t>(copied), frame.end());
    return out;
}

ReferenceTracker::ReferenceTracker(std::span<const std::uint8_t> header) {
    if (auto layout = read_hevc_layout(header)) layout_ = *layout;
    else off_ = layout.error();
}

const ReferenceTracker::Held* ReferenceTracker::held(std::uint16_t index) const noexcept {
    for (const Held& entry : held_) {
        if (entry.index == index) return &entry;
    }
    return nullptr;
}

ReferenceTracker::Verdict ReferenceTracker::unread(std::string why) {
    // What was known of the frames before is of no use behind a frame that was not read.
    held_.clear();
    anchor_.reset();
    off_ = std::move(why);
    Verdict verdict;
    verdict.way = Way::unread;
    verdict.why = off_;
    return verdict;
}

ReferenceTracker::Verdict ReferenceTracker::look(std::uint16_t index, std::span<const std::uint8_t> frame) {
    pending_.reset();
    if (carries_hevc_parameter_sets(frame)) {
        auto layout = read_hevc_layout(frame);
        if (!layout) {
            layout_.reset();
            return unread(layout.error());
        }
        layout_ = *layout;
    }
    if (!layout_) return unread(off_.empty() ? std::string("the stream's parameter sets are not all there") : off_);
    const auto info = read_hevc_frame(*layout_, frame);
    if (!info) return unread(info.error());
    if (!off_.empty()) {
        // Reading begins again with a picture that needs nothing from before.
        if (!info->restart) return unread(off_);
        off_.clear();
    }

    Verdict verdict;
    if (info->restart) {
        pending_ = Pending{index, info->order, true, true};
        return verdict;
    }
    if (anchor_) {
        const auto step = static_cast<std::uint16_t>(index - anchor_->index);
        if (step == 0 || step >= 0x8000U) return unread("the frames do not come in the stream's order");
        const unsigned modulo = 1U << layout_->order_bits;
        if (step > modulo / 2) {
            // Too far from the last frame held for a decoder to place this one's order count.
            held_.clear();
            anchor_.reset();
        } else if (((info->order - anchor_->order) & (modulo - 1)) != (step & (modulo - 1))) {
            return unread("a frame's order count does not follow its number in the stream");
        }
    }
    if (info->intra) {
        pending_ = Pending{index, info->order, true, false};
        return verdict;
    }

    const auto held_back = [&](unsigned back) -> const Held* {
        return back < 0x8000U ? held(static_cast<std::uint16_t>(index - back)) : nullptr;
    };
    std::vector<bool> used(info->named.size(), false);
    unsigned before = 0, now = 0;
    bool exact = true;
    for (std::size_t i = 0; i < info->named.size(); ++i) {
        if (!info->named[i].used) continue;
        ++before;
        if (const Held* entry = held_back(info->named[i].back)) {
            used[i] = true;
            ++now;
            exact = exact && entry->exact;
        }
    }
    if (before == 0) return unread("a frame is predicted from other frames and uses none it names");
    if (now == before) {
        verdict.exact = exact;
        pending_ = Pending{index, info->order, exact, false};
        return verdict;
    }
    // In place of each frame that is not held: the nearest frame held that it names and did not use.
    for (std::size_t i = 0; i < info->named.size() && now < before; ++i) {
        if (used[i] || held_back(info->named[i].back) == nullptr) continue;
        used[i] = true;
        ++now;
    }
    if (now == 0 || (now != before && layout_->lists_changeable)) {
        verdict.way = Way::no_reference;
        verdict.exact = false;
        return verdict;
    }
    auto changed = with_used_frames(*layout_, frame, used);
    if (!changed) return unread(changed.error());
    verdict.way = Way::changed;
    verdict.exact = false;
    verdict.changed = std::move(*changed);
    pending_ = Pending{index, info->order, false, false};
    return verdict;
}

void ReferenceTracker::decoded(std::uint16_t index, bool gave_picture) {
    if (!pending_ || pending_->index != index) return;
    const Pending pending = *pending_;
    pending_.reset();
    if (!gave_picture) return;
    if (pending.restart) held_.clear();
    held_.push_back(Held{index, pending.exact});
    if (held_.size() > kKept) held_.pop_front();
    anchor_ = pending;
}

}  // namespace psvp::media
