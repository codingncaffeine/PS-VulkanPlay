#include "media/video_decoder.h"

#include "media/frame_refs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace psvp::media {

namespace {

struct ContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

std::string error_text(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

// The first NAL unit's first byte, behind a start code of three or four bytes.
std::optional<std::uint8_t> first_unit_byte(std::span<const std::uint8_t> bytes) noexcept {
    std::size_t at = 0;
    while (at < bytes.size() && bytes[at] == 0) ++at;
    if (at < 2 || at + 1 >= bytes.size() || bytes[at] != 1) return std::nullopt;
    return bytes[at + 1];
}

}  // namespace

const char* codec_name(VideoCodec codec) noexcept { return codec == VideoCodec::h264 ? "H.264" : "HEVC"; }

std::optional<VideoCodec> codec_of_header(std::span<const std::uint8_t> header) noexcept {
    const auto first = first_unit_byte(header);
    if (!first) return std::nullopt;
    // H.264 begins with a sequence parameter set (unit type 7 in the low five bits, the top bit
    // zero); HEVC with a video parameter set (type 32 in the six bits after the top one).
    if ((*first & 0x80) == 0 && (*first & 0x1f) == 7) return VideoCodec::h264;
    if ((*first & 0x80) == 0 && ((*first >> 1) & 0x3f) == 32) return VideoCodec::hevc;
    return std::nullopt;
}

struct VideoDecoder::State {
    VideoCodec codec = VideoCodec::h264;
    const AVCodec* implementation = nullptr;
    std::unique_ptr<AVCodecContext, ContextDeleter> context;
    std::unique_ptr<AVPacket, PacketDeleter> packet;
    std::vector<std::uint8_t> header;
    bool header_due = true;
    VideoDecoderCounters counters;
    // HEVC: which frames the decoder holds, for the frames behind a lost one.
    std::optional<ReferenceTracker> tracker;
    std::string unfollowed;
};

VideoDecoder::VideoDecoder(std::unique_ptr<State> state) : state_(std::move(state)) {}
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;
VideoDecoder::~VideoDecoder() = default;

std::expected<VideoDecoder, std::string> VideoDecoder::open(VideoCodec codec, std::span<const std::uint8_t> header) {
    // The decoders report what they miss in a stream on stderr; this program counts it instead.
    av_log_set_level(AV_LOG_QUIET);

    auto state = std::make_unique<State>();
    state->codec = codec;
    state->implementation = avcodec_find_decoder(codec == VideoCodec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    if (state->implementation == nullptr) {
        return std::unexpected(std::format("this FFmpeg has no {} decoder", codec_name(codec)));
    }
    state->context.reset(avcodec_alloc_context3(state->implementation));
    state->packet.reset(av_packet_alloc());
    if (!state->context || !state->packet) return std::unexpected(std::string("out of memory"));

    AVCodecContext& context = *state->context;
    // A picture for every frame, at once: no frame is held back to wait for later ones, and one
    // thread decodes, since decoding several frames at a time delays each by the others.
    context.flags |= AV_CODEC_FLAG_LOW_DELAY;
    context.thread_count = 1;
    // After a lost frame the console sends frames that need none of the lost ones, but each
    // still names the frames before it. A decoder that drops every frame naming one it does
    // not have would never show a picture again (docs/protocol/video.md).
    context.flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    if (const int result = avcodec_open2(&context, state->implementation, nullptr); result < 0) {
        return std::unexpected(std::format("FFmpeg's {} decoder did not open: {}", state->implementation->name, error_text(result)));
    }
    state->header.assign(header.begin(), header.end());
    if (codec == VideoCodec::hevc) {
        state->tracker.emplace(header);
        state->unfollowed = state->tracker->unread_reason();
    } else {
        state->unfollowed = "an H.264 stream's frames are not read";
    }
    return VideoDecoder(std::move(state));
}

void VideoDecoder::restart() {
    avcodec_flush_buffers(state_->context.get());
    state_->header_due = true;
    if (state_->tracker) state_->tracker.emplace(state_->header);
}

VideoDecoder::Decoded VideoDecoder::decode(std::uint16_t index, std::span<const std::uint8_t> frame) {
    State& state = *state_;
    Decoded result;
    if (!state.tracker) {
        result.picture = decode(frame);
        return result;
    }
    using Way = ReferenceTracker::Way;
    const ReferenceTracker::Verdict verdict = state.tracker->look(index, frame);
    state.unfollowed = verdict.way == Way::unread ? verdict.why : std::string();
    if (verdict.way == Way::no_reference) {
        ++state.counters.frames;
        ++state.counters.undecodable;
        result.needs_keyframe = true;
        return result;
    }
    if (verdict.way == Way::changed) ++state.counters.changed;
    result.picture = decode(verdict.way == Way::changed ? std::span<const std::uint8_t>(verdict.changed) : frame);
    state.tracker->decoded(index, result.picture.has_value());
    if (result.picture && verdict.way != Way::unread) {
        // The decoder was asked for frames it holds only: what it thinks of the stream's own
        // list, which still names the lost frame, does not count.
        if (result.picture->damaged) --state.counters.damaged;
        result.picture->damaged = false;
        result.picture->approximate = !verdict.exact;
        if (!verdict.exact) ++state.counters.approximate;
    }
    return result;
}

const std::string& VideoDecoder::unfollowed_reason() const noexcept { return state_->unfollowed; }

std::optional<Picture> VideoDecoder::decode(std::span<const std::uint8_t> frame) {
    State& state = *state_;
    ++state.counters.frames;
    const auto began = std::chrono::steady_clock::now();
    const auto spent = [&] {
        const auto took = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        state.counters.microseconds += took;
        state.counters.slowest_microseconds = std::max(state.counters.slowest_microseconds, took);
    };

    // The header goes in front of the first frame, in the same packet.
    const std::size_t header_size = state.header_due ? state.header.size() : 0;
    AVPacket& packet = *state.packet;
    if (av_new_packet(&packet, static_cast<int>(header_size + frame.size())) < 0) {
        ++state.counters.refused;
        return std::nullopt;
    }
    if (header_size != 0) std::memcpy(packet.data, state.header.data(), header_size);
    if (!frame.empty()) std::memcpy(packet.data + header_size, frame.data(), frame.size());
    const int sent = avcodec_send_packet(state.context.get(), &packet);
    av_packet_unref(&packet);
    if (sent >= 0) state.header_due = false;

    std::unique_ptr<AVFrame, FrameDeleter> decoded(av_frame_alloc());
    if (sent < 0 || !decoded || avcodec_receive_frame(state.context.get(), decoded.get()) < 0) {
        spent();
        ++state.counters.refused;
        return std::nullopt;
    }
    spent();

    const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));
    const bool planar_420 = layout != nullptr && layout->nb_components == 3 && layout->log2_chroma_w == 1 && layout->log2_chroma_h == 1 &&
                            (layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BE)) == 0 &&
                            (layout->flags & AV_PIX_FMT_FLAG_PLANAR) != 0 && decoded->width > 0 && decoded->height > 0;
    if (!planar_420) {
        ++state.counters.refused;
        return std::nullopt;
    }

    Picture picture;
    picture.width = static_cast<unsigned>(decoded->width);
    picture.height = static_cast<unsigned>(decoded->height);
    picture.bits = static_cast<unsigned>(layout->comp[0].depth);
    picture.full_range = decoded->color_range == AVCOL_RANGE_JPEG;
    switch (decoded->colorspace) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: picture.matrix = ColorMatrix::bt2020; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: picture.matrix = ColorMatrix::bt601; break;
    default: picture.matrix = ColorMatrix::bt709; break;  // also when the stream does not say
    }
    switch (decoded->color_trc) {
    case AVCOL_TRC_SMPTE2084: picture.transfer = ColorTransfer::pq; break;
    case AVCOL_TRC_ARIB_STD_B67: picture.transfer = ColorTransfer::hlg; break;
    default: picture.transfer = ColorTransfer::sdr; break;
    }
    picture.keyframe = (decoded->flags & AV_FRAME_FLAG_KEY) != 0;
    picture.damaged = (decoded->flags & AV_FRAME_FLAG_CORRUPT) != 0 || decoded->decode_error_flags != 0;
    for (int plane = 0; plane < 3; ++plane) {
        picture.planes[plane] = decoded->data[plane];
        picture.strides[plane] = static_cast<std::size_t>(decoded->linesize[plane]);
    }
    picture.keep = std::shared_ptr<AVFrame>(decoded.release(), FrameDeleter{});
    ++state.counters.pictures;
    if (picture.damaged) ++state.counters.damaged;
    return picture;
}

const VideoDecoderCounters& VideoDecoder::counters() const noexcept { return state_->counters; }

std::string VideoDecoder::description() const {
    return std::format("{}, FFmpeg's {} decoder in software", codec_name(state_->codec), state_->implementation->name);
}

}  // namespace psvp::media
