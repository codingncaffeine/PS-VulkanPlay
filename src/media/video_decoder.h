// Turns the picture frames of a stream into pictures, with FFmpeg's decoders.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "media/test_clip.h"

namespace psvp::media {

enum class VideoCodec { h264, hevc };
[[nodiscard]] const char* codec_name(VideoCodec codec) noexcept;
// FFmpeg's own version, as it says it ("9.1").
[[nodiscard]] std::string ffmpeg_version();
// The codec of a stream, told from the header the console sends before the first
// frame (the parameter sets its decoder needs); nothing when it is neither.
[[nodiscard]] std::optional<VideoCodec> codec_of_header(std::span<const std::uint8_t> header) noexcept;

// How a picture's numbers are meant, as its stream says.
enum class ColorMatrix { bt709, bt601, bt2020 };
enum class ColorTransfer { sdr, pq, hlg };

// One decoded picture: three planes, the two color planes at half the width and height.
// Samples are bytes when `bits` is 8 and 16-bit numbers in the machine's byte order otherwise.
struct Picture {
    unsigned width = 0, height = 0;
    unsigned bits = 8;
    bool full_range = false;
    ColorMatrix matrix = ColorMatrix::bt709;
    ColorTransfer transfer = ColorTransfer::sdr;
    bool keyframe = false;
    // The decoder did not have everything this picture is built from: it may show damage.
    bool damaged = false;
    // Built on a frame that stood in for a lost one, or on a picture that was: close to the
    // console's picture, not the same. The console's repair ends it.
    bool approximate = false;
    // Decoded on a graphics card and still in its memory (media/gpu_device.h): no planes here,
    // `keep` holds FFmpeg's frame. `bits` is still the depth of its samples.
    bool on_gpu = false;
    const std::uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    std::size_t strides[3] = {0, 0, 0};  // in bytes
    std::shared_ptr<void> keep;          // the planes live as long as this does

    // For a picture of 8-bit samples.
    [[nodiscard]] PlanesView view() const noexcept {
        return PlanesView{planes[0], planes[1], planes[2], strides[0], strides[1], strides[2], width, height};
    }
};

struct VideoDecoderCounters {
    std::uint64_t frames = 0;    // handed in
    std::uint64_t pictures = 0;  // came out
    std::uint64_t refused = 0;   // frames the decoder gave no picture for
    std::uint64_t damaged = 0;   // pictures that came out marked as damaged
    std::uint64_t changed = 0;       // frames made to use a frame the decoder held, in place of a lost one
    std::uint64_t approximate = 0;   // pictures that came out of those, and of frames built on them
    std::uint64_t undecodable = 0;   // frames left out: the decoder held none of the frames they name
    std::uint64_t microseconds = 0;      // spent decoding, all frames together
    std::uint64_t slowest_microseconds = 0;
    std::uint64_t gpu_pictures = 0;      // of `pictures`, those that came out on the graphics card
    std::uint64_t checked = 0;           // pictures of the graphics card compared with the software decoder's
};

class GpuDevice;

class VideoDecoder {
public:
    // `header` is what the console sent before the first frame. With a `gpu`, the pictures are
    // decoded on that graphics card where it can: its first pictures (two seconds) are decoded in
    // software too and must be the same, sample for sample (decoding is exact by the standards), or the
    // decoder goes on in software and says why. It also does when the card stops giving pictures.
    // `gpu_checks`: how many of the card's first pictures are compared (a short clip's check takes fewer).
    [[nodiscard]] static std::expected<VideoDecoder, std::string> open(VideoCodec codec, std::span<const std::uint8_t> header,
                                                                     const GpuDevice* gpu = nullptr, int gpu_checks = -1);
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;
    ~VideoDecoder();

    // One frame as the console sent it, in the stream's order. A frame is one picture, and the
    // decoder holds none back: the picture of this frame comes out, or nothing does.
    [[nodiscard]] std::optional<Picture> decode(std::span<const std::uint8_t> frame);
    // The same for a frame whose number in the stream is known, which lets the pictures go on
    // behind a lost frame (HEVC; media/frame_refs.h): a frame predicted from one the decoder does
    // not hold is decoded from the nearest one it holds, and its picture is marked approximate,
    // not damaged. Nothing comes out and `needs_keyframe` is set when it holds none at all.
    struct Decoded {
        std::optional<Picture> picture;
        bool needs_keyframe = false;
    };
    [[nodiscard]] Decoded decode(std::uint16_t index, std::span<const std::uint8_t> frame);
    // Why frames are decoded as they are sent, whatever they are predicted from; empty when the stream is followed.
    [[nodiscard]] const std::string& unfollowed_reason() const noexcept;
    // The frames that follow do not continue the ones before: the next one must be a keyframe.
    void restart();

    [[nodiscard]] const VideoDecoderCounters& counters() const noexcept;
    // Which decoder this is, for a report: "H.264, FFmpeg's h264 decoder in software".
    [[nodiscard]] std::string description() const;
    // Pictures come out on the graphics card (once its check has passed).
    [[nodiscard]] bool on_gpu() const noexcept;
    // Why a decoder that was given a graphics card decodes in software; empty otherwise.
    [[nodiscard]] const std::string& software_reason() const noexcept;
    // Pictures of the graphics card it compares with the software decoder's: how many are still to come.
    [[nodiscard]] int checks_left() const noexcept;

private:
    struct State;
    explicit VideoDecoder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace psvp::media
