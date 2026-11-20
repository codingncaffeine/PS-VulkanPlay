#include "media/test_clip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace psvp::media {

namespace {

constexpr ClipBlock kWhite{235, 128, 128};
constexpr ClipBlock kBlack{16, 128, 128};
constexpr ClipBlock kGray{64, 128, 128};
// Color bars at three quarters of full strength, as BT.709 samples in the limited range.
constexpr std::array<ClipBlock, 8> kPatches{{
    {180, 128, 128}, {168, 44, 136}, {145, 147, 44}, {133, 63, 52},
    {63, 193, 204}, {51, 109, 212}, {28, 212, 120}, {16, 128, 128},
}};

// Bits, highest first, as H.264 writes them (ITU-T H.264, clause 7.2 and 9.1).
class BitWriter {
public:
    void bits(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            if (used_ == 0) bytes_.push_back(0);
            if (((value >> i) & 1U) != 0) bytes_.back() |= static_cast<std::uint8_t>(0x80U >> used_);
            used_ = (used_ + 1) % 8;
        }
    }
    // An unsigned number in the exponential Golomb code: ue(v).
    void ue(std::uint32_t value) {
        const std::uint32_t coded = value + 1;
        const auto width = static_cast<unsigned>(std::bit_width(coded));
        bits(0, width - 1);
        bits(coded, width);
    }
    // A signed one: se(v).
    void se(std::int32_t value) { ue(value > 0 ? static_cast<std::uint32_t>(2 * value - 1) : static_cast<std::uint32_t>(-2 * value)); }
    void align() { used_ = 0; }  // the bits left of the last byte stay zero
    void byte(std::uint8_t value) { bytes_.push_back(value); }  // only when aligned
    // rbsp_trailing_bits: a one, then zeros to the end of the byte.
    void finish() {
        bits(1, 1);
        align();
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    unsigned used_ = 0;  // bits used of the last byte; 0: none begun
};

// A NAL unit behind a four-byte start code. Inside it, two zero bytes are never followed
// by a byte below four: a 3 is put in between (clause 7.4.1).
void append_unit(std::vector<std::uint8_t>& out, std::uint8_t header, const std::vector<std::uint8_t>& payload) {
    out.insert(out.end(), {0, 0, 0, 1, header});
    unsigned zeros = 0;
    for (const std::uint8_t byte : payload) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
}

constexpr unsigned kFrameNumberBits = 8;
constexpr std::uint8_t kUnitSequenceSet = 0x67, kUnitPictureSet = 0x68, kUnitKeySlice = 0x65, kUnitSlice = 0x41;
constexpr std::uint32_t kPcmInKeySlice = 25, kPcmInOtherSlice = 30;  // mb_type of I_PCM (tables 7-11, 7-13)

// A macroblock whose 384 samples are written as they are.
void pcm_block(BitWriter& writer, std::uint32_t type, const ClipBlock& block) {
    writer.ue(type);
    writer.align();
    for (unsigned i = 0; i < 256; ++i) writer.byte(block.y);
    for (unsigned i = 0; i < 64; ++i) writer.byte(block.cb);
    for (unsigned i = 0; i < 64; ++i) writer.byte(block.cr);
}

}  // namespace

ClipBlock clip_block(std::uint32_t counter, unsigned column, unsigned row, unsigned columns, unsigned rows) {
    if (row <= 1 && column < kClipCounterBits) {
        const bool bit = ((counter >> (kClipCounterBits - 1 - column)) & 1U) != 0;
        return bit == (row == 0) ? kWhite : kBlack;
    }
    if (row == 2 && column < kPatches.size()) return kPatches[column];
    if (row >= 3 && rows > 3) {
        const unsigned places = columns * (rows - 3);
        if ((row - 3) * columns + column == counter % places) return kWhite;
    }
    return kGray;
}

std::optional<std::uint16_t> read_clip_counter(const PlanesView& picture) {
    if (picture.y == nullptr || picture.width < kClipMinWidth || picture.height < 2 * kClipBlock) return std::nullopt;
    std::uint16_t counter = 0;
    for (unsigned column = 0; column < kClipCounterBits; ++column) {
        const std::size_t x = column * kClipBlock + kClipBlock / 2;
        const bool first = picture.y[(kClipBlock / 2) * picture.y_stride + x] >= 128;
        const bool second = picture.y[(kClipBlock + kClipBlock / 2) * picture.y_stride + x] >= 128;
        if (first == second) return std::nullopt;
        counter = static_cast<std::uint16_t>((static_cast<unsigned>(counter) << 1) | (first ? 1U : 0U));
    }
    return counter;
}

