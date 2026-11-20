#include <cstdint>
#include <vector>

#include "check.h"
#include "media/haptics.h"

using namespace psvp::media;

namespace {
// A haptics frame whose thirty samples are these two values, left and right.
std::vector<std::uint8_t> steady(std::int16_t left, std::int16_t right) {
    std::vector<std::uint8_t> frame;
    for (int i = 0; i < 30; ++i) {
        for (const std::int16_t value : {left, right}) {
            frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xff));
            frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8));
        }
    }
    return frame;
}
}  // namespace

PSVP_TEST(haptics_level_is_the_largest_swing) {
    CHECK(haptics_level(steady(0, 0)) == (HapticsLevel{0, 0}));
    CHECK(haptics_level(steady(12800, -25600)) == (HapticsLevel{100, 200}));
    CHECK(haptics_level(steady(32767, -32768)) == (HapticsLevel{255, 255}));
    // One sample decides: the largest in the frame.
    auto frame = steady(100, 100);
    frame[4 * 17] = 0x00;
    frame[4 * 17 + 1] = 0x40;  // the left actuator's eighteenth sample: 16384
    CHECK(haptics_level(frame) == (HapticsLevel{128, 0}));
    // Not a haptics frame.
    frame.pop_back();
    CHECK(!haptics_level(frame));
    CHECK(!haptics_level({}));
}

PSVP_TEST(haptics_are_brought_to_the_controllers_rate) {
    HapticsUpsampler upsampler;
    // A frame at 1600 and -3200, from rest: the first sixteen samples climb there in a line, the rest stay.
    const auto first = upsampler.frame(steady(1600, -3200));
    CHECK(first.size() == 480 * 4);
    if (first.size() != 480 * 4) return;
    bool speaker_silent = true, climbs = true, stays = true;
    for (std::size_t i = 0; i < 480; ++i) {
        speaker_silent = speaker_silent && first[i * 4] == 0 && first[i * 4 + 1] == 0;
        if (i < 16) climbs = climbs && first[i * 4 + 2] == static_cast<std::int16_t>(100 * (i + 1)) && first[i * 4 + 3] == static_cast<std::int16_t>(-200 * static_cast<int>(i + 1));
        else stays = stays && first[i * 4 + 2] == 1600 && first[i * 4 + 3] == -3200;
    }
    CHECK(speaker_silent && climbs && stays);
    // The next frame goes on from where this one ended: no step between frames.
    const auto second = upsampler.frame(steady(1600, -3200));
    CHECK(second.size() == 480 * 4 && second[2] == 1600 && second[3] == -3200);
    const auto third = upsampler.frame(steady(0, 0));
    CHECK(third.size() == 480 * 4 && third[2] == 1500 && third[3] == -3000 && third[15 * 4 + 2] == 0 && third[479 * 4 + 3] == 0);
    // Not a haptics frame: nothing.
    CHECK(upsampler.frame(std::vector<std::uint8_t>(40, 7)).empty());
}

// The layout is the one SDL's test program documents for the controller's effect report.
PSVP_TEST(dualsense_trigger_report_puts_each_effect_in_its_place) {
    const std::array<std::uint8_t, 10> left{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, right{11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    const auto report = dualsense_trigger_effects(0x21, left, 0x26, right);
    CHECK(report.size() == 47);
    CHECK(report[0] == 0x0c && report[1] == 0);  // the report changes the two triggers and nothing else
    CHECK(report[10] == 0x26 && report[11] == 11 && report[20] == 20);  // the right trigger: its type, then its ten bytes
    CHECK(report[21] == 0x21 && report[22] == 1 && report[31] == 10);   // the left one behind it
    std::size_t others = 0;
    for (std::size_t i = 1; i < report.size(); ++i) others += (i < 10 || i > 31) && report[i] != 0 ? 1U : 0U;
    CHECK(others == 0);
    const auto off = dualsense_triggers_off();
    CHECK(off[0] == 0x0c && off[10] == 0x05 && off[21] == 0x05 && off[11] == 0 && off[22] == 0);
}
