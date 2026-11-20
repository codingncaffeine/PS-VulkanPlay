#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "media/frame_refs.h"
#include "media/video_decoder.h"

using namespace psvp::media;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Way = ReferenceTracker::Way;

// Writes bits, the first bit of a byte being its highest.
class BitWriter {
public:
    void u(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            if (bits_ % 8 == 0) bytes_.push_back(0);
            if (((value >> i) & 1U) != 0) bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (0x80U >> (bits_ % 8)));
            ++bits_;
        }
    }
    // The variable-length kind: so many zeros, a one, as many more bits.
    void ue(std::uint32_t value) {
        unsigned zeros = 0;
        while (((value + 1) >> (zeros + 1)) != 0) ++zeros;
        u(0, zeros);
        u(value + 1, zeros + 1);
    }
    [[nodiscard]] std::size_t bits() const noexcept { return bits_; }
    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

private:
    Bytes bytes_;
    std::size_t bits_ = 0;
};

// A 3 before any byte of 0 to 3 that follows two zeros, written out here apart from the code under test.
Bytes escaped(const Bytes& payload) {
    Bytes out;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const std::size_t n = out.size();
        if (n >= 2 && out[n - 1] == 0 && out[n - 2] == 0 && payload[i] <= 3) out.push_back(3);
        out.push_back(payload[i]);
    }
    return out;
}

// The layout of a PS5's 1080p stream, as FFmpeg's header trace shows it.
HevcLayout console_layout() {
    HevcLayout layout;
    layout.order_bits = 4;
    layout.address_bits = 9;
    return layout;
}

// A frame that begins the way a console's frames do: one slice that names the `named`
// frames before it and is predicted from the one `used_back` frames back.
Bytes console_frame(const HevcLayout& layout, unsigned order, unsigned named, unsigned used_back, const Bytes& rest = {0x5a, 0xc3, 0x80}) {
    BitWriter bits;
    bits.u(0x0201, 16);  // an ordinary picture that others are predicted from, layer 0
    bits.u(1, 1);        // the picture's first slice
    bits.ue(0);          // parameter set 0
    bits.ue(1);          // predicted from one frame
    bits.u(order, layout.order_bits);
    bits.u(0, 1);        // its own list of frames follows
    bits.ue(named);
    bits.ue(0);          // no later frames
    for (unsigned i = 1; i <= named; ++i) {
        bits.ue(0);      // the frame just before the one named last
        bits.u(i == used_back ? 1 : 0, 1);
    }
    bits.u(0, static_cast<unsigned>((8 - bits.bits() % 8) % 8));
    Bytes payload = bits.bytes();
    payload.insert(payload.end(), rest.begin(), rest.end());
    Bytes frame{0, 0, 0, 1};
    const Bytes body = escaped(payload);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

// A picture that needs no other: the kind a stream begins with.
Bytes console_keyframe() {
    BitWriter bits;
    bits.u(0x2601, 16);  // an IDR picture, layer 0
    bits.u(1, 1);
    bits.u(0, 1);        // earlier pictures may still be shown
    bits.ue(0);
    bits.ue(2);          // predicted from no frame
    bits.u(0, static_cast<unsigned>((8 - bits.bits() % 8) % 8));
    Bytes frame{0, 0, 0, 1};
    frame.insert(frame.end(), bits.bytes().begin(), bits.bytes().end());
    frame.insert(frame.end(), {0x77, 0x80});
    return frame;
}

// How many frames back the one frame is that `frame` is predicted from; 0 when it is not exactly one.
unsigned used_back(const HevcLayout& layout, const Bytes& frame) {
    const auto read = read_hevc_frame(layout, frame);
    if (!read) return 0;
    unsigned back = 0, count = 0;
    for (const NamedFrame& named : read->named) {
        if (!named.used) continue;
        back = named.back;
        ++count;
    }
    return count == 1 ? back : 0;
}

struct HevcClip {
    Bytes header;
    std::vector<Bytes> frames;
};
HevcClip read_clip() {
    std::ifstream file(std::string(PSVP_TEST_DATA) + "/clip.h265", std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 <= bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) starts.push_back(i > 0 && bytes[i - 1] == 0 ? i - 1 : i);
    }
    HevcClip clip;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(starts[i]);
        const auto end = i + 1 < starts.size() ? bytes.begin() + static_cast<std::ptrdiff_t>(starts[i + 1]) : bytes.end();
        if (i < 3) clip.header.insert(clip.header.end(), begin, end);
        else clip.frames.emplace_back(begin, end);
    }
    return clip;
}

