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
    std::uint64_t microseconds = 0;      // spent decoding, all frames together
    std::uint64_t slowest_microseconds = 0;
};

class VideoDecoder {
public:
    // `header` is what the console sent before the first frame.
    [[nodiscard]] static std::expected<VideoDecoder, std::string> open(VideoCodec codec, std::span<const std::uint8_t> header);
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;
    ~VideoDecoder();

    // One frame as the console sent it, in the stream's order. A frame is one picture, and the
    // decoder holds none back: the picture of this frame comes out, or nothing does.
    [[nodiscard]] std::optional<Picture> decode(std::span<const std::uint8_t> frame);
    // The frames that follow do not continue the ones before: the next one must be a keyframe.
    void restart();

    [[nodiscard]] const VideoDecoderCounters& counters() const noexcept;
    // Which decoder this is, for a report: "H.264, FFmpeg's h264 decoder in software".
    [[nodiscard]] std::string description() const;

private:
    struct State;
    explicit VideoDecoder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace psvp::media
