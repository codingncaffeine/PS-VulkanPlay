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
// they are not played back one after the other. No clock and no thread of its
// own in here.
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

    explicit FramePacer(double frames_per_second) noexcept;

    struct Slot {
        TimePoint at;       // when to show it; never before it is ready
        // It would have had to wait for longer than the most: frames have piled up behind a
        // pause. It is to be shown at once, and not at all when a newer one is already there.
        bool late = false;
    };
    // A picture is ready at `ready`, the one after the picture of the last call.
    [[nodiscard]] Slot schedule(TimePoint ready) noexcept;
    // The picture of the last call was not shown after all (a newer one took its place).
    void skipped() noexcept;

    // The delay every picture of the last second carried: what is being given back.
    [[nodiscard]] Duration spare() const noexcept;

private:
    Duration period_;
    std::optional<TimePoint> last_at_;   // when the last picture that was shown was shown
    std::optional<TimePoint> before_;    // and the one before it, for skipped()
    std::array<Duration, kMemory> waits_{};
    std::size_t waits_count_ = 0, waits_at_ = 0;
};

}  // namespace psvp::media
