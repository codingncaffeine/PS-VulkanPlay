#include "media/video_decoder.h"

#include "media/frame_refs.h"
#include "media/h264_refs.h"
#include "media/gpu_device.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
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

// FFmpeg's own messages are not printed: the program counts what they are about. The last
// error is kept, for the reason a graphics card's decoder did not start.
std::mutex last_error_mutex;
std::string last_error;
void keep_errors(void* source, int level, const char* format, va_list arguments) {
    if (level > AV_LOG_ERROR) return;
    char line[512] = {};
    int prefix = 0;
    av_log_format_line2(source, level, format, arguments, line, sizeof line, &prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    const std::lock_guard lock(last_error_mutex);
    last_error = std::move(text);
}
std::string take_last_error() {
    const std::lock_guard lock(last_error_mutex);
    return std::exchange(last_error, std::string());
}

// The graphics card's format when FFmpeg offers it for this stream, and otherwise the first one
// that is not a graphics card's: the stream is then decoded in software.
AVPixelFormat pick_gpu_format(AVCodecContext*, const AVPixelFormat* offered) {
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VULKAN) return *format;
    }
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(*format);
        if (layout != nullptr && (layout->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *format;
    }
    return AV_PIX_FMT_NONE;
}

// Pictures of a graphics card compared with the software decoder's before its own are shown: two
// seconds of a stream. Three were seen to be too few: a card's H.264 pictures that were the same at
// first went apart from the 31st on (Vulkan Video on an NVIDIA driver, PLAN D34).
constexpr int kGpuChecks = 120;
// Frames in a row the graphics card gives no picture for before the stream goes on in software.
constexpr unsigned kGpuRefusalsBeforeSoftware = 30;

// The first NAL unit's first byte, behind a start code of three or four bytes.
std::optional<std::uint8_t> first_unit_byte(std::span<const std::uint8_t> bytes) noexcept {
    std::size_t at = 0;
    while (at < bytes.size() && bytes[at] == 0) ++at;
    if (at < 2 || at + 1 >= bytes.size() || bytes[at] != 1) return std::nullopt;
    return bytes[at + 1];
}

}  // namespace

const char* codec_name(VideoCodec codec) noexcept { return codec == VideoCodec::h264 ? "H.264" : "HEVC"; }

std::string ffmpeg_version() {
    // A release's own build says "n9.1"; the number is the version.
    std::string text = av_version_info();
    if (text.size() > 1 && text[0] == 'n' && text[1] >= '0' && text[1] <= '9') text.erase(0, 1);
    return text;
}

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
    // Decoding on a graphics card: `context` decodes there; `check` decodes the same frames in
    // software until the card's first pictures were found the same as its own.
    bool on_gpu = false;
    std::string gpu_name;
    std::unique_ptr<AVCodecContext, ContextDeleter> check;
    int checks_left = 0;
    unsigned gpu_refusals = 0;  // in a row
    std::string software_reason;
    bool keyframe_wanted = false;
    // Which frames the decoder holds, for the frames behind a lost one: HEVC's are changed to
    // use those, H.264's only judged (the decoder fills a lost frame's place itself).
    std::optional<ReferenceTracker> tracker;
    std::optional<H264Tracker> h264_tracker;
    std::string unfollowed;
};

VideoDecoder::VideoDecoder(std::unique_ptr<State> state) : state_(std::move(state)) {}
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;
VideoDecoder::~VideoDecoder() = default;