// How far two pictures are apart: the mean difference of their brightness samples.
double difference(const Picture& a, const Picture& b) {
    if (a.width != b.width || a.height != b.height || a.width == 0 || a.height == 0) return 255.0;
    std::uint64_t sum = 0;
    for (unsigned y = 0; y < a.height; ++y) {
        for (unsigned x = 0; x < a.width; ++x) {
            sum += static_cast<std::uint64_t>(std::abs(static_cast<int>(a.planes[0][y * a.strides[0] + x]) - static_cast<int>(b.planes[0][y * b.strides[0] + x])));
        }
    }
    return static_cast<double>(sum) / (static_cast<double>(a.width) * a.height);
}

}  // namespace

// The numbers are the ones FFmpeg's header trace prints for this clip.
PSVP_TEST(hevc_layout_is_read_from_the_parameter_sets) {
    const HevcClip clip = read_clip();
    const auto layout = read_hevc_layout(clip.header);
    CHECK(layout.has_value());
    if (!layout) return;
    CHECK(layout->parameter_set == 0 && layout->order_bits == 8);
    CHECK(layout->address_bits == 4);  // 320 by 184 in blocks of 64: fifteen of them
    CHECK(layout->extra_bits == 0 && !layout->dependent_slices && !layout->output_flag && !layout->color_planes_apart && !layout->lists_changeable);
    CHECK(carries_hevc_parameter_sets(clip.header));
    CHECK(!carries_hevc_parameter_sets(clip.frames[5]));

    // Cut short anywhere, or without one of the sets: refused, never read wrong.
    for (std::size_t size = 0; size < clip.header.size(); ++size) {
        const auto cut = read_hevc_layout(std::span(clip.header).first(size));
        CHECK(!cut || *cut == *layout);
    }
    CHECK(!read_hevc_layout({}));
}

PSVP_TEST(hevc_frames_tell_which_frames_they_use) {
    const HevcClip clip = read_clip();
    const auto layout = read_hevc_layout(clip.header);
    CHECK(layout.has_value() && clip.frames.size() == 48);
    if (!layout || clip.frames.size() != 48) return;
    std::size_t right = 0, same = 0;
    for (std::size_t i = 0; i < clip.frames.size(); ++i) {
        const auto frame = read_hevc_frame(*layout, clip.frames[i]);
        if (!frame) continue;
        // The first frame needs none; every later one uses the four before it, as far as there are four.
        bool as_made = frame->restart == (i == 0) && frame->intra == (i == 0) && frame->order == (i & 0xff) && frame->named.size() == std::min<std::size_t>(i, 4);
        for (std::size_t n = 0; n < frame->named.size(); ++n) as_made = as_made && frame->named[n].back == n + 1 && frame->named[n].used;
        if (as_made) ++right;
        // Asked for the frames it uses already, a frame comes back byte for byte.
        std::vector<bool> used;
        for (const NamedFrame& named : frame->named) used.push_back(named.used);
        const auto again = with_used_frames(*layout, clip.frames[i], used);
        if (again && *again == clip.frames[i]) ++same;
    }
    CHECK(right == 48);
    CHECK(same == 48);

    // Made to use the second and the fourth frame back only: it says so, and nothing else about it changed.
    const auto changed = with_used_frames(*layout, clip.frames[30], {false, true, false, true});
    CHECK(changed.has_value());
    if (!changed) return;
    const auto read = read_hevc_frame(*layout, *changed);
    CHECK(read && read->order == 30 && read->named.size() == 4 && !read->named[0].used && read->named[1].used && !read->named[2].used && read->named[3].used);
    CHECK(changed->size() == clip.frames[30].size());
    std::size_t bytes_changed = 0;
    for (std::size_t i = 0; i < changed->size(); ++i) bytes_changed += (*changed)[i] != clip.frames[30][i] ? 1U : 0U;
    CHECK(bytes_changed >= 1 && bytes_changed <= 2);
    CHECK(!with_used_frames(*layout, clip.frames[30], {true, true}));

    // Cut short anywhere: refused or read the same, and never past the end.
    const Bytes& frame = clip.frames[30];
    for (std::size_t size = 0; size < 40; ++size) {
        const auto cut = read_hevc_frame(*layout, std::span(frame).first(size));
        CHECK(!cut || cut->named.size() == 4);
    }
}

