// A generated clip: the pictures the mock console sends and the decoder's
// start-up check decodes. Every frame shows its own number, so a reader of the
// decoded picture (or of a screen) can tell which frame it has and whether
// every sample of it is right. The clip is written as H.264 without any loss
// of information: what a decoder gives back is sample for sample what went in.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace psvp::media {

// One 16x16 block of the clip is one flat color.
struct ClipBlock {
    std::uint8_t y = 0, cb = 0, cr = 0;
    friend bool operator==(const ClipBlock&, const ClipBlock&) = default;
};

inline constexpr unsigned kClipBlock = 16;
inline constexpr unsigned kClipCounterBits = 16;
inline constexpr unsigned kClipMinWidth = kClipBlock * kClipCounterBits;
inline constexpr unsigned kClipMinHeight = kClipBlock * 4;

// What frame `counter` shows in the block at (column, row) of a clip of columns x rows blocks:
//   row 0      the counter's sixteen bits, the highest first: white for one, black for zero
//   row 1      the same bits the other way round, so that a reader can tell a real frame from a blank one
//   row 2      eight color patches that never change; its last block is the mark: white while
//              `mark` is set, gray otherwise (a console stand-in sets it to answer a button)
//   below      one white block on gray that moves on by one place with every frame
[[nodiscard]] ClipBlock clip_block(std::uint32_t counter, unsigned column, unsigned row, unsigned columns, unsigned rows, bool mark = false);
inline constexpr unsigned kClipMarkRow = 2;

// A decoded picture, or any picture, as three planes of 8-bit samples, the color planes at half size.
struct PlanesView {
    const std::uint8_t* y = nullptr;
    const std::uint8_t* cb = nullptr;
    const std::uint8_t* cr = nullptr;
    std::size_t y_stride = 0, cb_stride = 0, cr_stride = 0;
    unsigned width = 0, height = 0;
};

// The frame number a picture of the clip shows, read from the middle of the blocks of its
// first two rows; nothing when the two rows do not agree.
[[nodiscard]] std::optional<std::uint16_t> read_clip_counter(const PlanesView& picture);
// How many samples of a picture are not what the clip's frame `counter` shows.
[[nodiscard]] std::size_t clip_differences(const PlanesView& picture, std::uint32_t counter, bool mark = false);

// Writes the clip, one frame after another. The first frame and every frame asked
// for as a keyframe stand alone; the others carry only the blocks that changed
// since the frame the decoder is taken to have.
class ClipWriter {
public:
    // Width and height in samples, both even, at least kClipMinWidth x kClipMinHeight.
    ClipWriter(unsigned width, unsigned height);

    [[nodiscard]] unsigned width() const noexcept { return width_; }
    [[nodiscard]] unsigned height() const noexcept { return height_; }
    // What a decoder needs before the first frame: the two parameter sets, each behind a start code.
    [[nodiscard]] std::vector<std::uint8_t> parameter_sets() const;
    // The next frame, showing `counter`.
    [[nodiscard]] std::vector<std::uint8_t> frame(std::uint32_t counter, bool keyframe);
    // The decoder did not get the frames that showed `first` to `last`. It still shows, where
    // those frames changed something, what it had before them: the next frame carries every
    // block that is not what it should be by then. (What a console does after a loss report,
    // without a keyframe.) Frames older than the last kHistory ones cannot be named.
    void decoder_lost(std::uint32_t first, std::uint32_t last);
    static constexpr std::size_t kHistory = 64;
    // The frames written from now on show the mark (clip_block) or do not.
    void set_mark(bool mark) noexcept { mark_ = mark; }
    [[nodiscard]] bool mark() const noexcept { return mark_; }

private:
    [[nodiscard]] std::vector<ClipBlock> blocks(std::uint32_t counter) const;

    unsigned width_, height_, columns_, rows_;
    // A frame that was written: what the decoder showed before it, and the blocks it carried.
    struct Written {
        std::uint32_t counter = 0;
        std::vector<ClipBlock> before;
        std::vector<std::pair<std::uint32_t, ClipBlock>> carried;
    };
    std::optional<std::vector<ClipBlock>> decoder_;  // what the decoder shows after the last frame written
    std::deque<Written> history_;
    unsigned frame_number_ = 0;                       // of the next frame, counted from the last keyframe
    unsigned keyframes_ = 0;
    bool mark_ = false;
};

}  // namespace psvp::media