namespace {

// A decoder of the codec, set up for a stream: a picture for every frame, at once.
std::expected<std::unique_ptr<AVCodecContext, ContextDeleter>, std::string> open_context(const AVCodec* implementation, AVBufferRef* gpu) {
    std::unique_ptr<AVCodecContext, ContextDeleter> context(avcodec_alloc_context3(implementation));
    if (!context) return std::unexpected(std::string("out of memory"));
    // No frame is held back to wait for later ones, and one thread decodes, since decoding
    // several frames at a time delays each by the others.
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_count = 1;
    // After a lost frame the console sends frames that need none of the lost ones, but each
    // still names the frames before it. A decoder that drops every frame naming one it does
    // not have would never show a picture again (docs/protocol/video.md).
    context->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    if (gpu != nullptr) {
        context->hw_device_ctx = av_buffer_ref(gpu);
        if (context->hw_device_ctx == nullptr) return std::unexpected(std::string("out of memory"));
        context->get_format = pick_gpu_format;
        // A console's stream may name a level above what a card lists; the check of its first
        // pictures says whether it decodes them all the same.
        context->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        // The window holds pictures while they are shown: beyond what the decoder itself keeps.
        context->extra_hw_frames = 4;
    }
    if (const int result = avcodec_open2(context.get(), implementation, nullptr); result < 0) {
        return std::unexpected(std::format("FFmpeg's {} decoder did not open: {}", implementation->name, error_text(result)));
    }
    return context;
}

}  // namespace

std::expected<VideoDecoder, std::string> VideoDecoder::open(VideoCodec codec, std::span<const std::uint8_t> header, const GpuDevice* gpu, int gpu_checks) {
    av_log_set_level(AV_LOG_QUIET);
    av_log_set_callback(keep_errors);

    auto state = std::make_unique<State>();
    state->codec = codec;
    state->implementation = avcodec_find_decoder(codec == VideoCodec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    if (state->implementation == nullptr) {
        return std::unexpected(std::format("this FFmpeg has no {} decoder", codec_name(codec)));
    }
    state->packet.reset(av_packet_alloc());
    if (!state->packet) return std::unexpected(std::string("out of memory"));

    if (gpu != nullptr && gpu->context() != nullptr) {
        auto on_gpu = open_context(state->implementation, static_cast<AVBufferRef*>(gpu->context()));
        if (on_gpu) {
            state->context = std::move(*on_gpu);
            state->on_gpu = true;
            state->gpu_name = gpu->description();
            state->checks_left = gpu_checks > 0 ? gpu_checks : kGpuChecks;
            auto check = open_context(state->implementation, nullptr);
            if (!check) return std::unexpected(check.error());
            state->check = std::move(*check);
        } else {
            state->software_reason = std::format("the graphics card's decoder did not open: {}", on_gpu.error());
        }
    }
    if (!state->context) {
        auto context = open_context(state->implementation, nullptr);
        if (!context) return std::unexpected(context.error());
        state->context = std::move(*context);
    }
    state->header.assign(header.begin(), header.end());
    if (codec == VideoCodec::hevc) {
        state->tracker.emplace(header);
        state->unfollowed = state->tracker->unread_reason();
    } else {
        state->h264_tracker.emplace(header);
        state->unfollowed = state->h264_tracker->unread_reason();
    }
    return VideoDecoder(std::move(state));
}

void VideoDecoder::restart() {
    avcodec_flush_buffers(state_->context.get());
    if (state_->check) avcodec_flush_buffers(state_->check.get());
    state_->header_due = true;
    if (state_->tracker) state_->tracker.emplace(state_->header);
    if (state_->h264_tracker) state_->h264_tracker.emplace(state_->header);
}

VideoDecoder::Decoded VideoDecoder::decode(std::uint16_t index, std::span<const std::uint8_t> frame) {
    State& state = *state_;
    Decoded result;
    if (!state.tracker && !state.h264_tracker) {
        result.picture = decode(frame);
        result.needs_keyframe = std::exchange(state.keyframe_wanted, false);
        return result;
    }
    using Way = ReferenceTracker::Way;
    const ReferenceTracker::Verdict verdict = state.tracker ? state.tracker->look(index, frame) : state.h264_tracker->look(index, frame);
    state.unfollowed = verdict.way == Way::unread ? verdict.why : std::string();
    if (verdict.way == Way::no_reference) {
        ++state.counters.frames;
        ++state.counters.undecodable;
        result.needs_keyframe = true;
        return result;
    }
    if (verdict.way == Way::changed) ++state.counters.changed;
    // An H.264 verdict carries no bytes: that frame is decoded as sent.
    result.picture = decode(verdict.way == Way::changed && !verdict.changed.empty() ? std::span<const std::uint8_t>(verdict.changed) : frame);
    if (std::exchange(state.keyframe_wanted, false)) {
        // The decoder started again in software: nothing before the next keyframe can be decoded.
        result.needs_keyframe = true;
        return result;
    }
    if (state.tracker) state.tracker->decoded(index, result.picture.has_value());
    else state.h264_tracker->decoded(index, result.picture.has_value());
    if (result.picture && verdict.way != Way::unread) {
        // The frame was decoded from frames the decoder holds only (an HEVC frame changed so; for
        // H.264 the decoder put the frame it got last in a lost frame's place): what the decoder
        // thinks of the stream's own list, which still names the lost frame, does not count.
        if (result.picture->damaged) --state.counters.damaged;
        result.picture->damaged = false;
        result.picture->approximate = !verdict.exact;
        if (!verdict.exact) ++state.counters.approximate;
    }
    return result;
}

const std::string& VideoDecoder::unfollowed_reason() const noexcept { return state_->unfollowed; }

namespace {

// What a decoded frame is to the rest of the program; nothing for a layout it does not take.
std::optional<Picture> picture_of(std::unique_ptr<AVFrame, FrameDeleter> decoded) {
    Picture picture;
    if (decoded->format == AV_PIX_FMT_VULKAN) {
        // On the graphics card: its depth is that of the frames it decodes into.
        if (decoded->hw_frames_ctx == nullptr) return std::nullopt;
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(decoded->hw_frames_ctx->data);
        const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(frames->sw_format);
        if (layout == nullptr || layout->nb_components != 3 || layout->log2_chroma_w != 1 || layout->log2_chroma_h != 1) return std::nullopt;
        picture.on_gpu = true;
        picture.bits = static_cast<unsigned>(layout->comp[0].depth);
    } else {
        const AVPixFmtDescriptor* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));
        const bool planar_420 = layout != nullptr && layout->nb_components == 3 && layout->log2_chroma_w == 1 && layout->log2_chroma_h == 1 &&
                                (layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BE)) == 0 &&
                                (layout->flags & AV_PIX_FMT_FLAG_PLANAR) != 0 && layout->comp[1].plane == 1 && layout->comp[2].plane == 2;
        if (!planar_420) return std::nullopt;
        picture.bits = static_cast<unsigned>(layout->comp[0].depth);
        for (int plane = 0; plane < 3; ++plane) {
            picture.planes[plane] = decoded->data[plane];
            picture.strides[plane] = static_cast<std::size_t>(decoded->linesize[plane]);
        }
    }
    if (decoded->width <= 0 || decoded->height <= 0) return std::nullopt;
    picture.width = static_cast<unsigned>(decoded->width);
    picture.height = static_cast<unsigned>(decoded->height);
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
    picture.keep = std::shared_ptr<AVFrame>(decoded.release(), FrameDeleter{});
    return picture;
}

