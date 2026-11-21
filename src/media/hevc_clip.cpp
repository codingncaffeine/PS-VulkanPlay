#include "media/hevc_clip.h"

#include <algorithm>
#include <array>

#include "media/bit_writer.h"

namespace psvp::media {

namespace {

using detail::append_unit;
using detail::BitWriter;

// Every block of the clip is one coding tree block and one coding unit of 16x16 samples.
constexpr unsigned kLog2Block = 4;
constexpr unsigned kOrderBits = 4;   // as a PS5's
constexpr unsigned kLevel = 153;     // 5.1: sixteen frames held up to 2,228,224 samples a picture
constexpr std::uint8_t kSliceQp = 26;

// Unit types (table 7-1), written as the first byte of the two-byte unit header.
constexpr std::uint8_t kTrailing = 1, kKeyframe = 19 /* IDR_W_RADL */, kVideoSet = 32, kSequenceSet = 33, kPictureSet = 34;
constexpr std::uint8_t header0(std::uint8_t type) { return static_cast<std::uint8_t>(type << 1); }
constexpr std::uint8_t kHeader1 = 1;  // layer 0, temporal id 0

// The arithmetic coder's tables, the same as H.264's (H.265 tables 9-52 and 9-53).
constexpr std::array<std::array<std::uint8_t, 4>, 64> kRangeLps{{
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205},
    {116, 142, 169, 195}, {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166},
    {95, 116, 137, 158}, {90, 110, 130, 150}, {85, 104, 123, 142}, {81, 99, 117, 135},
    {77, 94, 111, 128}, {73, 89, 105, 122}, {69, 85, 100, 116}, {66, 80, 95, 110},
    {62, 76, 90, 104}, {59, 72, 86, 99}, {56, 69, 81, 94}, {53, 65, 77, 89},
    {51, 62, 73, 85}, {48, 59, 69, 80}, {46, 56, 66, 76}, {43, 53, 63, 72},
    {41, 50, 59, 69}, {39, 48, 56, 65}, {37, 45, 54, 62}, {35, 43, 51, 59},
    {33, 41, 48, 56}, {32, 39, 46, 53}, {30, 37, 43, 50}, {29, 35, 41, 48},
    {27, 33, 39, 45}, {26, 31, 37, 43}, {24, 30, 35, 41}, {23, 28, 33, 39},
    {22, 27, 32, 37}, {21, 26, 30, 35}, {20, 24, 29, 33}, {19, 23, 27, 31},
    {18, 22, 26, 30}, {17, 21, 25, 28}, {16, 20, 23, 27}, {15, 19, 22, 25},
    {14, 18, 21, 24}, {14, 17, 20, 23}, {13, 16, 19, 22}, {12, 15, 18, 21},
    {12, 14, 17, 20}, {11, 14, 16, 19}, {11, 13, 15, 18}, {10, 12, 15, 17},
    {10, 12, 14, 16}, {9, 11, 13, 15}, {9, 11, 12, 14}, {8, 10, 12, 14},
    {8, 9, 11, 13}, {7, 9, 11, 12}, {7, 9, 10, 12}, {7, 8, 10, 11},
    {6, 8, 9, 11}, {6, 7, 9, 10}, {6, 7, 8, 9}, {2, 2, 2, 2},
}};
constexpr std::array<std::uint8_t, 64> kNextStateLps{
    0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9, 11, 11, 12,
    13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
    33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63,
};

// One context variable (9.3.2.2): its state and its more probable value.
struct Context {
    std::uint8_t state = 0;
    std::uint8_t mps = 0;
};

// A context from its initValue at the slice's QP (9.3.2.2, equations 9-6).
Context initial(unsigned init_value) {
    const int slope = static_cast<int>(init_value >> 4) * 5 - 45;
    const int offset = (static_cast<int>(init_value & 15) << 3) - 16;
    const int pre = std::clamp(((slope * kSliceQp) >> 4) + offset, 1, 126);
    return pre <= 63 ? Context{static_cast<std::uint8_t>(63 - pre), 0} : Context{static_cast<std::uint8_t>(pre - 64), 1};
}

// The arithmetic encoder of 9.3.5 (the same as H.264's 9.3.4), writing into `out`.
class Cabac {
public:
    explicit Cabac(BitWriter& out) : out_(out) {}