PSVP_TEST(console_shaped_frames_are_read_and_changed) {
    const HevcLayout layout = console_layout();
    const Bytes frame = console_frame(layout, 13, 9, 1);
    const auto read = read_hevc_frame(layout, frame);
    CHECK(read.has_value());
    if (!read) return;
    CHECK(!read->restart && !read->intra && read->order == 13 && read->named.size() == 9);
    for (unsigned i = 0; i < 9; ++i) CHECK(read->named[i].back == i + 1 && read->named[i].used == (i == 0));
    // The console's own repair looks like this: the fifth frame back.
    CHECK(used_back(layout, console_frame(layout, 3, 9, 5)) == 5);

    std::vector<bool> second(9, false);
    second[1] = true;
    const auto changed = with_used_frames(layout, frame, second);
    CHECK(changed && *changed == console_frame(layout, 13, 9, 2));

    const auto key = read_hevc_frame(layout, console_keyframe());
    CHECK(key && key->restart && key->intra && key->named.empty());

    // Refused: a frame of a kind that has no meaning yet, one that names another parameter set, one with nothing in it.
    Bytes odd = frame;
    odd[4] = 0x3c;  // unit type 30
    CHECK(!read_hevc_frame(layout, odd));
    HevcLayout other = layout;
    other.parameter_set = 1;
    CHECK(!read_hevc_frame(other, frame));
    CHECK(!read_hevc_frame(layout, {}));
    CHECK(!read_hevc_frame(layout, Bytes{0, 0, 0, 1}));
    CHECK(!read_hevc_frame(layout, Bytes{0, 0, 1, 0x4e, 0x01, 0x05}));  // only a note for the decoder, no picture
}

// A changed flag can make two zero bytes and a small one meet, which inside a unit would
// read as the start of the next one: the byte that keeps them apart comes and goes with it.
PSVP_TEST(a_changed_frame_keeps_its_start_codes_apart) {
    HevcLayout layout = console_layout();
    layout.order_bits = 7;  // puts the third frame's flag at the top of a byte
    const Bytes rest{0x00, 0x01, 0xab, 0x80};
    const Bytes third = console_frame(layout, 5, 3, 3, rest);
    const Bytes first = console_frame(layout, 5, 3, 1, rest);
    // With the third frame used the byte is 0x80; without it 0, and a 3 has to follow the two zeros.
    CHECK(first.size() == third.size() + 1);
    CHECK(used_back(layout, third) == 3 && used_back(layout, first) == 1);
    const auto to_first = with_used_frames(layout, third, {true, false, false});
    CHECK(to_first && *to_first == first);
    const auto to_third = with_used_frames(layout, first, {false, false, true});
    CHECK(to_third && *to_third == third);
    // Start codes in the result: the frame's own only.
    if (to_first) {
        std::size_t starts = 0;
        for (std::size_t i = 0; i + 3 <= to_first->size(); ++i) starts += (*to_first)[i] == 0 && (*to_first)[i + 1] == 0 && (*to_first)[i + 2] == 1 ? 1U : 0U;
        CHECK(starts == 1);
    }
}

