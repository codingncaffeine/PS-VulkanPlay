// The controller's orientation, worked out from its motion sensors, as the input state carries it
// (docs/protocol/input.md, "Motion"). A console wants the rotation of the controller from where it
// rests, not only the rates of turn: games that aim or steer by tilting read it. Nothing here
// touches a device; the player hands in each reading with the sensor's own time.
//
// The fusion is Madgwick's gradient-descent filter for a gyroscope with an accelerometer
// (S. Madgwick, "An efficient orientation filter for inertial and inertial/magnetic sensor arrays",
// 2010): the rates of turn move the rotation, and a step down the gradient of "where gravity should
// point" against "where the accelerometer says it points" pulls the tilt back, with gain beta.
// The behavior the console's games are used to (the reference's, read in its source): a fast pull
// while the first readings come in, a slow one after, small changes held back as the kernel's input
// layer holds back jitter (in what is told only: the reference holds them back inside the filter,
// where a slow turn then never registers), and "take this position" (the console's request, kind 9)
// making the controller's present position the one it rests in, acceleration included.
#pragma once

#include <array>
#include <cstdint>

namespace psvp::proto {

struct Rotation {
    float x = 0.0F, y = 0.0F, z = 0.0F, w = 1.0F;
    friend bool operator==(const Rotation&, const Rotation&) = default;
};

// Rates of turn in radians a second, acceleration in multiples of the earth's pull, both along the
// controller's axes as SDL gives them (at rest on a table, gravity pulls along +y).
using Axes = std::array<float, 3>;

class MotionTracker {
public:
    MotionTracker() { start(); }

    // One reading of either sensor; `time_us` is the sensor's own clock, in microseconds. The other
    // sensor's last reading is used with it, as both move the same rotation.
    void gyro(const Axes& rate, std::uint64_t time_us);
    void accel(const Axes& pull, std::uint64_t time_us);

    // "Take this position": from now on the controller is at rest where it is now.
    void take_as_rest();

    // What the console is told.
    [[nodiscard]] Rotation rotation() const;               // the rotation from rest; none at rest
    [[nodiscard]] const Axes& rate() const { return rate_; }
    [[nodiscard]] const Axes& acceleration() const { return pull_; }  // with the rest position's offset taken off

    // The filter's gains and how many readings count as the first ones.
    static constexpr float kFirstGain = 20.0F;
    static constexpr float kGain = 0.05F;
    static constexpr std::uint64_t kFirstReadings = 30;

private:
    void start();
    void step(std::uint64_t time_us);

    Rotation q_;                         // in the filter's frame: gravity along its z
    Rotation told_;                      // q_ with its jitter held back: what the console is told
    Axes rate_{0.0F, 0.0F, 0.0F};
    Axes pull_{0.0F, 1.0F, 0.0F};        // the last acceleration, offset taken off
    Axes real_pull_{0.0F, 1.0F, 0.0F};   // the last acceleration as the sensor read it
    Axes offset_{0.0F, 0.0F, 0.0F};      // what take_as_rest() takes off every reading
    std::uint64_t readings_ = 0;
    std::uint64_t last_us_ = 0;
};

}  // namespace psvp::proto
