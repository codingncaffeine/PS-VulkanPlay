#include <cmath>
#include <cstdint>
#include <span>

#include "check.h"
#include "proto/hex.h"
#include "proto/input.h"
#include "proto/motion.h"

using namespace psvp::proto;
using namespace psvp::proto::transport;

namespace {

constexpr Axes kStill{0.0F, 0.0F, 0.0F};
constexpr Axes kFlat{0.0F, 1.0F, 0.0F};

// Readings every millisecond, the gyroscope then the accelerometer, as a controller sends them.
void feed(MotionTracker& tracker, std::uint64_t& now_us, int count, const Axes& rate, const Axes& pull) {
    for (int i = 0; i < count; ++i) {
        now_us += 1000;
        tracker.gyro(rate, now_us);
        tracker.accel(pull, now_us);
    }
}

bool near(float a, float b, float within) { return std::fabs(a - b) <= within; }

// The angle between two rotations, in radians (either sign of a rotation is the same rotation):
// that of a's inverse followed by b, from its parts' sizes (acos of a dot product near 1 loses
// everything single precision has).
float apart(const Rotation& a, const Rotation& b) {
    const auto wide = [](float v) { return static_cast<double>(v); };
    const double aw = wide(a.w), ax = wide(a.x), ay = wide(a.y), az = wide(a.z), bw = wide(b.w), bx = wide(b.x), by = wide(b.y), bz = wide(b.z);
    const double w = aw * bw + ax * bx + ay * by + az * bz;
    const double x = aw * bx - ax * bw - ay * bz + az * by;
    const double y = aw * by + ax * bz - ay * bw - az * bx;
    const double z = aw * bz - ax * by + ay * bx - az * bw;
    return static_cast<float>(2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::fabs(w)));
}

std::string packed(const Rotation& r) {
    InputState state;
    state.orient_x = r.x;
    state.orient_y = r.y;
    state.orient_z = r.z;
    state.orient_w = r.w;
    const auto bytes = encode_input_state(ConsoleKind::ps5, state);
    return hex_encode(std::span<const std::uint8_t>(bytes).subspan(13, 4));
}

}  // namespace

// At rest on a table: no rotation, and the packed form docs/protocol/input.md reads for it.
PSVP_TEST(motion_at_rest_is_no_rotation) {
    MotionTracker tracker;
    CHECK(tracker.rotation() == Rotation{} || apart(tracker.rotation(), Rotation{}) < 1e-6F);
    // Still at every reading, the first ones too (where the filter pulls hardest).
    std::uint64_t now = 0;
    float worst = 0.0F;
    for (int i = 0; i < 2000; ++i) {
        feed(tracker, now, 1, kStill, kFlat);
        worst = std::fmax(worst, apart(tracker.rotation(), Rotation{}));
    }
    CHECK(worst < 1e-5F);
    CHECK(packed(tracker.rotation()) == "fef7ef1f");
    CHECK(tracker.acceleration() == kFlat);
}

// A turn about the upright axis that gravity cannot see: the rates of turn alone carry it.
PSVP_TEST(motion_follows_a_turn_about_the_upright_axis) {
    MotionTracker tracker;
    std::uint64_t now = 0;
    feed(tracker, now, 100, kStill, kFlat);
    feed(tracker, now, 1000, Axes{0.0F, 1.0F, 0.0F}, kFlat);  // one radian a second for a second
    const Rotation r = tracker.rotation();
    CHECK(near(std::fabs(r.y), std::sin(0.5F), 0.01F) && near(std::fabs(r.w), std::cos(0.5F), 0.01F));
    CHECK(near(r.x, 0.0F, 0.01F) && near(r.z, 0.0F, 0.01F));
    // The other way back is where it started.
    feed(tracker, now, 1000, Axes{0.0F, -1.0F, 0.0F}, kFlat);
    CHECK(apart(tracker.rotation(), Rotation{}) < 0.02F);
}

// The same turn read on a clock that runs twice as fast between readings turns twice as far: the
// sensor's time, not the number of readings, carries it.
PSVP_TEST(motion_takes_its_time_from_the_sensor_clock) {
    MotionTracker tracker;
    std::uint64_t now = 0;
    feed(tracker, now, 100, kStill, kFlat);
    for (int i = 0; i < 500; ++i) {
        now += 2000;
        tracker.gyro(Axes{0.0F, 1.0F, 0.0F}, now);
        tracker.accel(kFlat, now);
    }
    CHECK(near(apart(tracker.rotation(), Rotation{}), 1.0F, 0.02F));
    // A clock that goes back moves nothing.
    const Rotation before = tracker.rotation();
    tracker.gyro(Axes{0.0F, 5.0F, 0.0F}, now - 500000);
    CHECK(apart(tracker.rotation(), before) < 1e-3F);
}