PSVP_TEST(tracker_changes_the_frames_behind_a_lost_one) {
    // A tracker reads its layout from parameter sets; a console-shaped stream's are not at hand
    // here, so the clip's stand in and the frames are written for the clip's layout. The
    // stream's first frame has the number 1 and the order count 0.
    const HevcClip clip = read_clip();
    const auto layout = read_hevc_layout(clip.header);
    CHECK(layout.has_value());
    if (!layout) return;
    const auto frame = [&](unsigned index, unsigned back) { return console_frame(*layout, (index - 1) & 0xff, 9, back); };

    ReferenceTracker tracker(clip.header);
    CHECK(tracker.unread_reason().empty());
    const auto take = [&](std::uint16_t index, const Bytes& bytes, bool decodes = true) {
        auto verdict = tracker.look(index, bytes);
        if (verdict.way == Way::as_sent || verdict.way == Way::changed) tracker.decoded(index, decodes);
        return verdict;
    };
    CHECK(take(1, console_keyframe()).way == Way::as_sent);
    for (std::uint16_t index = 2; index <= 10; ++index) {
        const auto verdict = take(index, frame(index, 1));
        CHECK(verdict.way == Way::as_sent && verdict.exact);
    }
    // Frame 11 is lost. Frame 12 is predicted from it: it is made to use frame 10.
    const auto after = take(12, frame(12, 1));
    CHECK(after.way == Way::changed && !after.exact);
    CHECK(after.changed == frame(12, 2));
    // Frames 13 and 14 are built on 12: decodable as sent, and not the console's picture yet.
    for (std::uint16_t index = 13; index <= 14; ++index) {
        const auto verdict = take(index, frame(index, 1));
        CHECK(verdict.way == Way::as_sent && !verdict.exact);
    }
    // The console's repair, predicted from frame 10: exact again, and so is what follows.
    const auto repair = take(15, frame(15, 5));
    CHECK(repair.way == Way::as_sent && repair.exact);
    const auto next = take(16, frame(16, 1));
    CHECK(next.way == Way::as_sent && next.exact);

    // A frame the decoder gave no picture for is not held either.
    (void)take(17, frame(17, 1), false);
    const auto behind = take(18, frame(18, 1));
    CHECK(behind.way == Way::changed && behind.changed == frame(18, 2));

    // Six frames lost: frame 25 is made to use the nearest one held, seven back.
    const auto far = take(25, frame(25, 1));
    CHECK(far.way == Way::changed && far.changed == frame(25, 7));
    // A repair that is predicted from a frame that was lost too is changed like any other.
    const auto lost_repair = take(27, frame(27, 4));
    CHECK(lost_repair.way == Way::changed && lost_repair.changed == frame(27, 2));
}

PSVP_TEST(tracker_waits_for_a_keyframe_when_nothing_is_held) {
    const HevcClip clip = read_clip();
    const auto layout = read_hevc_layout(clip.header);
    CHECK(layout.has_value());
    if (!layout) return;
    const auto frame = [&](unsigned index, unsigned back) { return console_frame(*layout, index & 0xff, 9, back); };
    ReferenceTracker tracker(clip.header);
    const auto take = [&](std::uint16_t index, const Bytes& bytes) {
        auto verdict = tracker.look(index, bytes);
        if (verdict.way == Way::as_sent || verdict.way == Way::changed) tracker.decoded(index, true);
        return verdict;
    };
    // A stream joined behind its first frame: nothing to build on.
    CHECK(take(40, frame(40, 1)).way == Way::no_reference);
    CHECK(take(41, frame(41, 1)).way == Way::no_reference);
    CHECK(take(42, console_keyframe()).way == Way::as_sent);
    CHECK(take(43, frame(1, 1)).way == Way::as_sent);
    // Every frame it names is lost: frames 44 to 52.
    CHECK(take(53, frame(11, 1)).way == Way::no_reference);
    // Nor the next one: frame 53 was not decoded, and frame 43 is eleven back.
    CHECK(take(54, frame(12, 1)).way == Way::no_reference);
    // Further from the last frame held than a decoder can place an order count: nothing is looked up at all.
    CHECK(take(250, frame(77, 3)).way == Way::no_reference);
    CHECK(tracker.unread_reason().empty());

    // An order count that does not follow the frame's number: the stream is of another shape.
    // Nothing is read from then on, until a keyframe.
    ReferenceTracker other(clip.header);
    CHECK(other.look(1, console_keyframe()).way == Way::as_sent);
    other.decoded(1, true);
    CHECK(other.look(2, frame(1, 1)).way == Way::as_sent);
    other.decoded(2, true);
    CHECK(other.look(3, frame(7, 1)).way == Way::unread);
    CHECK(!other.unread_reason().empty());
    CHECK(other.look(4, frame(3, 1)).way == Way::unread);
    CHECK(other.look(5, console_keyframe()).way == Way::as_sent);
    other.decoded(5, true);
    CHECK(other.look(6, frame(1, 1)).way == Way::as_sent && other.unread_reason().empty());

    // Bytes that are no frame, and a header that is none.
    CHECK(other.look(7, Bytes{1, 2, 3, 4, 5}).way == Way::unread);
    ReferenceTracker headless(Bytes{0, 0, 1, 0x40, 0x01, 0x0c});
    CHECK(!headless.unread_reason().empty());
    CHECK(headless.look(1, console_keyframe()).way == Way::unread);
}

