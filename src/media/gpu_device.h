// A graphics card's video decoder, as FFmpeg reaches it through Vulkan: the device the
// pictures are decoded on, and the way back to the machine's memory for checks.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "media/video_decoder.h"

namespace psvp::media {

// One Vulkan device FFmpeg decodes on. Copies share it; the device itself lives as long as
// whoever made it keeps it (the stream window keeps its own; `create_own` makes one FFmpeg keeps).
class GpuDevice {
public:
    // FFmpeg's own device on the best graphics card, for decoding without a window (`decode`).
    [[nodiscard]] static std::expected<GpuDevice, std::string> create_own();
    // Whether this program was built with decoding on a graphics card at all, and why not.
    [[nodiscard]] static std::optional<std::string> unavailable_reason();

    // Every submission to a queue FFmpeg uses goes between these, from whichever thread.
    void lock_queue(std::uint32_t family, std::uint32_t index) const;
    void unlock_queue(std::uint32_t family, std::uint32_t index) const;
    // "NVIDIA GeForce RTX 5080 (H.264, HEVC)".
    [[nodiscard]] const std::string& description() const noexcept;
    // The FFmpeg device (an AVBufferRef), for the decoder.
    [[nodiscard]] void* context() const noexcept;
    // Holds on the device: copies of this, and FFmpeg's own (decoders, frames). 1: only this one.
    [[nodiscard]] long users() const noexcept;

    struct State;
    explicit GpuDevice(std::shared_ptr<State> state) : state_(std::move(state)) {}

private:
    std::shared_ptr<State> state_;
};

// A picture decoded on a graphics card, brought into the machine's memory in the layout of
// the software decoder's pictures (three planes, samples of more than 8 bits in the low bits).
// Takes a while (it waits for the card): for checks, not for every picture of a stream.
[[nodiscard]] std::optional<Picture> download(const Picture& picture);

// How two pictures in the machine's memory differ: the samples of every plane that are not the
// same, and the largest difference. Nothing when their sizes or depths differ.
struct PictureDifference {
    std::uint64_t samples = 0;     // compared
    std::uint64_t different = 0;
    unsigned largest = 0;
};
[[nodiscard]] std::optional<PictureDifference> compare_pictures(const Picture& a, const Picture& b);

}  // namespace psvp::media