// Held tilted from the first reading: the fast pull of the first readings finds the tilt, the slow
// one keeps it.
PSVP_TEST(motion_finds_a_tilt_from_gravity) {
    MotionTracker tracker;
    std::uint64_t now = 0;
    const float tilt = 0.5F;  // radians, about the controller's x axis
    const Axes pull{0.0F, std::cos(tilt), std::sin(tilt)};
    feed(tracker, now, 3000, kStill, pull);
    CHECK(near(apart(tracker.rotation(), Rotation{}), tilt, 0.02F));
    CHECK(near(std::fabs(tracker.rotation().x), std::sin(tilt / 2.0F), 0.01F));
}

// "Take this position": the tilt becomes rest, acceleration included; tilting back to flat is then
// a tilt the other way.
PSVP_TEST(motion_take_as_rest_makes_this_position_rest) {
    MotionTracker tracker;
    std::uint64_t now = 0;
    const float tilt = 0.5F;
    const Axes pull{0.0F, std::cos(tilt), std::sin(tilt)};
    feed(tracker, now, 3000, kStill, pull);
    tracker.take_as_rest();
    CHECK(apart(tracker.rotation(), Rotation{}) < 1e-6F);
    CHECK(tracker.acceleration() == kFlat);
    feed(tracker, now, 1000, kStill, pull);
    CHECK(apart(tracker.rotation(), Rotation{}) < 1e-3F);
    CHECK(near(tracker.acceleration()[1], 1.0F, 1e-6F) && near(tracker.acceleration()[2], 0.0F, 1e-6F));
    // The rate of turn is still told as read.
    tracker.gyro(Axes{0.25F, 0.0F, 0.0F}, now + 1000);
    CHECK(tracker.rate()[0] == 0.25F);
}

// Small wobbles of the rotation are held back, as the kernel holds back jitter; a real move is not.
PSVP_TEST(motion_holds_back_jitter_not_moves) {
    MotionTracker tracker;
    std::uint64_t now = 0;
    feed(tracker, now, 2000, kStill, kFlat);
    const Rotation rest = tracker.rotation();
    // A tremor of a hundredth of a radian a second, back and forth every reading.
    for (int i = 0; i < 200; ++i) {
        now += 1000;
        tracker.gyro(Axes{0.0F, (i % 2 == 0) ? 0.01F : -0.01F, 0.0F}, now);
    }
    CHECK(tracker.rotation() == rest);
    feed(tracker, now, 100, Axes{0.0F, 2.0F, 0.0F}, kFlat);
    CHECK(near(apart(tracker.rotation(), rest), 0.2F, 0.01F));
}

// A slow turn arrives whole, at the Edge's 1000 readings a second too (the reference's filter
// holds such a turn back inside itself: 0.5 radians a second came out as 0.001 radians in a second).
PSVP_TEST(motion_slow_turns_are_not_lost) {
    for (const float rate : {0.1F, 0.25F, 0.5F}) {
        MotionTracker tracker;
        std::uint64_t now = 0;
        feed(tracker, now, 500, kStill, kFlat);
        feed(tracker, now, 1000, Axes{0.0F, rate, 0.0F}, kFlat);
        CHECK(near(apart(tracker.rotation(), Rotation{}), rate, 0.01F));
    }
}

// The packed orientation reads back as the rotation that was packed, to within its nine-bit steps.
PSVP_TEST(motion_packed_rotation_reads_back) {
    const Rotation rotations[] = {Rotation{}, Rotation{0.2F, 0.0F, 0.0F, std::sqrt(1.0F - 0.04F)}, Rotation{-0.5F, 0.5F, -0.5F, 0.5F},
                                  Rotation{0.1F, -0.7F, 0.3F, -std::sqrt(1.0F - 0.59F)}};
    for (const Rotation& r : rotations) {
        InputState state;
        state.orient_x = r.x;
        state.orient_y = r.y;
        state.orient_z = r.z;
        state.orient_w = r.w;
        const auto bytes = encode_input_state(ConsoleKind::ps5, state);
        const auto fields = parse_input_state(ConsoleKind::ps5, bytes);
        CHECK(fields.has_value());
        if (!fields) continue;
        const auto q = unpacked_rotation(fields->orientation);
        CHECK(apart(Rotation{q[0], q[1], q[2], q[3]}, r) < 0.01F);
    }
}
