#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <chiaki/base64.h>

#include "oracle.h"
#include "proto/base64.h"

namespace psvp::oracle {
namespace {

std::string reference_encode(const std::vector<std::uint8_t>& bytes) {
    std::vector<char> out((bytes.size() + 2) / 3 * 4 + 1);
    if (chiaki_base64_encode(bytes.data(), bytes.size(), out.data(), out.size()) != CHIAKI_ERR_SUCCESS) return "?";
    return std::string(out.data());
}

std::optional<std::vector<std::uint8_t>> reference_decode(std::string_view text) {
    std::vector<std::uint8_t> out(text.size() + 4);
    std::size_t size = out.size();
    if (chiaki_base64_decode(text.data(), text.size(), out.data(), &size) != CHIAKI_ERR_SUCCESS) return std::nullopt;
    out.resize(size);
    return out;
}

}  // namespace

Outcome diff_base64(bool plant) {
    Outcome outcome;
    Random random(0x70737670'62363421ULL);
    int both_accept = 0;
    int both_refuse = 0;
    int stricter[4] = {0, 0, 0, 0};  // by proto::Base64Error

    const auto mismatch = [&](const char* what, std::size_t length) {
        if (++outcome.mismatches <= 5) std::fprintf(stderr, "    MISMATCH base64 %s (input of %zu)\n", what, length);
    };

    for (int round = 0; round < 20000; ++round) {
        std::vector<std::uint8_t> bytes(random.below(97));
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random.below(256));

        // Encoding: the same text, character for character.
        std::string ours = proto::base64_encode(bytes);
        if (plant && round == 12345 && !ours.empty()) ours[0] = static_cast<char>(ours[0] ^ 0x01);
        const std::string reference = reference_encode(bytes);
        ++outcome.compared;
        if (ours != reference) mismatch("encode", bytes.size());

        // Decoding what the other side produced: the same bytes.
        const auto ours_back = proto::base64_decode(reference);
        const auto reference_back = reference_decode(ours);
        ++outcome.compared;
        if (!ours_back || *ours_back != bytes || (!plant && (!reference_back || *reference_back != bytes))) {
            mismatch("decode", bytes.size());
        }

        // One printable character changed. The reference reads bytes above 0x7f
        // outside its lookup table, so it is never given any.
        if (reference.empty()) continue;
        std::string changed = reference;
        changed[random.below(static_cast<std::uint32_t>(changed.size()))] =
            static_cast<char>(0x20 + random.below(0x5f));
        const auto ours_changed = proto::base64_decode(changed);
        const auto reference_changed = reference_decode(changed);
        ++outcome.compared;
        if (ours_changed) {
            // Whatever we accept, the reference must read the same way.
            if (!reference_changed || *reference_changed != *ours_changed) mismatch("changed text", changed.size());
            else ++both_accept;
        } else if (reference_changed) {
            // We refuse more than the reference does; each such case is counted under our reason.
            ++stricter[static_cast<int>(ours_changed.error())];
        } else {
            ++both_refuse;
        }
    }

    std::fprintf(stderr,
                 "  base64: %d comparisons, %d mismatches; changed text: %d read alike, %d refused by both,\n"
                 "          %d refused by us alone (length %d, character %d, padding %d, loose bits %d)\n",
                 outcome.compared, outcome.mismatches, both_accept, both_refuse,
                 stricter[0] + stricter[1] + stricter[2] + stricter[3], stricter[0], stricter[1], stricter[2],
                 stricter[3]);
    return outcome;
}

}  // namespace psvp::oracle