    void decision(Context& context, unsigned bin) {
        const std::uint32_t lps = kRangeLps[context.state][(range_ >> 6) & 3];
        range_ -= lps;
        if (bin != context.mps) {
            low_ += range_;
            range_ = lps;
            if (context.state == 0) context.mps = static_cast<std::uint8_t>(1 - context.mps);
            context.state = kNextStateLps[context.state];
        } else if (context.state < 62) {
            ++context.state;
        }
        renormalize();
    }
    // end_of_slice_segment_flag and pcm_flag. A one ends the arithmetic code: what follows is
    // written as it is, from the next byte on (the last bit written is a one).
    void terminate(bool bin) {
        range_ -= 2;
        if (!bin) {
            renormalize();
            return;
        }
        low_ += range_;
        range_ = 2;
        renormalize();
        put(low_ >> 9 & 1);
        out_.bits(((low_ >> 7) & 3) | 1, 2);
        out_.align();
    }
    // After samples written as they are, the coder begins again (9.3.2.5); the contexts stay.
    void restart() {
        low_ = 0;
        range_ = 510;
        outstanding_ = 0;
        first_ = true;
    }

private:
    void put(std::uint32_t bit) {
        if (first_) first_ = false;
        else out_.bits(bit, 1);
        for (; outstanding_ > 0; --outstanding_) out_.bits(1 - bit, 1);
    }
    void renormalize() {
        while (range_ < 256) {
            if (low_ < 256) {
                put(0);
            } else if (low_ >= 512) {
                low_ -= 512;
                put(1);
            } else {
                low_ -= 256;
                ++outstanding_;
            }
            range_ <<= 1;
            low_ <<= 1;
        }
    }

    BitWriter& out_;
    std::uint32_t low_ = 0, range_ = 510;
    unsigned outstanding_ = 0;
    bool first_ = true;
};

// profile_tier_level(1, 0) (7.3.3): Main profile, the main tier, progressive frames.
void profile(BitWriter& out) {
    out.bits(0, 2);               // general_profile_space
    out.bits(0, 1);               // general_tier_flag
    out.bits(1, 5);               // general_profile_idc: Main
    out.bits(0x60000000U, 32);    // general_profile_compatibility_flag: Main and Main 10
    out.bits(1, 1);               // general_progressive_source_flag
    out.bits(0, 1);               // general_interlaced_source_flag
    out.bits(0, 1);               // general_non_packed_constraint_flag
    out.bits(1, 1);               // general_frame_only_constraint_flag
    out.bits(0, 32);              // general_reserved_zero_43bits...
    out.bits(0, 11);
    out.bits(0, 1);               // general_inbld_flag
    out.bits(kLevel, 8);          // general_level_idc
}

// A PCM coding unit's samples (7.3.8.7), each in eight bits.
void pcm_samples(BitWriter& out, const ClipBlock& block) {
    for (unsigned i = 0; i < 256; ++i) out.byte(block.y);
    for (unsigned i = 0; i < 64; ++i) out.byte(block.cb);
    for (unsigned i = 0; i < 64; ++i) out.byte(block.cr);
}

}  // namespace

HevcClipWriter::HevcClipWriter(unsigned width, unsigned height) : ClipEncoder(width, height) {}

