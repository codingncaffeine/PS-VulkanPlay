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

// Writes the clip in one codec, one frame after another, in the shape a PS5's streams
// were seen to have (docs/protocol/video.md): every frame keeps the frames before it, up
// to kKept, and is predicted from one of them: the frame just before it, or, after a loss
// report, the last frame before the loss. The first frame and every frame asked for as a
// keyframe stand alone; the others carry only the blocks that differ from the frame they
// are predicted from.
class ClipEncoder {
public:
    virtual ~ClipEncoder() = default;

    [[nodiscard]] unsigned width() const noexcept { return width_; }
    [[nodiscard]] unsigned height() const noexcept { return height_; }
    // What a decoder needs before the first frame: the parameter sets, each behind a start code.
    [[nodiscard]] virtual std::vector<std::uint8_t> parameter_sets() const = 0;
    // The next frame, showing `counter`.
    [[nodiscard]] virtual std::vector<std::uint8_t> frame(std::uint32_t counter, bool keyframe) = 0;
    // The decoder did not get the frames that showed `first` to `last` (counted in sixteen bits,
    // as a stream counts its frames). The next frame is predicted from the newest frame kept
    // that is neither lost nor built on a lost one, so the picture is right again without a
    // keyframe (what a console does after a loss report); when none is kept, it is a keyframe.
    void decoder_lost(std::uint32_t first, std::uint32_t last);
    // The frames written from now on show the mark (clip_block) or do not.
    void set_mark(bool mark) noexcept { mark_ = mark; }
    [[nodiscard]] bool mark() const noexcept { return mark_; }

    static constexpr std::size_t kKept = 9;  // as a PS5 keeps, in H.264 and in HEVC

protected:
    // Width and height in samples, both even, at least kClipMinWidth x kClipMinHeight.
    ClipEncoder(unsigned width, unsigned height);
    // Every block of frame `counter`, row after row.
    [[nodiscard]] std::vector<ClipBlock> blocks(std::uint32_t counter) const;
    // How many frames back the frame to predict the next one from is; 0: the next is a keyframe.
    [[nodiscard]] std::size_t reference_back() const;
    // The picture of `back` frames back (1: the last frame written).
    [[nodiscard]] const std::vector<ClipBlock>& shown(std::size_t back) const { return kept_[kept_.size() - back].shown; }
    // After writing a frame: what it shows and how many frames back it was predicted from (0: a keyframe).
    void keep(std::uint32_t counter, std::vector<ClipBlock> shown, std::size_t used);
    [[nodiscard]] std::size_t kept() const noexcept { return kept_.size(); }

    unsigned width_, height_, columns_, rows_;

private:
    struct Kept {
        std::uint32_t counter = 0;
        std::vector<ClipBlock> shown;  // the picture, as a decoder with every frame shows it
        bool lost = false;             // the client said it did not get it
        bool built_on_lost = false;    // predicted from a lost frame, or from one built on one
        std::size_t used = 0;          // how many frames back the frame it is predicted from was
    };
    void mark_built_on_lost();

    std::deque<Kept> kept_;  // newest last
    bool mark_ = false;
};

// The clip as H.264.
class ClipWriter final : public ClipEncoder {
public:
    ClipWriter(unsigned width, unsigned height) : ClipEncoder(width, height) {}

    [[nodiscard]] std::vector<std::uint8_t> parameter_sets() const override;
    [[nodiscard]] std::vector<std::uint8_t> frame(std::uint32_t counter, bool keyframe) override;

private:
    unsigned frame_number_ = 0;  // of the next frame, counted from the last keyframe
    unsigned keyframes_ = 0;
};

}  // namespace psvp::media
