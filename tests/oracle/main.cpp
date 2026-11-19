// psvp_oracle_tests          every comparison must agree: exit 0
// psvp_oracle_tests --plant  one of our results is corrupted on purpose: the
//                            comparison must notice, and the program exits 1
#include <cstdio>
#include <string_view>

#include "oracle.h"

int main(int argc, char** argv) {
    const bool plant = argc > 1 && std::string_view(argv[1]) == "--plant";
    const psvp::oracle::Outcome base64 = psvp::oracle::diff_base64(plant);

    const int compared = base64.compared;
    const int mismatches = base64.mismatches;
    std::fprintf(stderr, "oracle: %d comparisons, %d mismatches%s\n", compared, mismatches,
                 plant ? " (one planted)" : "");
    if (compared == 0) return 2;
    return mismatches == 0 ? 0 : 1;
}