std::vector<std::uint8_t> HevcClipWriter::parameter_sets() const {
    const auto held = static_cast<std::uint32_t>(kNamed);  // max_dec_pic_buffering_minus1: the frames named and the frame itself

    // video_parameter_set_rbsp (7.3.2.1).
    BitWriter video;
    video.bits(0, 4);        // vps_video_parameter_set_id
    video.bits(1, 1);        // vps_base_layer_internal_flag
    video.bits(1, 1);        // vps_base_layer_available_flag
    video.bits(0, 6);        // vps_max_layers_minus1
    video.bits(0, 3);        // vps_max_sub_layers_minus1
    video.bits(1, 1);        // vps_temporal_id_nesting_flag
    video.bits(0xffff, 16);  // vps_reserved_0xffff_16bits
    profile(video);
    video.bits(1, 1);        // vps_sub_layer_ordering_info_present_flag
    video.ue(held);          // vps_max_dec_pic_buffering_minus1
    video.ue(0);             // vps_max_num_reorder_pics
    video.ue(0);             // vps_max_latency_increase_plus1
    video.bits(0, 6);        // vps_max_layer_id
    video.ue(0);             // vps_num_layer_sets_minus1
    video.bits(0, 1);        // vps_timing_info_present_flag
    video.bits(0, 1);        // vps_extension_flag
    video.finish();

    // seq_parameter_set_rbsp (7.3.2.2).
    BitWriter sequence;
    sequence.bits(0, 4);     // sps_video_parameter_set_id
    sequence.bits(0, 3);     // sps_max_sub_layers_minus1
    sequence.bits(1, 1);     // sps_temporal_id_nesting_flag
    profile(sequence);
    sequence.ue(0);          // sps_seq_parameter_set_id
    sequence.ue(1);          // chroma_format_idc: 4:2:0
    sequence.ue(columns_ << kLog2Block);  // pic_width_in_luma_samples: whole blocks
    sequence.ue(rows_ << kLog2Block);     // pic_height_in_luma_samples
    // conformance_window_flag and its offsets, in pairs of samples: the picture's own size.
    const unsigned crop_right = ((columns_ << kLog2Block) - width_) / 2, crop_bottom = ((rows_ << kLog2Block) - height_) / 2;
    const bool cropped = crop_right != 0 || crop_bottom != 0;
    sequence.bits(cropped ? 1 : 0, 1);
    if (cropped) {
        sequence.ue(0);
        sequence.ue(crop_right);
        sequence.ue(0);
        sequence.ue(crop_bottom);
    }
    sequence.ue(0);          // bit_depth_luma_minus8
    sequence.ue(0);          // bit_depth_chroma_minus8
    sequence.ue(kOrderBits - 4);  // log2_max_pic_order_cnt_lsb_minus4
    sequence.bits(1, 1);     // sps_sub_layer_ordering_info_present_flag
    sequence.ue(held);       // sps_max_dec_pic_buffering_minus1
    sequence.ue(0);          // sps_max_num_reorder_pics: shown in the order decoded
    sequence.ue(0);          // sps_max_latency_increase_plus1
    sequence.ue(kLog2Block - 3);  // log2_min_luma_coding_block_size_minus3: 16
    sequence.ue(0);          // log2_diff_max_min_luma_coding_block_size: coding tree blocks of 16, never split
    sequence.ue(0);          // log2_min_luma_transform_block_size_minus2: 4
    sequence.ue(2);          // log2_diff_max_min_luma_transform_block_size: 16
    sequence.ue(0);          // max_transform_hierarchy_depth_inter
    sequence.ue(0);          // max_transform_hierarchy_depth_intra
    sequence.bits(0, 1);     // scaling_list_enabled_flag
    sequence.bits(0, 1);     // amp_enabled_flag
    sequence.bits(0, 1);     // sample_adaptive_offset_enabled_flag: the samples stay as written
    sequence.bits(1, 1);     // pcm_enabled_flag
    sequence.bits(7, 4);     // pcm_sample_bit_depth_luma_minus1
    sequence.bits(7, 4);     // pcm_sample_bit_depth_chroma_minus1
    sequence.ue(kLog2Block - 3);  // log2_min_pcm_luma_coding_block_size_minus3: 16
    sequence.ue(0);          // log2_diff_max_min_pcm_luma_coding_block_size
    sequence.bits(1, 1);     // pcm_loop_filter_disabled_flag
    sequence.ue(0);          // num_short_term_ref_pic_sets: every frame carries its own, as a PS5's
    sequence.bits(0, 1);     // long_term_ref_pics_present_flag
    sequence.bits(0, 1);     // sps_temporal_mvp_enabled_flag
    sequence.bits(0, 1);     // strong_intra_smoothing_enabled_flag
    sequence.bits(1, 1);     // vui_parameters_present_flag (E.2.1)
    sequence.bits(0, 1);     // aspect_ratio_info_present_flag
    sequence.bits(0, 1);     // overscan_info_present_flag
    sequence.bits(1, 1);     // video_signal_type_present_flag
    sequence.bits(5, 3);     // video_format: unspecified
    sequence.bits(0, 1);     // video_full_range_flag: the limited range, as a console sends
    sequence.bits(1, 1);     // colour_description_present_flag
    sequence.bits(1, 8);     // colour_primaries: BT.709
    sequence.bits(1, 8);     // transfer_characteristics: BT.709
    sequence.bits(1, 8);     // matrix_coeffs: BT.709
    sequence.bits(0, 1);     // chroma_loc_info_present_flag
    sequence.bits(0, 1);     // neutral_chroma_indication_flag
    sequence.bits(0, 1);     // field_seq_flag
    sequence.bits(0, 1);     // frame_field_info_present_flag
    sequence.bits(0, 1);     // default_display_window_flag
    sequence.bits(0, 1);     // vui_timing_info_present_flag
    sequence.bits(0, 1);     // bitstream_restriction_flag
    sequence.bits(0, 1);     // sps_extension_present_flag
    sequence.finish();

    // pic_parameter_set_rbsp (7.3.2.3).
    BitWriter picture;
    picture.ue(0);           // pps_pic_parameter_set_id
    picture.ue(0);           // pps_seq_parameter_set_id
    picture.bits(0, 1);      // dependent_slice_segments_enabled_flag
    picture.bits(0, 1);      // output_flag_present_flag
    picture.bits(0, 3);      // num_extra_slice_header_bits
    picture.bits(0, 1);      // sign_data_hiding_enabled_flag
    picture.bits(0, 1);      // cabac_init_present_flag
    picture.ue(0);           // num_ref_idx_l0_default_active_minus1: one frame used
    picture.ue(0);           // num_ref_idx_l1_default_active_minus1
    picture.se(0);           // init_qp_minus26
    picture.bits(0, 1);      // constrained_intra_pred_flag
    picture.bits(0, 1);      // transform_skip_enabled_flag
    picture.bits(0, 1);      // cu_qp_delta_enabled_flag
    picture.se(0);           // pps_cb_qp_offset
    picture.se(0);           // pps_cr_qp_offset
    picture.bits(0, 1);      // pps_slice_chroma_qp_offsets_present_flag
    picture.bits(0, 1);      // weighted_pred_flag
    picture.bits(0, 1);      // weighted_bipred_flag
    picture.bits(0, 1);      // transquant_bypass_enabled_flag
    picture.bits(0, 1);      // tiles_enabled_flag
    picture.bits(0, 1);      // entropy_coding_sync_enabled_flag
    picture.bits(0, 1);      // pps_loop_filter_across_slices_enabled_flag
    picture.bits(1, 1);      // deblocking_filter_control_present_flag
    picture.bits(0, 1);      // deblocking_filter_override_enabled_flag
    picture.bits(1, 1);      // pps_deblocking_filter_disabled_flag: no filter, the samples stay as written
    picture.bits(0, 1);      // pps_scaling_list_data_present_flag
    picture.bits(0, 1);      // lists_modification_present_flag
    picture.ue(0);           // log2_parallel_merge_level_minus2
    picture.bits(0, 1);      // slice_segment_header_extension_present_flag
    picture.bits(0, 1);      // pps_extension_present_flag
    picture.finish();

    std::vector<std::uint8_t> out;
    append_unit(out, {header0(kVideoSet), kHeader1}, video.bytes());
    append_unit(out, {header0(kSequenceSet), kHeader1}, sequence.bytes());
    append_unit(out, {header0(kPictureSet), kHeader1}, picture.bytes());
    return out;
}