std::size_t clip_differences(const PlanesView& picture, std::uint32_t counter) {
    const unsigned columns = (picture.width + kClipBlock - 1) / kClipBlock;
    const unsigned rows = (picture.height + kClipBlock - 1) / kClipBlock;
    std::size_t differences = 0;
    for (unsigned y = 0; y < picture.height; ++y) {
        for (unsigned x = 0; x < picture.width; ++x) {
            const ClipBlock block = clip_block(counter, x / kClipBlock, y / kClipBlock, columns, rows);
            if (picture.y[y * picture.y_stride + x] != block.y) ++differences;
            if (x % 2 == 0 && y % 2 == 0) {
                if (picture.cb[(y / 2) * picture.cb_stride + x / 2] != block.cb) ++differences;
                if (picture.cr[(y / 2) * picture.cr_stride + x / 2] != block.cr) ++differences;
            }
        }
    }
    return differences;
}

ClipWriter::ClipWriter(unsigned width, unsigned height)
    : width_(width), height_(height), columns_((width + kClipBlock - 1) / kClipBlock), rows_((height + kClipBlock - 1) / kClipBlock) {
    if (width < kClipMinWidth || height < kClipMinHeight || width % 2 != 0 || height % 2 != 0 || width > 4096 || height > 2304) {
        throw std::invalid_argument("a test clip of this size cannot be written");
    }
}

std::vector<ClipBlock> ClipWriter::blocks(std::uint32_t counter) const {
    std::vector<ClipBlock> all;
    all.reserve(static_cast<std::size_t>(columns_) * rows_);
    for (unsigned row = 0; row < rows_; ++row) {
        for (unsigned column = 0; column < columns_; ++column) all.push_back(clip_block(counter, column, row, columns_, rows_));
    }
    return all;
}

std::vector<std::uint8_t> ClipWriter::parameter_sets() const {
    // seq_parameter_set_data (clause 7.3.2.1.1) with the picture's colors and the promise of no reordering.
    BitWriter sequence;
    sequence.bits(66, 8);    // profile_idc: Baseline
    sequence.bits(0xc0, 8);  // constraint_set0_flag and constraint_set1_flag
    sequence.bits(40, 8);    // level_idc
    sequence.ue(0);          // seq_parameter_set_id
    sequence.ue(kFrameNumberBits - 4);  // log2_max_frame_num_minus4
    sequence.ue(2);          // pic_order_cnt_type: pictures are shown in the order they are decoded
    sequence.ue(1);          // max_num_ref_frames
    sequence.bits(0, 1);     // gaps_in_frame_num_value_allowed_flag
    sequence.ue(columns_ - 1);
    sequence.ue(rows_ - 1);
    sequence.bits(1, 1);     // frame_mbs_only_flag
    sequence.bits(1, 1);     // direct_8x8_inference_flag
    const unsigned crop_right = (columns_ * kClipBlock - width_) / 2, crop_bottom = (rows_ * kClipBlock - height_) / 2;
    const bool cropped = crop_right != 0 || crop_bottom != 0;
    sequence.bits(cropped ? 1 : 0, 1);
    if (cropped) {
        sequence.ue(0);
        sequence.ue(crop_right);
        sequence.ue(0);
        sequence.ue(crop_bottom);
    }
    sequence.bits(1, 1);     // vui_parameters_present_flag (annex E.1.1)
    sequence.bits(0, 1);     // aspect_ratio_info_present_flag
    sequence.bits(0, 1);     // overscan_info_present_flag
    sequence.bits(1, 1);     // video_signal_type_present_flag
    sequence.bits(5, 3);     // video_format: unspecified
    sequence.bits(0, 1);     // video_full_range_flag: the limited range, as a console sends
    sequence.bits(1, 1);     // colour_description_present_flag
    sequence.bits(1, 8);     // colour_primaries: BT.709
    sequence.bits(1, 8);     // transfer_characteristics: BT.709
    sequence.bits(1, 8);     // matrix_coefficients: BT.709
    sequence.bits(0, 1);     // chroma_loc_info_present_flag
    sequence.bits(0, 1);     // timing_info_present_flag
    sequence.bits(0, 1);     // nal_hrd_parameters_present_flag
    sequence.bits(0, 1);     // vcl_hrd_parameters_present_flag
    sequence.bits(0, 1);     // pic_struct_present_flag
    sequence.bits(1, 1);     // bitstream_restriction_flag
    sequence.bits(1, 1);     // motion_vectors_over_pic_boundaries_flag
    sequence.ue(0);          // max_bytes_per_pic_denom
    sequence.ue(0);          // max_bits_per_mb_denom
    sequence.ue(10);         // log2_max_mv_length_horizontal
    sequence.ue(10);         // log2_max_mv_length_vertical
    sequence.ue(0);          // max_num_reorder_frames
    sequence.ue(1);          // max_dec_frame_buffering
    sequence.finish();

    // pic_parameter_set_rbsp (clause 7.3.2.2).
    BitWriter picture;
    picture.ue(0);           // pic_parameter_set_id
    picture.ue(0);           // seq_parameter_set_id
    picture.bits(0, 1);      // entropy_coding_mode_flag
    picture.bits(0, 1);      // bottom_field_pic_order_in_frame_present_flag
    picture.ue(0);           // num_slice_groups_minus1
    picture.ue(0);           // num_ref_idx_l0_default_active_minus1
    picture.ue(0);           // num_ref_idx_l1_default_active_minus1
    picture.bits(0, 1);      // weighted_pred_flag
    picture.bits(0, 2);      // weighted_bipred_idc
    picture.se(0);           // pic_init_qp_minus26
    picture.se(0);           // pic_init_qs_minus26
    picture.se(0);           // chroma_qp_index_offset
    picture.bits(1, 1);      // deblocking_filter_control_present_flag
    picture.bits(0, 1);      // constrained_intra_pred_flag
    picture.bits(0, 1);      // redundant_pic_cnt_present_flag
    picture.finish();

    std::vector<std::uint8_t> out;
    append_unit(out, kUnitSequenceSet, sequence.bytes());
    append_unit(out, kUnitPictureSet, picture.bytes());
    return out;
}

