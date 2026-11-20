#include <chrono>
#include <cstdint>
#include <vector>

#include "check.h"
#include "media/sound_buffer.h"

using namespace psvp::media;
using namespace std::chrono_literals;

namespace {
using Kind = SoundBuffer::Kind;
const SoundBuffer::TimePoint kStart{};
// A frame that says which one it is.
std::vector<std::uint8_t> frame(unsigned number) { return {static_cast<std::uint8_t>(number >> 8), static_cast<std::uint8_t>(number & 0xff)}; }
unsigned number_of(const SoundBuffer::Next& next) { return next.frame.size() == 2 ? (static_cast<unsigned>(next.frame[0]) << 8) | next.frame[1] : 0xffffU; }
}  // namespace

PSVP_TEST(sound_buffer_plays_a_steady_stream_as_it_comes) {
    SoundBuffer buffer;
    // Nothing has come: silence that counts for nothing.
    CHECK(buffer.pull(kStart).kind == Kind::silence && buffer.counters().takes == 0);
    // Playing begins when the line has been filled to its aim once.
    buffer.push(frame(1), 0, kStart);
    buffer.push(frame(2), 0, kStart);
    CHECK(buffer.pull(kStart).kind == Kind::silence && buffer.counters().takes == 0 && buffer.depth() == 2);
    buffer.push(frame(3), 0, kStart);
    // Frames come two at a time every 20 ms, as a console sends them; the device takes one every 10 ms.
    unsigned next_in = 4, next_out = 1;
    bool in_order = true, even_speed = true;
    for (int tick = 0; tick < 900; ++tick) {
        const auto now = kStart + tick * 10ms;
        if (tick % 2 == 1) {
            buffer.push(frame(next_in++), 0, now);
            buffer.push(frame(next_in++), 0, now);
        }
        buffer.begin_period(1);
        const auto taken = buffer.pull(now);
        in_order = in_order && taken.kind == Kind::frame && number_of(taken) == next_out++;
        even_speed = even_speed && buffer.speed() == 1.0;
    }
    CHECK(in_order && even_speed);
    const auto& counters = buffer.counters();
    CHECK(counters.played == 900 && counters.concealed == 0 && counters.silent == 0 && counters.let_go == 0 && counters.ran_empty == 0);
    CHECK(counters.least_depth >= 2 && counters.most_depth <= 4 && counters.periods == 900 && counters.largest_period == 1);
    // Nine seconds without running empty: the aim is still the first one; after ten it comes down.
    CHECK(buffer.aim() == SoundBuffer::kStartDepth);
    buffer.push(frame(next_in++), 0, kStart + 10s);
    CHECK(buffer.aim() == SoundBuffer::kStartDepth - 1);
}

PSVP_TEST(sound_buffer_continues_the_sound_over_a_pause) {
    SoundBuffer buffer;
    for (unsigned i = 1; i <= 3; ++i) buffer.push(frame(i), 0, kStart);
    for (unsigned i = 1; i <= 3; ++i) CHECK(number_of(buffer.pull(kStart)) == i);
    // Nothing comes for 100 ms: the sound before is continued for eight frames' time, then silence.
    for (unsigned i = 0; i < SoundBuffer::kMostConcealed; ++i) CHECK(buffer.pull(kStart + 30ms).kind == Kind::concealed);
    CHECK(buffer.pull(kStart + 110ms).kind == Kind::silence);
    CHECK(buffer.pull(kStart + 120ms).kind == Kind::silence);
    CHECK(buffer.counters().ran_empty == 1 && buffer.counters().concealed == SoundBuffer::kMostConcealed && buffer.counters().silent == 2);
    // Running empty once makes the aim a frame longer, however long it lasted.
    CHECK(buffer.aim() == SoundBuffer::kStartDepth + 1);

    // The ten frames of that time arrive in a bunch, one behind the other. Their time has been played:
    // when the line is more than three frames beyond its aim of four, the oldest are let go, down to
    // one beyond it. That happens at the eighth of them: frames 4, 5 and 6 go.
    for (unsigned i = 4; i <= 13; ++i) buffer.push(frame(i), 0, kStart + 130ms);
    CHECK(buffer.counters().let_go == 3 && buffer.depth() == 7);
    CHECK(buffer.depth() <= buffer.aim() + SoundBuffer::kSurplus);
    // What is played next is the oldest that stayed; the few frames too many go by playing a little faster.
    CHECK(number_of(buffer.pull(kStart + 130ms)) == 7);
    CHECK(number_of(buffer.pull(kStart + 140ms)) == 8);
    // A second pause is a second time.
    while (buffer.depth() != 0) (void)buffer.pull(kStart + 150ms);
    CHECK(buffer.pull(kStart + 200ms).kind == Kind::concealed && buffer.counters().ran_empty == 2);
    // The aim has an end, whatever the network does.
    for (int i = 0; i < 10; ++i) {
        buffer.push(frame(100), 0, kStart + 1s + i * 100ms);
        (void)buffer.pull(kStart + 1s + i * 100ms);
        (void)buffer.pull(kStart + 1s + i * 100ms);
    }
    CHECK(buffer.aim() == SoundBuffer::kMostDepth);
}