std::vector<std::uint8_t> HevcClipWriter::frame(std::uint32_t counter, bool keyframe) {
    // The frame predicted from: the newest one the decoder holds as it should be.
    const std::size_t back = keyframe ? 0 : reference_back();
    keyframe = back == 0;
    if (keyframe) order_ = 0;
    const std::size_t named = keyframe ? 0 : kept();
    const std::vector<ClipBlock> wanted = blocks(counter);
    const std::vector<ClipBlock>* from = keyframe ? nullptr : &shown(back);

    // slice_segment_header (7.3.6.1) of one slice that is the whole picture.
    BitWriter slice;
    slice.bits(1, 1);                // first_slice_segment_in_pic_flag
    if (keyframe) slice.bits(0, 1);  // no_output_of_prior_pics_flag
    slice.ue(0);                     // slice_pic_parameter_set_id
    slice.ue(keyframe ? 2 : 1);      // slice_type: I, or P
    if (!keyframe) {
        slice.bits(order_ % (1U << kOrderBits), kOrderBits);  // slice_pic_order_cnt_lsb
        slice.bits(0, 1);            // short_term_ref_pic_set_sps_flag
        // st_ref_pic_set (7.3.7): every frame kept, one further back each, one of them used.
        slice.ue(static_cast<std::uint32_t>(named));  // num_negative_pics
        slice.ue(0);                 // num_positive_pics
        for (std::size_t i = 1; i <= named; ++i) {
            slice.ue(0);             // delta_poc_s0_minus1
            slice.bits(i == back ? 1 : 0, 1);  // used_by_curr_pic_s0_flag
        }
        slice.bits(0, 1);            // num_ref_idx_active_override_flag
        slice.ue(4);                 // five_minus_max_num_merge_cand: one, so a skipped block names none
    }
    slice.se(0);                     // slice_qp_delta
    slice.bits(1, 1);                // byte_alignment(): alignment_bit_equal_to_one...
    slice.align();                   // ...then zeros

    // slice_segment_data (7.3.8): a coding tree unit per block, each one coding unit.
    Context part_mode = initial(keyframe ? 184 : 154);
    std::array<Context, 3> skip_flag{initial(197), initial(185), initial(201)};
    Context pred_mode = initial(149);
    std::vector<bool> skipped(wanted.size(), false);
    Cabac cabac(slice);
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const auto column = static_cast<unsigned>(i % columns_), row = static_cast<unsigned>(i / columns_);
        bool written = true;
        if (!keyframe) {
            const unsigned increment = (column > 0 && skipped[i - 1] ? 1U : 0U) + (row > 0 && skipped[i - columns_] ? 1U : 0U);
            skipped[i] = wanted[i] == (*from)[i];
            cabac.decision(skip_flag[increment], skipped[i] ? 1 : 0);  // cu_skip_flag
            written = !skipped[i];
            if (written) cabac.decision(pred_mode, 1);                 // pred_mode_flag: intra
        }
        if (written) {
            cabac.decision(part_mode, 1);  // part_mode: PART_2Nx2N
            cabac.terminate(true);         // pcm_flag
            pcm_samples(slice, wanted[i]);
            cabac.restart();
        }
        cabac.terminate(i + 1 == wanted.size());  // end_of_slice_segment_flag
    }
    // The last bit the coder wrote is the stop bit of rbsp_slice_segment_trailing_bits.

    keep(counter, wanted, back);
    ++order_;

    std::vector<std::uint8_t> out;
    append_unit(out, {header0(keyframe ? kKeyframe : kTrailing), kHeader1}, slice.bytes());
    return out;
}

}  // namespace psvp::media
