// Erasure repair: our code against the reference's (which uses the jerasure library).
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <chiaki/fec.h>

#include "oracle.h"
#include "proto/erasure.h"

namespace psvp::oracle {

Outcome diff_erasure(bool plant) {
    Outcome outcome;
    Random random(0x65726173'75726521ULL);
    bool plant_repair = plant;
    bool plant_encode = plant;
    const auto compare = [&](bool equal, const char* what) {
        ++outcome.compared;
        if (!equal && ++outcome.mismatches <= 8) std::fprintf(stderr, "    MISMATCH erasure %s\n", what);
    };
    int repaired = 0;
    int refused = 0;
    for (int round = 0; round < 4000; ++round) {
        const unsigned k = 1 + random.below(round % 8 == 0 ? 200 : 30);
        const unsigned m = 1 + random.below(round % 8 == 0 ? 50 : 8);
        const std::size_t unit = 1 + random.below(300);
        const std::size_t stride = (unit + 15) / 16 * 16;
        std::vector<std::uint8_t> frame((k + m) * stride);
        for (std::size_t i = 0; i < k * stride; ++i) frame[i] = static_cast<std::uint8_t>(random.below(256));

        // The repair units: ours, and the reference's (which writes them unit_size apart behind the source units).
        std::vector<std::uint8_t> ours = frame;
        const bool made = proto::make_repair_units(ours, unit, stride, k, m);
        if (std::exchange(plant_encode, false)) ours[k * stride] ^= 1;
        std::vector<std::uint8_t> packed((k + m) * unit);
        for (unsigned s = 0; s < k; ++s) std::memcpy(packed.data() + s * unit, frame.data() + s * stride, unit);
        const bool theirs_made = chiaki_fec_encode(packed.data(), unit, unit, k, m) == CHIAKI_ERR_SUCCESS;
        bool same = made && theirs_made;
        for (unsigned r = 0; same && r < m; ++r) same = std::memcmp(ours.data() + (k + r) * stride, packed.data() + (k + r) * unit, unit) == 0;
        compare(same, "repair units");
        if (!same) continue;

        // Lose some units, more than can be repaired now and then, and restore them both ways.
        const unsigned lose = random.below(m + 2);
        std::vector<unsigned> missing;
        for (unsigned i = 0; i < k + m && missing.size() < lose; ++i) {
            if (random.below(k + m) <= lose) missing.push_back(i);
        }
        std::vector<std::uint8_t> damaged = ours;
        for (const unsigned index : missing) std::memset(damaged.data() + index * stride, 0, unit);
        std::vector<std::uint8_t> restored = damaged;
        const bool mine = proto::repair_units(restored, unit, stride, k, m, missing);
        if (missing.size() > m) {
            // The reference is not asked: its library reads past its tables for this many erasures.
            compare(!mine && restored == damaged, "too many missing units are refused");
            ++refused;
            continue;
        }
        if (mine && !missing.empty() && missing.front() < k && std::exchange(plant_repair, false)) restored[missing.front() * stride] ^= 1;
        std::vector<std::uint8_t> reference = damaged;
        const bool theirs = chiaki_fec_decode(reference.data(), unit, stride, k, m, missing.data(), missing.size()) == CHIAKI_ERR_SUCCESS;
        same = mine && theirs;
        for (unsigned s = 0; same && s < k; ++s) {
            same = std::memcmp(restored.data() + s * stride, reference.data() + s * stride, unit) == 0 &&
                   std::memcmp(restored.data() + s * stride, ours.data() + s * stride, unit) == 0;
        }
        compare(same, "repaired units");
        ++repaired;
    }
    std::fprintf(stderr, "  erasure: %d comparisons, %d mismatches (%d frames repaired, %d refused)\n", outcome.compared, outcome.mismatches,
                 repaired, refused);
    return outcome;
}

}  // namespace psvp::oracle
