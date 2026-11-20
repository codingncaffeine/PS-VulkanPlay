#include <cstdint>
#include <string>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/input.h"

using namespace psvp::proto;
using namespace psvp::proto::transport;
using namespace std::chrono_literals;

namespace {

std::string hex(std::span<const std::uint8_t> bytes) { return hex_encode(bytes); }
std::string hex(const HistoryEvent& event) { return hex_encode(event.data()); }
const TimePoint kT0{};

// The events of a packet as words, in its order: newest first.
std::vector<std::string> words(const InputHistory::Packet& packet) {
    std::vector<std::string> out;
    const auto events = parse_history_events(packet.payload);
    CHECK(events.has_value());
    if (!events) return out;
    CHECK(events->size() == packet.events);
    for (const ReadEvent& event : *events) out.push_back(describe(event));
    return out;
}

}  // namespace

// Byte for byte as docs/protocol/input.md lists them.
PSVP_TEST(input_events_are_exact) {
    // Buttons whose state is a third byte.
    CHECK(hex(button_event(Button::cross, true)) == "8088ff" && hex(button_event(Button::cross, false)) == "808800");
    CHECK(hex(button_event(Button::circle, true)) == "8089ff" && hex(button_event(Button::square, true)) == "808aff");
    CHECK(hex(button_event(Button::triangle, true)) == "808bff");
    CHECK(hex(button_event(Button::up, true)) == "8080ff" && hex(button_event(Button::down, true)) == "8081ff");
    CHECK(hex(button_event(Button::left, true)) == "8082ff" && hex(button_event(Button::right, false)) == "808300");
    CHECK(hex(button_event(Button::l1, true)) == "8084ff" && hex(button_event(Button::r1, true)) == "8085ff");
    // Buttons whose state is in the code: pressed is 0x20 more than released.
    CHECK(hex(button_event(Button::options, false)) == "808c" && hex(button_event(Button::options, true)) == "80ac");
    CHECK(hex(button_event(Button::share, false)) == "808d" && hex(button_event(Button::share, true)) == "80ad");
    CHECK(hex(button_event(Button::ps, false)) == "808e" && hex(button_event(Button::ps, true)) == "80ae");
    CHECK(hex(button_event(Button::l3, false)) == "808f" && hex(button_event(Button::l3, true)) == "80af");
    CHECK(hex(button_event(Button::r3, false)) == "8090" && hex(button_event(Button::r3, true)) == "80b0");
    CHECK(hex(button_event(Button::touchpad, false)) == "8091" && hex(button_event(Button::touchpad, true)) == "80b1");
    // The triggers carry how far they are pulled.
    CHECK(hex(trigger_event(false, 0x40)) == "808640" && hex(trigger_event(true, 0xff)) == "8087ff");
    // A touch: its number, then x and y in twelve bits each.
    CHECK(hex(touch_event(Touch{true, 5, 0x123, 0x456})) == "d005123456");
    CHECK(hex(touch_event(Touch{false, 0x7f, 0xfff, 0x001})) == "c07ffff001");
    // The packet around the events: kind 1, the newest event's number, the number of events less one.
    const std::vector<std::uint8_t> payload{0xaa, 0xbb};
    CHECK(hex(encode_history_packet(0x0102, 3, payload)) == "01" "0102" "02" "00000000" "00000000" "aabb");
}

PSVP_TEST(input_events_read_back) {
    std::vector<std::uint8_t> payload;
    std::vector<ReadEvent> expected;
    const auto add = [&](const HistoryEvent& event, const ReadEvent& read) {
        payload.insert(payload.end(), event.data().begin(), event.data().end());
        expected.push_back(read);
    };
    for (std::size_t i = 0; i < kButtons; ++i) {
        const auto button = static_cast<Button>(i);
        CHECK(button_named(button_name(button)) == button);
        for (const bool pressed : {true, false}) add(button_event(button, pressed), ReadEvent{ReadEvent::What::button, button, pressed, false, 0, {}});
    }
    add(trigger_event(false, 7), ReadEvent{ReadEvent::What::trigger, Button::cross, false, false, 7, {}});
    add(trigger_event(true, 200), ReadEvent{ReadEvent::What::trigger, Button::cross, false, true, 200, {}});
    add(touch_event(Touch{true, 3, 1919, 1079}), ReadEvent{ReadEvent::What::touch, Button::cross, false, false, 0, Touch{true, 3, 1919, 1079}});
    add(touch_event(Touch{false, 3, 0, 0}), ReadEvent{ReadEvent::What::touch, Button::cross, false, false, 0, Touch{false, 3, 0, 0}});
    CHECK(parse_history_events(payload) == expected);
    CHECK(describe(expected[0]) == "cross pressed" && describe(expected[1]) == "cross released");
    CHECK(describe(expected[32]) == "l2 at 7" && describe(expected[34]) == "touch 3 down at 1919,1079");
    CHECK(parse_history_events({}).value().empty());
    CHECK(!button_named("start"));
    // Not events: an unknown first byte, an unknown code, a state that is neither pressed nor released, a cut-off event.
    for (const char* bad : {"70", "8092ff", "80", "80887f", "8088", "d0051234", "8086", "c080000000"}) {
        CHECK(!parse_history_events(hex_decode(bad).value()));
    }
}

