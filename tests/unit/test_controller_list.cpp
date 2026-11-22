#include <string>
#include <vector>

#include "app/controller_list.h"
#include "check.h"

using namespace psvp::app;

PSVP_TEST(controller_kinds_are_told_from_their_numbers_and_what_they_were_taken_for) {
    using Kind = ControllerFacts::Kind;
    CHECK(controller_kind(0x054c, 0x0df2, true, false, false, false) == Kind::dualsense_edge);
    CHECK(controller_kind(0x054c, 0x0ce6, true, false, false, false) == Kind::dualsense);
    // An Edge whose numbers a wireless link does not pass on is still a DualSense to the console.
    CHECK(controller_kind(0, 0, true, false, false, false) == Kind::dualsense);
    CHECK(controller_kind(0x054c, 0x09cc, false, true, false, false) == Kind::dualshock4);
    CHECK(controller_kind(0x045e, 0x0b12, false, false, true, false) == Kind::xbox);
    CHECK(controller_kind(0x057e, 0x2009, false, false, false, true) == Kind::nintendo);
    CHECK(controller_kind(0x1234, 0x5678, false, false, false, false) == Kind::other);
    // Another maker's controller that only shares a product number is not an Edge.
    CHECK(controller_kind(0x1234, 0x0df2, false, false, false, false) == Kind::other);
}

PSVP_TEST(a_controller_is_said_in_one_line) {
    ControllerFacts edge;
    edge.name = "DualSense Edge Wireless Controller";
    edge.kind = ControllerFacts::Kind::dualsense_edge;
    edge.link = ControllerFacts::Link::cable;
    edge.power = ControllerFacts::Power::charging;
    edge.battery = 55;
    edge.haptics = ControllerFacts::Haptics::sound_device;
    CHECK(controller_line(edge) == "DualSense Edge Wireless Controller: on a cable, 55%, charging; haptics through its own sound device");
    edge.link = ControllerFacts::Link::wireless;
    edge.power = ControllerFacts::Power::on_battery;
    edge.haptics = ControllerFacts::Haptics::rumble;
    CHECK(controller_line(edge) == "DualSense Edge Wireless Controller: wireless, 55%; haptics as rumble");
    ControllerFacts bare;
    CHECK(controller_line(bare) == "A controller");
    bare.name = "Some Pad";
    bare.power = ControllerFacts::Power::on_battery;
    CHECK(controller_line(bare) == "Some Pad: on its battery");
}

PSVP_TEST(controllers_are_listed_as_json_on_one_line) {
    CHECK(controllers_json({}) == R"({"controllers": []})");
    ControllerFacts pad;
    pad.name = "A \"quoted\" pad\nwith a break";
    pad.kind = ControllerFacts::Kind::dualshock4;
    pad.link = ControllerFacts::Link::wireless;
    pad.power = ControllerFacts::Power::on_battery;
    pad.battery = 80;
    pad.touchpad = pad.motion = pad.rumble = true;
    const std::string text = controllers_json({pad});
    CHECK(text.find('\n') == std::string::npos);
    const auto read = Json::parse(text);
    CHECK(read.has_value());
    if (!read) return;
    const Json* list = read->find("controllers");
    CHECK(list != nullptr && list->as_array() != nullptr && list->as_array()->size() == 1);
    if (list == nullptr || list->as_array() == nullptr || list->as_array()->empty()) return;
    const Json& one = list->as_array()->front();
    CHECK(one.find("name") != nullptr && *one.find("name")->as_string() == "A \"quoted\" pad\nwith a break");
    CHECK(*one.find("kind")->as_string() == "dualshock4");
    CHECK(*one.find("link")->as_string() == "wireless");
    CHECK(*one.find("power")->as_string() == "battery");
    CHECK(*one.find("battery")->as_number() == 80.0);
    CHECK(*one.find("touchpad")->as_bool() && *one.find("motion")->as_bool() && *one.find("rumble")->as_bool());
    CHECK(*one.find("haptics")->as_string() == "none");
    CHECK(one.find("through_steam") != nullptr && !*one.find("through_steam")->as_bool());
}

// Steam's virtual controller is known by its numbers, and said.
PSVP_TEST(controllers_behind_steam_input_are_said) {
    CHECK(is_steam_virtual(0x28de, 0x11ff));
    CHECK(!is_steam_virtual(0x28de, 0x1205));  // a Steam Deck's own controls are not Steam Input
    CHECK(!is_steam_virtual(0x054c, 0x0df2));
    ControllerFacts pad;
    pad.name = "Steam Virtual Gamepad";
    pad.through_steam = true;
    CHECK(controller_line(pad) == "Steam Virtual Gamepad; through Steam Input");
    const auto read = Json::parse(controllers_json({pad}));
    CHECK(read.has_value() && *read->find("controllers")->as_array()->front().find("through_steam")->as_bool());
}