PSVP_TEST(sound_buffer_keeps_the_place_of_a_lost_frame) {
    SoundBuffer buffer;
    buffer.push(frame(1), 0, kStart);
    // Frames 2 and 3 were lost on the way: the decoder fills their time, between 1 and 4.
    buffer.push(frame(4), 2, kStart);
    CHECK(buffer.depth() == 4);
    CHECK(number_of(buffer.pull(kStart)) == 1);
    CHECK(buffer.pull(kStart).kind == Kind::concealed);
    CHECK(buffer.pull(kStart).kind == Kind::concealed);
    CHECK(number_of(buffer.pull(kStart)) == 4);
    CHECK(buffer.counters().filled == 2 && buffer.counters().concealed == 0 && buffer.counters().ran_empty == 0);
    // A long gap is not filled whole.
    buffer.push(frame(50), 40, kStart);
    CHECK(buffer.depth() == SoundBuffer::kMostFilled + 1);
}

PSVP_TEST(sound_buffer_changes_the_speed_only_a_little) {
    SoundBuffer buffer;
    for (unsigned i = 1; i <= 3; ++i) buffer.push(frame(i), 0, kStart);
    // A line that stays two frames longer than aimed for, below the point where frames are let go:
    // played a little faster, never by more than the most.
    unsigned next = 4;
    double fastest = 1.0;
    for (int tick = 0; tick < 300; ++tick) {
        while (buffer.depth() < buffer.aim() + 3) buffer.push(frame(next++), 0, kStart + tick * 10ms);
        buffer.begin_period(1);
        (void)buffer.pull(kStart + tick * 10ms);
        fastest = std::max(fastest, buffer.speed());
    }
    CHECK(fastest > 1.0 && fastest <= 1.0 + SoundBuffer::kMostChange);
    CHECK(buffer.counters().let_go == 0);
    // A line that stays nearly empty: a little slower, which lets it fill.
    double slowest = 1.0;
    while (buffer.depth() > 1) (void)buffer.pull(kStart + 3s);
    for (int tick = 0; tick < 300; ++tick) {
        buffer.begin_period(1);
        (void)buffer.pull(kStart + 3s + tick * 10ms);
        buffer.push(frame(next++), 0, kStart + 3s + tick * 10ms);
        slowest = std::min(slowest, buffer.speed());
    }
    CHECK(slowest < 1.0 && slowest >= 1.0 - SoundBuffer::kMostChange);
    CHECK(buffer.counters().ran_empty == 0);
}

// A device that takes its sound in larger pieces: the line is aimed that much longer, so that
// the last frame of a piece is still a frame that came, not one made up.
PSVP_TEST(sound_buffer_allows_for_the_size_of_the_devices_pieces) {
    SoundBuffer buffer;
    unsigned next_in = 1, next_out = 1;
    // Frames every 10 ms; the device takes four at a time, every 40 ms.
    bool in_order = true;
    for (int tick = 0; tick < 2000; ++tick) {
        const auto now = kStart + tick * 10ms;
        buffer.push(frame(next_in++), 0, now);
        if (tick % 4 != 3) continue;
        buffer.begin_period(4);
        for (int i = 0; i < 4; ++i) {
            const auto taken = buffer.pull(now);
            if (taken.kind == Kind::silence && buffer.counters().takes == 0) continue;  // before playing began
            in_order = in_order && taken.kind == Kind::frame && number_of(taken) == next_out++;
        }
    }
    CHECK(in_order);
    CHECK(buffer.period() == 4 && buffer.aim() >= SoundBuffer::kLeastDepth + 3);
    // It ran empty at most at the very start, while the first pieces showed their size.
    CHECK(buffer.counters().ran_empty == 0 && buffer.counters().concealed == 0 && buffer.counters().let_go == 0);
    CHECK(buffer.counters().largest_period == 4);
    // A device that turns to smaller pieces is believed after a while.
    for (int i = 0; i < 300; ++i) buffer.begin_period(1);
    CHECK(buffer.period() == 1);
}
