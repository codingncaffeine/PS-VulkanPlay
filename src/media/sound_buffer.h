// A stream's sound frames between their arrival and the sound device: a short
// line of frames, still as the console coded them, from which the device takes
// one whenever it needs ten milliseconds more. Frames arrive unevenly and the
// device plays evenly; the line is what lies between. When it is empty the sound
// before is continued for a moment instead of a hole (the decoder makes that
// up), and when frames then arrive in a bunch the oldest are let go, since their
// time has been played already. It decides; whoever owns the device decodes and
// plays. No sound device, no decoder and no clock of its own in here, and no
// lock: the owner calls it from one thread at a time.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace psvp::media {

class SoundBuffer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    // How many frames are aimed to be in line when the device takes one. A frame is 10 ms.
    static constexpr std::size_t kLeastDepth = 2, kStartDepth = 3, kMostDepth = 5;
    // This many frames beyond the aim, the oldest are let go, down to one beyond it.
    static constexpr std::size_t kSurplus = 3;
    // Frames in a row that continue the sound before with nothing in line; silence after that.
    static constexpr unsigned kMostConcealed = 8;
    // Frames filled in for frames that were lost on the way, per gap: a long gap is silence, not a held note.
    static constexpr std::uint32_t kMostFilled = 5;
    // The speed of playing follows the line's length, within this of the recorded speed either way.
    static constexpr double kMostChange = 0.04;
    static constexpr double kBand = 1.0;   // frames off the aim that change nothing
    static constexpr double kFull = 3.0;   // frames beyond the band at which the change is the most
    static constexpr std::chrono::seconds kCalm{10};  // so long without running empty brings the aim down a frame
    static constexpr unsigned kPeriodMemory = 256;    // pieces after which a smaller piece counts as the device's size

    enum class Kind {
        frame,      // decode `frame` and play it
        concealed,  // play what the decoder makes up to continue the sound before
        silence,
    };
    struct Next {
        Kind kind = Kind::silence;
        std::vector<std::uint8_t> frame;
    };
    struct Counters {
        std::uint64_t arrived = 0;
        std::uint64_t played = 0;        // frames handed to the device as they came
        std::uint64_t filled = 0;        // made up for frames that were lost on the way
        std::uint64_t concealed = 0;     // made up while the line was empty
        std::uint64_t silent = 0;        // silence, when the line stayed empty for longer
        std::uint64_t let_go = 0;        // arrived too late to be played: their time had been played
        std::uint64_t ran_empty = 0;     // times the line ran empty, however long each lasted
        std::uint64_t takes = 0;         // frames' worth of sound the device took
        std::uint64_t periods = 0;       // pieces it took them in
        std::uint64_t depth_sum = 0;     // of the frames in line when a piece began: the mean is depth_sum / periods
        std::size_t least_depth = 0, most_depth = 0;
        std::size_t largest_period = 0;  // frames in the largest piece
    };

    // The device is about to take this many frames in a row (a sound device takes a piece at a
    // time, of its own size). To be called before those calls of pull().
    void begin_period(std::size_t frames) noexcept;
    // A frame arrived; `lost_before` frames that should have come before it never did.
    void push(std::vector<std::uint8_t> frame, std::uint32_t lost_before, TimePoint now);
    // The device needs a frame's worth of sound.
    [[nodiscard]] Next pull(TimePoint now);
    // How fast to play what is taken: 1 is as it was recorded.
    [[nodiscard]] double speed() const noexcept;

    [[nodiscard]] std::size_t depth() const noexcept { return line_.size(); }
    // Frames aimed to be in line when the device begins to take: the aim for a device that takes
    // one at a time, and what a larger piece takes on top of that.
    [[nodiscard]] std::size_t aim() const noexcept { return aim_ + period_ - 1; }
    [[nodiscard]] std::size_t period() const noexcept { return period_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

private:
    struct Entry {
        bool lost = false;  // stands for a frame that never came
        std::vector<std::uint8_t> frame;
    };
    std::deque<Entry> line_;
    bool playing_ = false;  // the line has been filled to its aim once
    bool empty_ = false;    // it ran empty and nothing has come since
    unsigned concealed_run_ = 0;
    std::size_t aim_ = kStartDepth;
    std::size_t period_ = 1;          // the largest piece the device took of late
    unsigned periods_since_ = 0;      // pieces since one of that size
    double smoothed_depth_ = kStartDepth;
    TimePoint last_change_{};
    Counters counters_;
};

}  // namespace psvp::media