std::unique_ptr<AVFrame, FrameDeleter> receive(AVCodecContext* context, bool sent) {
    std::unique_ptr<AVFrame, FrameDeleter> decoded(av_frame_alloc());
    if (!sent || !decoded || avcodec_receive_frame(context, decoded.get()) < 0) return nullptr;
    return decoded;
}

}  // namespace

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
    const bool sent = avcodec_send_packet(state.context.get(), &packet) >= 0;
    const bool check_sent = state.check && avcodec_send_packet(state.check.get(), &packet) >= 0;
    av_packet_unref(&packet);
    if (sent || check_sent) state.header_due = false;

    std::unique_ptr<AVFrame, FrameDeleter> decoded = receive(state.context.get(), sent);
    std::unique_ptr<AVFrame, FrameDeleter> checked = state.check ? receive(state.check.get(), check_sent) : nullptr;

    // From now on in software, with the decoder that checked the graphics card: it was given every frame.
    const auto to_software = [&](std::string why) {
        state.software_reason = std::move(why);
        state.context = std::move(state.check);
        state.on_gpu = false;
        state.checks_left = 0;
        decoded = std::move(checked);
    };
    if (state.check) {
        if (decoded && decoded->format != AV_PIX_FMT_VULKAN) {
            const std::string error = take_last_error();
            to_software(error.empty() ? std::string("the graphics card cannot decode this stream") : std::format("the graphics card cannot decode this stream: {}", error));
        } else if (!decoded && checked) {
            const std::string error = take_last_error();
            to_software(error.empty() ? std::string("the graphics card gave no picture for a frame the software decoder decoded")
                                      : std::format("the graphics card gave no picture for a frame the software decoder decoded: {}", error));
        } else if (decoded && checked) {
            // Both decoded the frame: the card's picture, read back, must be the same as the software decoder's.
            std::optional<Picture> on_card = picture_of(std::move(decoded));
            std::optional<Picture> in_software = picture_of(std::move(checked));
            const std::optional<Picture> read_back = on_card ? download(*on_card) : std::nullopt;
            const std::optional<PictureDifference> apart = read_back && in_software ? compare_pictures(*read_back, *in_software) : std::nullopt;
            ++state.counters.checked;
            const auto software_frame = [&] {
                // The software decoder's own frame again, for the picture shown now.
                return in_software ? std::unique_ptr<AVFrame, FrameDeleter>(av_frame_clone(static_cast<AVFrame*>(in_software->keep.get()))) : nullptr;
            };
            checked = software_frame();
            if (!read_back) {
                to_software("the graphics card's picture could not be read back to be compared");
            } else if (!apart) {
                to_software(std::format("the graphics card's picture is not the software decoder's size or depth ({}x{}, {} bits)", read_back->width,
                                        read_back->height, read_back->bits));
            } else if (apart->different != 0) {
                to_software(std::format("the graphics card's picture was not the software decoder's: {} of {} samples differed, by up to {}",
                                        apart->different, apart->samples, apart->largest));
            } else {
                // Shown from the software decoder until the card has passed every check.
                decoded = std::move(checked);
                if (--state.checks_left == 0) state.check.reset();
            }
        }
        // Neither decoded the frame: nothing to compare.
    } else if (state.on_gpu) {
        if (decoded) {
            state.gpu_refusals = 0;
        } else if (++state.gpu_refusals >= kGpuRefusalsBeforeSoftware) {
            // The card stopped decoding: in software from the next keyframe on.
            const std::string error = take_last_error();
            auto software = open_context(state.implementation, nullptr);
            if (software) {
                state.context = std::move(*software);
                state.on_gpu = false;
                state.header_due = true;
                if (state.tracker) state.tracker.emplace(state.header);
                if (state.h264_tracker) state.h264_tracker.emplace(state.header);
                state.keyframe_wanted = true;
                state.software_reason = std::format("the graphics card stopped giving pictures after {} frames{}", state.counters.frames,
                                                    error.empty() ? std::string() : ": " + error);
            }
        }
    }
    spent();
    if (!decoded) {
        ++state.counters.refused;
        return std::nullopt;
    }
    std::optional<Picture> picture = picture_of(std::move(decoded));
    if (!picture) {
        ++state.counters.refused;
        return std::nullopt;
    }
    ++state.counters.pictures;
    if (picture->on_gpu) ++state.counters.gpu_pictures;
    if (picture->damaged) ++state.counters.damaged;
    return picture;
}

const VideoDecoderCounters& VideoDecoder::counters() const noexcept { return state_->counters; }

bool VideoDecoder::on_gpu() const noexcept { return state_->on_gpu; }
const std::string& VideoDecoder::software_reason() const noexcept { return state_->software_reason; }
int VideoDecoder::checks_left() const noexcept { return state_->checks_left; }

std::string VideoDecoder::description() const {
    const State& state = *state_;
    if (state.on_gpu) {
        return std::format("{}, FFmpeg's {} decoder on {} through Vulkan Video{}", codec_name(state.codec), state.implementation->name, state.gpu_name,
                           state.checks_left > 0 ? ", its first pictures checked against software" : "");
    }
    return std::format("{}, FFmpeg's {} decoder in software{}", codec_name(state.codec), state.implementation->name,
                       state.software_reason.empty() ? std::string() : std::format(" ({})", state.software_reason));
}

}  // namespace psvp::media