void ClipWriter::decoder_lost(std::uint32_t first, std::uint32_t last) {
    // In sixteen bits, as a stream counts its frames: the count may have started again in between.
    const auto lost = [&](const Written& written) {
        return static_cast<std::uint16_t>(written.counter - first) <= static_cast<std::uint16_t>(last - first);
    };
    const auto begin = std::ranges::find_if(history_, lost);
    if (begin == history_.end()) return;
    // From what the decoder showed before the first lost frame, go through the frames since
    // again: a lost one changed nothing, every other one put its blocks where they belong.
    std::vector<ClipBlock> shown = begin->before;
    for (auto written = begin; written != history_.end(); ++written) {
        written->before = shown;
        if (lost(*written)) {
            written->carried.clear();
            continue;
        }
        for (const auto& [index, block] : written->carried) shown[index] = block;
    }
    decoder_ = std::move(shown);
}

std::vector<std::uint8_t> ClipWriter::frame(std::uint32_t counter, bool keyframe) {
    if (!decoder_) keyframe = true;
    const std::vector<ClipBlock> wanted = blocks(counter);
    if (keyframe) frame_number_ = 0;

    // slice_header (clause 7.3.3) and slice_data (7.3.4) of one slice that is the whole picture.
    BitWriter slice;
    slice.ue(0);                 // first_mb_in_slice
    slice.ue(keyframe ? 7 : 5);  // slice_type: every slice of the picture is I, or P
    slice.ue(0);                 // pic_parameter_set_id
    slice.bits(frame_number_ % (1U << kFrameNumberBits), kFrameNumberBits);
    if (keyframe) slice.ue(keyframes_++ % 2);  // idr_pic_id: two keyframes in a row differ in it
    if (!keyframe) {
        slice.bits(0, 1);        // num_ref_idx_active_override_flag
        slice.bits(0, 1);        // ref_pic_list_modification_flag_l0
    }
    if (keyframe) {
        slice.bits(0, 1);        // no_output_of_prior_pics_flag
        slice.bits(0, 1);        // long_term_reference_flag
    } else {
        slice.bits(0, 1);        // adaptive_ref_pic_marking_mode_flag
    }
    slice.se(0);                 // slice_qp_delta
    slice.ue(1);                 // disable_deblocking_filter_idc: no filter, the samples stay as written

    Written written;
    written.counter = counter;
    if (decoder_) written.before = *decoder_;
    if (keyframe) {
        for (const ClipBlock& block : wanted) pcm_block(slice, kPcmInKeySlice, block);
        // Nothing before a keyframe matters to the decoder any more.
        history_.clear();
    } else {
        // Blocks that did not change are skipped: the decoder keeps what it has there.
        std::uint32_t skipped = 0;
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (wanted[i] == (*decoder_)[i]) {
                ++skipped;
                continue;
            }
            slice.ue(skipped);   // mb_skip_run
            skipped = 0;
            pcm_block(slice, kPcmInOtherSlice, wanted[i]);
            written.carried.emplace_back(static_cast<std::uint32_t>(i), wanted[i]);
        }
        if (skipped != 0) slice.ue(skipped);
    }
    slice.finish();

    decoder_ = wanted;
    if (!keyframe) {
        history_.push_back(std::move(written));
        if (history_.size() > kHistory) history_.pop_front();
    }
    ++frame_number_;
    std::vector<std::uint8_t> out;
    append_unit(out, keyframe ? kUnitKeySlice : kUnitSlice, slice.bytes());
    return out;
}

}  // namespace psvp::media
