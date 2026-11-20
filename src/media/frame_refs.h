// Which earlier frames a frame of an HEVC stream is predicted from, read from the
// frame itself, and the same frame made to use other ones. This is what lets the
// picture go on after a lost frame instead of standing still until the console's
// repair (docs/protocol/video.md, "After a lost frame"). The syntax read here is
// the one of ITU-T H.265, 7.3.1 to 7.3.7. No decoder and no I/O in here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psvp::media {

// What a stream's parameter sets say that the layout of a frame's first bits depends on.
struct HevcLayout {
    unsigned parameter_set = 0;      // the picture parameter set's number
    unsigned order_bits = 0;         // bits of a frame's order count
    unsigned address_bits = 0;       // bits of a slice's address in the picture
    unsigned extra_bits = 0;         // bits a slice begins with that mean nothing here
    bool dependent_slices = false;
    bool output_flag = false;
    bool color_planes_apart = false;
    // A slice may reorder the frames it uses: then the number of frames used cannot change.
    bool lists_changeable = false;
    friend bool operator==(const HevcLayout&, const HevcLayout&) = default;
};

// From bytes that hold a stream's sequence and picture parameter sets: the header a console
// sends before the first frame, or a frame that carries them. Streams of a shape this reader
// does not follow are refused with the reason (scaling lists, reference sets or long-term
// frames in the parameter sets, more than one parameter set of a kind).
[[nodiscard]] std::expected<HevcLayout, std::string> read_hevc_layout(std::span<const std::uint8_t> bytes);
// True when the bytes hold a sequence or picture parameter set.
[[nodiscard]] bool carries_hevc_parameter_sets(std::span<const std::uint8_t> bytes) noexcept;

// An earlier frame that a frame names: how many frames back it is, and whether the frame is
// predicted from it (a frame also names frames it only wants kept for later ones).
struct NamedFrame {
    unsigned back = 0;
    bool used = false;
    friend bool operator==(const NamedFrame&, const NamedFrame&) = default;
};

struct HevcFrame {
    bool restart = false;            // a random access picture: nothing before it is needed any more
    bool intra = false;              // predicted from no other frame
    unsigned order = 0;              // the low bits of its order count
    std::vector<NamedFrame> named;   // nearest first
};

// One frame as a console sends it: the NAL units of one picture, each behind a start code.
[[nodiscard]] std::expected<HevcFrame, std::string> read_hevc_frame(const HevcLayout& layout, std::span<const std::uint8_t> frame);
// The same frame, predicted from the named frames marked in `used` (one entry per named
// frame, in the order read_hevc_frame gives them) and from no other.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> with_used_frames(const HevcLayout& layout, std::span<const std::uint8_t> frame,
                                                                                     const std::vector<bool>& used);

// Follows which frames a decoder holds, and says for each frame that arrives how it can be decoded.
class ReferenceTracker {
public:
    static constexpr std::size_t kKept = 16;  // a stream's frames name no more earlier ones than this

    enum class Way {
        as_sent,       // the decoder holds every frame this one is predicted from
        changed,       // it does not: decode `changed`, predicted from frames the decoder holds
        no_reference,  // the decoder holds none of the frames it names: it cannot be decoded
        unread,        // the frame is of a shape this reader does not follow: `why`
    };
    struct Verdict {
        Way way = Way::as_sent;
        // The picture will be the console's, sample for sample. False for a changed frame and
        // for every frame built on one, until a frame that is built on exact ones only.
        bool exact = true;
        std::vector<std::uint8_t> changed;
        std::string why;
    };

    // `header` is what the console sent before the first frame.
    explicit ReferenceTracker(std::span<const std::uint8_t> header);
    // To be called for every frame, in the stream's order, with its number in the stream.
    [[nodiscard]] Verdict look(std::uint16_t index, std::span<const std::uint8_t> frame);
    // After look(): whether the decoder gave a picture for that frame (it holds the frame then).
    void decoded(std::uint16_t index, bool gave_picture);

    // Why frames are not read, when they are not.
    [[nodiscard]] const std::string& unread_reason() const noexcept { return off_; }

private:
    struct Held {
        std::uint16_t index = 0;
        bool exact = true;
    };
    struct Pending {
        std::uint16_t index = 0;
        unsigned order = 0;
        bool exact = true;
        bool restart = false;
    };
    [[nodiscard]] const Held* held(std::uint16_t index) const noexcept;
    Verdict unread(std::string why);

    std::optional<HevcLayout> layout_;
    std::string off_;                  // not empty: nothing is read until a random access picture
    std::deque<Held> held_;            // newest last
    std::optional<Pending> anchor_;    // the newest frame the decoder holds
    std::optional<Pending> pending_;
};

}  // namespace psvp::media
