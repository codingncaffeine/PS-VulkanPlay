// psvp_oracle_tests            every comparison must agree: exit 0
// psvp_oracle_tests --plant    one of our results is corrupted on purpose: the
//                              comparison must notice, and the program exits 1
// psvp_oracle_tests --vectors  prints results computed by the reference, for the unit tests
//                              (--vectors-stream: those of the stream's set-up)
#include <cstdio>
#include <string_view>

#include "oracle.h"

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (mode == "--vectors") {
        psvp::oracle::print_regist_vectors();
        return 0;
    }
    if (mode == "--vectors-stream") {
        psvp::oracle::print_stream_vectors();
        return 0;
    }
    const bool plant = mode == "--plant";

    int compared = 0;
    int mismatches = 0;
    const auto add = [&](const psvp::oracle::Outcome& outcome) {
        compared += outcome.compared;
        mismatches += outcome.mismatches;
    };
    add(psvp::oracle::diff_base64(plant));
    add(psvp::oracle::diff_stream(plant));
    if (!plant) {
        add(psvp::oracle::diff_discovery());
        add(psvp::oracle::diff_regist());
    }
    std::fprintf(stderr, "oracle: %d comparisons, %d mismatches%s\n", compared, mismatches,
                 plant ? " (planted on purpose)" : "");
    if (compared == 0) return 2;
    return mismatches == 0 ? 0 : 1;
}
