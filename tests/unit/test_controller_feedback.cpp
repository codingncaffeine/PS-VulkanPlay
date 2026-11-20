#include <cstdint>
#include <vector>

#include "check.h"
#include "proto/controller_feedback.h"

using namespace psvp::proto::transport;

namespace {
using Bytes = std::vector<std::uint8_t>;
}

PSVP_TEST(rumble_is_three_bytes) {
    const auto rumble = parse_rumble(Bytes{0x01, 0xc8, 0x5a});
    CHECK(rumble && rumble->first == 1 && rumble->left == 200 && rumble->right == 90);
    // More bytes than three are not looked at; fewer are not rumble.
    CHECK(parse_rumble(Bytes{0, 7, 9, 0xff, 0xff}) == (Rumble{0, 7, 9}));
    CHECK(!parse_rumble(Bytes{0, 7}));
    CHECK(!parse_rumble({}));
    CHECK(encode_rumble(Rumble{1, 200, 90}) == (Bytes{0x01, 0xc8, 0x5a}));
}

PSVP_TEST(trigger_effects_are_a_type_and_ten_bytes_each) {
    Bytes content(25, 0);
    content[1] = 0x21;  // the left trigger's kind of effect
    content[2] = 0x26;  // the right one's
    for (std::uint8_t i = 0; i < 10; ++i) {
        content[5 + i] = static_cast<std::uint8_t>(0xa0 + i);
        content[15 + i] = static_cast<std::uint8_t>(0xb0 + i);
    }
    const auto effects = parse_trigger_effects(content);
    CHECK(effects.has_value());
    if (!effects) return;
    CHECK(effects->left_type == 0x21 && effects->right_type == 0x26);
    CHECK(effects->left[0] == 0xa0 && effects->left[9] == 0xa9 && effects->right[0] == 0xb0 && effects->right[9] == 0xb9);
    CHECK(encode_trigger_effects(*effects) == content);
    content.pop_back();
    CHECK(!parse_trigger_effects(content));
}

PSVP_TEST(controller_information_comes_in_two_sizes) {
    // The short form, as a PS5 was seen to send it when a stream was set up: no player's lights, a
    // dark light bar, "take this position as rest", haptics and trigger effects at full strength.
    const Bytes seen{0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0};
    const auto short_form = parse_controller_info(seen);
    CHECK(short_form.has_value());
    if (!short_form) return;
    CHECK(short_form->player == 0 && short_form->light == (std::array<std::uint8_t, 3>{0, 0, 0}) && short_form->reset_motion);
    CHECK(short_form->haptics == static_cast<std::uint8_t>(Strength::strong) && short_form->triggers == static_cast<std::uint8_t>(Strength::strong));
    CHECK(!short_form->answers);

    // The long form has eight bytes in front: the input state it answers, and a time.
    Bytes longer{0x12, 0x34, 0, 0, 0, 0, 0, 9};
    const Bytes rest{3, 10, 20, 30, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 0, 0, 0};
    longer.insert(longer.end(), rest.begin(), rest.end());
    const auto long_form = parse_controller_info(longer);
    CHECK(long_form.has_value());
    if (!long_form) return;
    CHECK(long_form->answers == 0x1234 && long_form->player == 3 && long_form->light == (std::array<std::uint8_t, 3>{10, 20, 30}));
    CHECK(!long_form->reset_motion && long_form->haptics == 2 && long_form->triggers == 3);

    // Written and read again, in the form that fits.
    CHECK(encode_controller_info(*long_form).size() == 25 && parse_controller_info(encode_controller_info(*long_form)) == *long_form);
    ControllerInfo plain = *long_form;
    plain.answers.reset();
    CHECK(encode_controller_info(plain) == rest);
    // Any other size is not controller information.
    for (std::size_t size = 0; size < 40; ++size) {
        if (size != 17 && size != 25) CHECK(!parse_controller_info(Bytes(size, 1)));
    }
}
