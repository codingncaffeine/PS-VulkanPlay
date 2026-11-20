// Says when to show each picture so that pictures follow one another evenly.
// Frames leave a console up to sixty times a second and arrive unevenly: held
// up on the way by a few milliseconds, two at a time, or after a pause. Shown
// as they come, one picture stays for a few milliseconds and the next for
// thirty. The pacer shows a picture no sooner than a frame's time after the one
// before it. That alone evens out frames that come in bunches, and costs only
// what the bunching is: a picture that comes right behind a late one waits, the
// late one does not. It makes no row of places from the frames' numbers: a
// console leaves frames out when a game does not make sixty pictures, and the
// numbers do not say so.
//
// What a picture waits is watched: when every picture of the last second waited,
// the least of those waits is delay nobody needs, and it is given back a little
// with every picture. A picture is never held for longer than a frame and a
// half: behind a pause on the network the frames of that pause come at once, and
// they are not played back one after the other.
//
// A cushion can be asked for: that much delay is then kept on purpose, so that
// a frame which comes up to that much later than the others is still shown in
// its place. It costs exactly that delay, all the time, and is nothing unless
// asked for. No clock and no thread of its own in here.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace psvp::media {

class FramePacer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::microseconds;
    // A picture is never held back by more than this: a frame and a half at sixty a second.
    static constexpr Duration kMostWait{25000};
    // Delay that every picture of the last second carried is given back by this much per picture at most.
    static constexpr Duration kGiveBack{1500};
    // The pictures whose waits are looked at for that: a second's worth.
    static constexpr std::size_t kMemory = 60;

    // What is taken up per picture at most while the cushion asked for is not there yet.
    static constexpr Duration kTakeUp{1500};
    // The cushion is measured on the pictures of the last second that waited least, but for this
    // many: the frames that come behind a pause say nothing of how the others come.
    static constexpr std::size_t kSeldom = 6;
    // Pictures there must be before the cushion is taken up.
    static constexpr std::size_t kLeastKnown = 12;
    // No cushion is longer than this: three frames' time.
    static constexpr Duration kMostCushion{50000};

    explicit FramePacer(double frames_per_second, Duration cushion = Duration{0}) noexcept;

    struct Slot {
        TimePoint at;       // when to show it; never before it is ready
        // The stream is behind: frames have piled up behind a pause. The picture is to be shown
        // at once, and the pictures behind it take their distance from it.
        bool late = false;
    };
    // A picture is ready at `ready`, the one after the picture of the last call. `earliest` is
    // when it could have been ready had nothing stood in its way here: when its frame arrived,
    // and the time decoding it took. A picture that waited for the one before it to be shown has
    // carried that delay already, and only this says so. `waiting` is how many frames that came
    // after it are already there to be decoded: more of them than the cushion is long, and the
    // stream is behind. Without that, the frames of a pause would be shown a frame's time apart
    // one after the other, and the pause would stay as delay.
    [[nodiscard]] Slot schedule(TimePoint earliest, TimePoint ready, std::size_t waiting) noexcept;
    // The same for a picture that nothing stood in the way of.
    [[nodiscard]] Slot schedule(TimePoint ready, std::size_t waiting = 0) noexcept { return schedule(ready, ready, waiting); }
    // The picture of the last call was not shown after all (a newer one took its place).
    void skipped() noexcept;

    // The delay every picture of the last second carried: what is given back, but for the cushion.
    [[nodiscard]] Duration spare() const noexcept;
    [[nodiscard]] Duration cushion() const noexcept { return cushion_; }

private:
    // What all but a few of the pictures remembered waited at least; nothing while there are too few.
    [[nodiscard]] std::optional<Duration> usual_least() const noexcept;

    Duration period_;
    Duration cushion_;
    std::optional<TimePoint> last_at_;   // when the last picture that was shown was shown
    std::optional<TimePoint> before_;    // and the one before it, for skipped()
    std::array<Duration, kMemory> waits_{};
    std::size_t waits_count_ = 0, waits_at_ = 0;
};

}  // namespace psvp::media