PSVP_TEST(input_history_orders_and_repeats_events) {
    InputHistory history;
    Buttons before;
    Buttons now;
    now.set(Button::cross, true);
    now.l2 = 40;
    auto packets = history.on_change(before, now, kT0);
    // One packet per event; each carries the events before it again, newest first.
    CHECK(packets.size() == 2 && packets[0].sequence == 0 && packets[1].sequence == 1);
    if (packets.size() != 2) return;
    CHECK((words(packets[0]) == std::vector<std::string>{"cross pressed"}));
    CHECK((words(packets[1]) == std::vector<std::string>{"l2 at 40", "cross pressed"}));
    CHECK(hex(packets[1].payload) == "808628" "8088ff");

    // Everything at once: releases first, then touches, then presses, then the triggers.
    before = now;
    now.set(Button::cross, false);
    now.set(Button::circle, true);
    now.set(Button::ps, true);
    now.touches[0] = Touch{true, 1, 100, 200};
    now.l2 = 0;
    now.r2 = 255;
    packets = history.on_change(before, now, kT0 + 1ms);
    CHECK(packets.size() == 6 && packets.back().sequence == 7);
    if (packets.size() != 6) return;
    CHECK((words(packets.back()) == std::vector<std::string>{"r2 at 255", "l2 at 0", "ps pressed", "circle pressed", "touch 1 down at 100,200",
                                                             "cross released", "l2 at 40", "cross pressed"}));
    // Nothing changed: nothing is sent.
    CHECK(history.on_change(now, now, kT0 + 2ms).empty());

    // A finger that moves is told as down again; one that is replaced by another is told as lifted, and
    // the new one with its next change.
    before = now;
    now.touches[0] = Touch{true, 1, 101, 200};
    packets = history.on_change(before, now, kT0 + 3ms);
    CHECK(packets.size() == 1 && words(packets[0]).front() == "touch 1 down at 101,200");
    before = now;
    now.touches[0] = Touch{true, 2, 500, 500};
    packets = history.on_change(before, now, kT0 + 4ms);
    CHECK(packets.size() == 1 && words(packets[0]).front() == "touch 1 up at 101,200");
    // That packet is the tenth event's: it carries the eight kept from before and the new one.
    CHECK(packets[0].sequence == 9 && packets[0].events == 9);
}

PSVP_TEST(input_history_carries_at_most_nine_events) {
    InputHistory history;
    Buttons state;
    for (unsigned i = 0; i < 20; ++i) {
        Buttons next = state;
        next.set(Button::cross, !state.is_pressed(Button::cross));
        const auto packets = history.on_change(state, next, kT0);
        state = next;
        CHECK(packets.size() == 1);
        if (packets.size() != 1) return;
        // Eight events are kept after every packet; the next packet adds its own.
        CHECK(packets[0].sequence == i && packets[0].events == std::min(i + 1, 9U));
        CHECK(words(packets[0]).front() == (state.is_pressed(Button::cross) ? "cross pressed" : "cross released"));
    }
}

PSVP_TEST(input_history_sends_its_last_packet_again) {
    InputHistory history;
    CHECK(!history.deadline() && !history.on_timeout(kT0 + 1s));
    Buttons before;
    Buttons now;
    now.set(Button::ps, true);
    const auto packets = history.on_change(before, now, kT0);
    CHECK(packets.size() == 1);
    // 20, 50, 100 and 200 ms after it was first sent, then no more.
    for (const auto after : {20ms, 50ms, 100ms, 200ms}) {
        CHECK(history.deadline() == kT0 + after);
        CHECK(!history.on_timeout(kT0 + after - 1ms));
        const auto again = history.on_timeout(kT0 + after);
        CHECK(again && again->sequence == 0 && again->payload == packets[0].payload);
    }
    CHECK(!history.deadline() && !history.on_timeout(kT0 + 10s));
    // A new event starts the row again, from its own time.
    before = now;
    now.set(Button::ps, false);
    CHECK(history.on_change(before, now, kT0 + 1s).size() == 1);
    CHECK(history.deadline() == kT0 + 1s + 20ms);
    const auto again = history.on_timeout(kT0 + 1s + 30ms);
    CHECK(again && again->sequence == 1 && history.deadline() == kT0 + 1s + 50ms);
}