// The whole way, with FFmpeg's decoder: frame 20 of the clip is lost. Each of the next
// four frames is predicted from it among others; changed to use only frames the decoder
// holds, they decode to pictures close to the real ones, where the frames as sent decode
// to pictures built on a frame that is not there.
PSVP_TEST(pictures_go_on_behind_a_lost_frame) {
    const HevcClip clip = read_clip();
    CHECK(clip.frames.size() == 48);
    if (clip.frames.size() != 48) return;
    constexpr std::size_t kLost = 20;

    std::vector<Picture> real;
    {
        auto decoder = VideoDecoder::open(VideoCodec::hevc, clip.header);
        CHECK(decoder.has_value());
        if (!decoder) return;
        for (const Bytes& frame : clip.frames) {
            auto picture = decoder->decode(frame);
            CHECK(picture.has_value());
            if (!picture) return;
            real.push_back(std::move(*picture));
        }
    }

    std::vector<double> as_sent(clip.frames.size(), 0.0), tracked(clip.frames.size(), 0.0);
    {
        auto decoder = VideoDecoder::open(VideoCodec::hevc, clip.header);
        if (!decoder) return;
        for (std::size_t i = 0; i < clip.frames.size(); ++i) {
            if (i == kLost) continue;
            const auto picture = decoder->decode(clip.frames[i]);
            as_sent[i] = picture ? difference(*picture, real[i]) : 255.0;
        }
    }
    std::size_t changed = 0, sent = 0, exact = 0, damaged = 0, pictures = 0;
    {
        auto decoder = VideoDecoder::open(VideoCodec::hevc, clip.header);
        if (!decoder) return;
        ReferenceTracker tracker(clip.header);
        for (std::size_t i = 0; i < clip.frames.size(); ++i) {
            if (i == kLost) continue;
            const auto index = static_cast<std::uint16_t>(i + 1);
            const auto verdict = tracker.look(index, clip.frames[i]);
            CHECK(verdict.way == (i > kLost && i <= kLost + 4 ? Way::changed : Way::as_sent));
            if (verdict.way == Way::changed) ++changed;
            if (verdict.way == Way::as_sent) ++sent;
            if (verdict.exact) ++exact;
            const auto picture = decoder->decode(verdict.way == Way::changed ? std::span<const std::uint8_t>(verdict.changed) : std::span<const std::uint8_t>(clip.frames[i]));
            tracker.decoded(index, picture.has_value());
            if (!picture) continue;
            ++pictures;
            if (picture->damaged) ++damaged;
            tracked[i] = difference(*picture, real[i]);
        }
    }
    CHECK(changed == 4 && sent == 43 && pictures == 47);
    CHECK(exact == kLost);     // everything behind the loss is built on a changed frame: this clip has no repair
    CHECK(damaged == 0);       // the decoder held every frame it was asked to use
    for (std::size_t i = 0; i < kLost; ++i) CHECK(tracked[i] == 0.0 && as_sent[i] == 0.0);
    // The first frames behind the loss: far closer to the real picture than the frame as sent.
    CHECK(tracked[kLost + 1] < 6.0);
    CHECK(tracked[kLost + 1] * 2 < as_sent[kLost + 1]);
}
