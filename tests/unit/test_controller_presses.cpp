#include <string>
#include <vector>

#include "app/controller_presses.h"
#include "check.h"

using namespace psvp::app;

namespace {

using Presses = std::vector<Press>;

}  // namespace

PSVP_TEST(a_press_is_said_as_json_on_one_line) {
    CHECK(press_json(Press::down, 0) == R"({"press":"down","controller":0})");
    CHECK(press_json(Press::previous_page, 2) == R"({"press":"previous","controller":2})");
    CHECK(std::string(press_text(Press::up)) == "up");
    CHECK(std::string(press_text(Press::left)) == "left");
    CHECK(std::string(press_text(Press::right)) == "right");
    CHECK(std::string(press_text(Press::accept)) == "accept");
    CHECK(std::string(press_text(Press::back)) == "back");
    CHECK(std::string(press_text(Press::next_page)) == "next");
}

PSVP_TEST(a_button_says_its_press_once_when_it_goes_down) {
    PressReader reader;
    Presses out;
    reader.button(Press::accept, true, 1000, out);
    CHECK(out == Presses{Press::accept});
    // Held, said down again by the input library, let go: nothing more.
    reader.button(Press::accept, true, 1100, out);
    reader.tick(5000, out);
    reader.button(Press::accept, false, 5100, out);
    CHECK(out == Presses{Press::accept});
    CHECK(!reader.next_repeat().has_value());
    reader.button(Press::accept, true, 5200, out);
    reader.button(Press::back, true, 5210, out);
    reader.button(Press::next_page, true, 5220, out);
    reader.button(Press::previous_page, true, 5230, out);
    CHECK((out == Presses{Press::accept, Press::accept, Press::back, Press::next_page, Press::previous_page}));
}

PSVP_TEST(a_held_direction_is_said_again_after_a_moment_and_then_steadily) {
    PressReader reader;
    Presses out;
    reader.button(Press::down, true, 1000, out);
    CHECK(out == Presses{Press::down});
    CHECK(reader.next_repeat() == 1000 + PressReader::kFirstRepeatMs);
    reader.tick(1399, out);
    CHECK(out.size() == 1);
    reader.tick(1400, out);
    CHECK(out.size() == 2);
    CHECK(reader.next_repeat() == 1400 + PressReader::kRepeatMs);
    reader.tick(1509, out);
    CHECK(out.size() == 2);
    reader.tick(1510, out);
    reader.tick(1620, out);
    CHECK((out == Presses{Press::down, Press::down, Press::down, Press::down}));
    reader.button(Press::down, false, 1650, out);
    CHECK(!reader.next_repeat().has_value());
    reader.tick(9000, out);
    CHECK(out.size() == 4);
}

PSVP_TEST(a_reader_that_was_held_up_does_not_make_up_for_it_in_a_burst) {
    PressReader reader;
    Presses out;
    reader.button(Press::right, true, 0, out);
    // Three seconds without a tick: one press, and the next one a little later, not twenty at once.
    reader.tick(3000, out);
    CHECK((out == Presses{Press::right, Press::right}));
    const auto next = reader.next_repeat();
    CHECK(next.has_value() && *next > 3000 && *next <= 3000 + PressReader::kRepeatMs);
    reader.tick(3000, out);
    CHECK(out.size() == 2);
}

PSVP_TEST(the_direction_button_pressed_last_is_the_one_that_counts) {
    PressReader reader;
    Presses out;
    reader.button(Press::down, true, 0, out);
    reader.button(Press::right, true, 100, out);
    CHECK((out == Presses{Press::down, Press::right}));
    reader.tick(100 + PressReader::kFirstRepeatMs, out);
    CHECK(out.back() == Press::right && out.size() == 3);
    // Right comes up while down is still held: down again, as a new press.
    reader.button(Press::right, false, 600, out);
    CHECK(out.back() == Press::down && out.size() == 4);
    CHECK(reader.next_repeat() == 600 + PressReader::kFirstRepeatMs);
    reader.button(Press::down, false, 700, out);
    CHECK(out.size() == 4 && !reader.next_repeat().has_value());
}

PSVP_TEST(the_left_stick_is_a_direction_past_most_of_its_throw_and_until_nearly_back) {
    PressReader reader;
    Presses out;
    reader.stick(0, PressReader::kStickOn - 1, 0, out);
    CHECK(out.empty());
    reader.stick(0, PressReader::kStickOn, 10, out);
    CHECK(out == Presses{Press::down});
    // Wobbling between the two marks: still the one press.
    reader.stick(0, PressReader::kStickOff, 20, out);
    reader.stick(0, PressReader::kStickOn + 100, 30, out);
    CHECK(out.size() == 1);
    reader.tick(10 + PressReader::kFirstRepeatMs, out);
    CHECK((out == Presses{Press::down, Press::down}));
    reader.stick(0, PressReader::kStickOff - 1, 500, out);
    CHECK(!reader.next_repeat().has_value());
    reader.stick(0, -32768, 600, out);
    reader.stick(0, 0, 610, out);
    reader.stick(-32768, 0, 620, out);
    reader.stick(0, 0, 630, out);
    reader.stick(32767, 0, 640, out);
    CHECK((out == Presses{Press::down, Press::down, Press::up, Press::left, Press::right}));
}

PSVP_TEST(a_stick_held_between_two_directions_does_not_flutter) {
    PressReader reader;
    Presses out;
    reader.stick(24000, 23000, 0, out);
    CHECK(out == Presses{Press::right});
    // The other axis a little ahead, then behind again: it stays what it was.
    reader.stick(23000, 24000, 10, out);
    reader.stick(24000, 23000, 20, out);
    reader.stick(22000, 26000, 30, out);
    CHECK(out.size() == 1);
    // Pushed clearly another way without passing the middle: that way.
    reader.stick(9000, 30000, 40, out);
    CHECK((out == Presses{Press::right, Press::down}));
    // From one side straight to the other.
    reader.stick(0, -30000, 50, out);
    CHECK((out == Presses{Press::right, Press::down, Press::up}));
}

PSVP_TEST(the_direction_buttons_win_over_the_stick_while_one_is_held) {
    PressReader reader;
    Presses out;
    reader.stick(-32768, 0, 0, out);
    reader.button(Press::up, true, 100, out);
    CHECK((out == Presses{Press::left, Press::up}));
    reader.stick(32767, 0, 200, out);   // the stick moves meanwhile: the button still counts
    CHECK(out.size() == 2);
    reader.button(Press::up, false, 300, out);
    CHECK((out == Presses{Press::left, Press::up, Press::right}));
}
