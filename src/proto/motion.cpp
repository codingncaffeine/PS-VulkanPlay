#include "proto/motion.h"

#include <cmath>
#include <numbers>

namespace psvp::proto {

namespace {

// The filter wants gravity along its z; the controller at rest has it along y. Its frame is the
// controller's turned a quarter about x, and what the console is told is turned back.
constexpr float kHalf = std::numbers::sqrt2_v<float> / 2.0F;  // cos and sin of an eighth of a turn
constexpr Rotation kIntoFilter{kHalf, 0.0F, 0.0F, kHalf};      // a quarter turn about x
constexpr Rotation kOutOfFilter{-kHalf, 0.0F, 0.0F, kHalf};    // and back

// Hamilton's product then * first, written out: the rotation `first` followed by `then`.
Rotation compose(const Rotation& then, const Rotation& first) {
    return {
        then.w * first.x + then.x * first.w + then.y * first.z - then.z * first.y,
        then.w * first.y - then.x * first.z + then.y * first.w + then.z * first.x,
        then.w * first.z + then.x * first.y - then.y * first.x + then.z * first.w,
        then.w * first.w - then.x * first.x - then.y * first.y - then.z * first.z,
    };
}

// The kernel input layer's way with jitter: a change under half the step is not a change; under a
// step it is let through at a quarter, under two steps at four tenths; beyond that it is taken whole.
float steadied(float now, float before) {
    constexpr float kStep = 0.0007F;
    const float change = std::fabs(now - before);
    if (change < kStep / 2.0F) return before;
    if (change < kStep) return 0.75F * before + 0.25F * now;
    if (change < kStep * 2.0F) return 0.6F * before + 0.4F * now;
    return now;
}

}  // namespace

void MotionTracker::start() {
    q_ = kIntoFilter;
    told_ = kIntoFilter;
    readings_ = 0;
    last_us_ = 0;
}

void MotionTracker::gyro(const Axes& rate, std::uint64_t time_us) {
    rate_ = rate;
    step(time_us);
}

void MotionTracker::accel(const Axes& pull, std::uint64_t time_us) {
    real_pull_ = pull;
    for (std::size_t i = 0; i < 3; ++i) pull_[i] = pull[i] - offset_[i];
    step(time_us);
}

void MotionTracker::take_as_rest() {
    // The present pull is what rest reads from now on: gravity straight along y.
    offset_ = {real_pull_[0], real_pull_[1] - 1.0F, real_pull_[2]};
    pull_ = {0.0F, 1.0F, 0.0F};
    const std::uint64_t at = last_us_;
    start();
    step(at);  // only notes the time: the next reading moves on from here
}

void MotionTracker::step(std::uint64_t time_us) {
    ++readings_;
    if (readings_ == 1) {
        last_us_ = time_us;
        return;
    }
    // A clock that went back counts as no time passing.
    const float seconds = time_us > last_us_ ? static_cast<float>(time_us - last_us_) / 1e6F : 0.0F;
    last_us_ = time_us;

    const float q0 = q_.w, q1 = q_.x, q2 = q_.y, q3 = q_.z;
    const float gx = rate_[0], gy = rate_[1], gz = rate_[2];
    // How the rotation changes under the rates of turn: half of q times (0, rate).
    float d0 = 0.5F * (-q1 * gx - q2 * gy - q3 * gz);
    float d1 = 0.5F * (q0 * gx + q2 * gz - q3 * gy);
    float d2 = 0.5F * (q0 * gy - q1 * gz + q3 * gx);
    float d3 = 0.5F * (q0 * gz + q1 * gy - q2 * gx);

    float ax = pull_[0], ay = pull_[1], az = pull_[2];
    const float pull_length = std::sqrt(ax * ax + ay * ay + az * az);
    if (pull_length > 0.0F) {
        ax /= pull_length;
        ay /= pull_length;
        az /= pull_length;
        // Where gravity points under q, less where the sensor says it points ...
        const float f1 = 2.0F * (q1 * q3 - q0 * q2) - ax;
        const float f2 = 2.0F * (q0 * q1 + q2 * q3) - ay;
        const float f3 = 1.0F - 2.0F * (q1 * q1 + q2 * q2) - az;
        // ... and the gradient of its square: that difference through the transposed Jacobian.
        float s0 = -2.0F * q2 * f1 + 2.0F * q1 * f2;
        float s1 = 2.0F * q3 * f1 + 2.0F * q0 * f2 - 4.0F * q1 * f3;
        float s2 = -2.0F * q0 * f1 + 2.0F * q3 * f2 - 4.0F * q2 * f3;
        float s3 = 2.0F * q1 * f1 + 2.0F * q2 * f2;
        // The step is the gradient's direction at a fixed size, so a gradient that is only rounding
        // (gravity already where it should be) would be blown up into a full step in a direction
        // the rounding picks: at rest that shook the rotation by a degree. Under this size the tilt
        // is right to within a hundred-thousandth of a radian: no step.
        constexpr float kRoundingOnly = 1e-5F;
        const float s_length = std::sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
        if (s_length > kRoundingOnly) {
            const float gain = (readings_ < kFirstReadings ? kFirstGain : kGain) / s_length;
            d0 -= gain * s0;
            d1 -= gain * s1;
            d2 -= gain * s2;
            d3 -= gain * s3;
        }
    }

    float n0 = q0 + d0 * seconds, n1 = q1 + d1 * seconds, n2 = q2 + d2 * seconds, n3 = q3 + d3 * seconds;
    const float length = std::sqrt(n0 * n0 + n1 * n1 + n2 * n2 + n3 * n3);
    if (!(length > 0.0F) || !std::isfinite(length)) return;  // nothing sensible came of it: keep the last
    n0 /= length;
    n1 /= length;
    n2 /= length;
    n3 /= length;
    q_ = {n1, n2, n3, n0};
    // Jitter is held back only in what the console is told, never in the filter itself: held back
    // inside it, a slow turn (under about 1.4 radians a second at 1000 readings a second) never
    // moves the rotation at all, which is what the reference does.
    told_ = {steadied(n1, told_.x), steadied(n2, told_.y), steadied(n3, told_.z), steadied(n0, told_.w)};
}

Rotation MotionTracker::rotation() const { return compose(kOutOfFilter, told_); }

}  // namespace psvp::proto
