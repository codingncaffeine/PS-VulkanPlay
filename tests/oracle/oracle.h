// Differential tests: our code and the reference implementation get the same
// inputs and must agree, or the difference must be one we chose and can name.
#pragma once

#include <cstdint>

namespace psvp::oracle {

// A fixed-seed generator of our own, so every run and every platform sees the same inputs.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    // A value in [0, bound).
    std::uint32_t below(std::uint32_t bound) { return static_cast<std::uint32_t>(next() % bound); }

private:
    std::uint64_t state_;
};

struct Outcome {
    int compared = 0;
    int mismatches = 0;
};

// `plant` corrupts one of our results on purpose: the comparison must then report a mismatch.
Outcome diff_base64(bool plant);
Outcome diff_discovery();
Outcome diff_regist();

// With --vectors: results computed by the reference for fixed inputs, printed
// as C++ for the unit tests, which then hold them on machines without the reference.
void print_regist_vectors();

}  // namespace psvp::oracle
