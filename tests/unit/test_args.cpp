#include <string_view>
#include <vector>

#include "app/args.h"
#include "check.h"

namespace {

auto parse(std::vector<std::string_view> args) { return psvp::app::parse_args(args); }

}  // namespace

PSVP_TEST(args_default_to_a_normal_start) {
    const auto options = parse({});
    CHECK(options.has_value());
    if (!options) return;
    CHECK(!options->show_version);
    CHECK(!options->show_help);
    CHECK(!options->verbose);
    CHECK(!options->frames.has_value());
}

PSVP_TEST(args_read_each_option) {
    CHECK(parse({"--version"}).value().show_version);
    CHECK(parse({"--help"}).value().show_help);
    CHECK(parse({"-h"}).value().show_help);
    CHECK(parse({"--verbose"}).value().verbose);
    const auto frames = parse({"--verbose", "--frames", "30"});
    CHECK(frames.has_value() && frames->verbose && frames->frames == 30U);
    const auto clip = parse({"--test-clip"});
    CHECK(clip.has_value() && clip->test_clip && !clip->test_clip_frame);
    const auto still = parse({"--test-clip-frame", "23130"});
    CHECK(still.has_value() && still->test_clip && still->test_clip_frame == 23130U);
    CHECK(!parse({"--test-clip-frame"}).has_value());
    CHECK(!parse({"--test-clip-frame", "65536"}).has_value());
    CHECK(!parse({"--test-clip-frame", "x"}).has_value());
}

PSVP_TEST(args_refuse_a_bad_frame_count) {
    CHECK(!parse({"--frames"}).has_value());
    CHECK(!parse({"--frames", "0"}).has_value());
    CHECK(!parse({"--frames", "-3"}).has_value());
    CHECK(!parse({"--frames", "ten"}).has_value());
    CHECK(!parse({"--frames", "30x"}).has_value());
    CHECK(!parse({"--frames", ""}).has_value());
}

PSVP_TEST(args_name_an_unknown_option) {
    const auto options = parse({"--nonsense"});
    CHECK(!options.has_value());
    if (options) return;
    CHECK(options.error().find("--nonsense") != std::string::npos);
}

PSVP_TEST(args_help_lists_every_option) {
    const std::string help = psvp::app::help_text();
    for (const char* option : {"--version", "--help", "--verbose", "--frames", "--test-clip", "--test-clip-frame"}) {
        CHECK(help.find(option) != std::string::npos);
    }
}
