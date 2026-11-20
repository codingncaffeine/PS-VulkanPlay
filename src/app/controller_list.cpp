#include "app/controller_list.h"

#include <format>

namespace psvp::app {

namespace {

constexpr std::uint16_t kSony = 0x054c;
constexpr std::uint16_t kDualSense = 0x0ce6;
constexpr std::uint16_t kDualSenseEdge = 0x0df2;

const char* kind_text(ControllerFacts::Kind kind) noexcept {
    switch (kind) {
        case ControllerFacts::Kind::dualsense: return "dualsense";
        case ControllerFacts::Kind::dualsense_edge: return "dualsense-edge";
        case ControllerFacts::Kind::dualshock4: return "dualshock4";
        case ControllerFacts::Kind::xbox: return "xbox";
        case ControllerFacts::Kind::nintendo: return "nintendo";
        case ControllerFacts::Kind::other: break;
    }
    return "other";
}

const char* link_text(ControllerFacts::Link link) noexcept {
    return link == ControllerFacts::Link::cable ? "cable" : link == ControllerFacts::Link::wireless ? "wireless" : "";
}

const char* power_text(ControllerFacts::Power power) noexcept {
    switch (power) {
        case ControllerFacts::Power::on_battery: return "battery";
        case ControllerFacts::Power::charging: return "charging";
        case ControllerFacts::Power::full: return "full";
        case ControllerFacts::Power::no_battery: return "none";
        case ControllerFacts::Power::unknown: break;
    }
    return "";
}

const char* haptics_text(ControllerFacts::Haptics haptics) noexcept {
    return haptics == ControllerFacts::Haptics::sound_device ? "sound-device" : haptics == ControllerFacts::Haptics::rumble ? "rumble" : "none";
}

}  // namespace

ControllerFacts::Kind controller_kind(std::uint16_t vendor, std::uint16_t product, bool sony_ps5, bool sony_ps4, bool xbox, bool nintendo) noexcept {
    if (vendor == kSony && product == kDualSenseEdge) return ControllerFacts::Kind::dualsense_edge;
    if ((vendor == kSony && product == kDualSense) || sony_ps5) return ControllerFacts::Kind::dualsense;
    if (sony_ps4) return ControllerFacts::Kind::dualshock4;
    if (xbox) return ControllerFacts::Kind::xbox;
    if (nintendo) return ControllerFacts::Kind::nintendo;
    return ControllerFacts::Kind::other;
}

std::string controller_line(const ControllerFacts& controller) {
    std::string line = controller.name.empty() ? "A controller" : controller.name;
    std::string parts;
    const auto add = [&](std::string_view part) {
        if (part.empty()) return;
        parts += parts.empty() ? ": " : ", ";
        parts += part;
    };
    add(controller.link == ControllerFacts::Link::cable ? "on a cable" : controller.link == ControllerFacts::Link::wireless ? "wireless" : "");
    if (controller.battery >= 0) add(std::format("{}%", controller.battery));
    add(controller.power == ControllerFacts::Power::charging ? "charging" : controller.power == ControllerFacts::Power::full ? "full"
        : controller.power == ControllerFacts::Power::on_battery && controller.battery < 0 ? "on its battery" : "");
    line += parts;
    if (controller.haptics == ControllerFacts::Haptics::sound_device) line += "; haptics through its own sound device";
    else if (controller.haptics == ControllerFacts::Haptics::rumble) line += "; haptics as rumble";
    return line;
}

std::string controllers_json(const std::vector<ControllerFacts>& controllers) {
    Json::Array list;
    for (const ControllerFacts& controller : controllers) {
        list.push_back(Json(Json::Object{
            {"name", Json(controller.name)},
            {"kind", Json(kind_text(controller.kind))},
            {"link", Json(link_text(controller.link))},
            {"battery", Json(static_cast<std::int64_t>(controller.battery))},
            {"power", Json(power_text(controller.power))},
            {"touchpad", Json(controller.touchpad)},
            {"motion", Json(controller.motion)},
            {"rumble", Json(controller.rumble)},
            {"haptics", Json(haptics_text(controller.haptics))},
        }));
    }
    // The writer puts every member on a line of its own. A line break never stands inside a
    // string of its (it writes them as \n), so taking the breaks and the indents out leaves the same JSON.
    const std::string spread = Json(Json::Object{{"controllers", Json(std::move(list))}}).dump();
    std::string line;
    for (std::size_t i = 0; i < spread.size(); ++i) {
        if (spread[i] != '\n') {
            line.push_back(spread[i]);
            continue;
        }
        while (i + 1 < spread.size() && spread[i + 1] == ' ') ++i;
    }
    return line;
}

}  // namespace psvp::app
