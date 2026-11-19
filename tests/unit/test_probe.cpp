#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/probe.h"

using namespace psvp::proto;
using namespace psvp::proto::transport;
using namespace std::chrono_literals;

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) { return hex_encode(bytes); }

const TimePoint kStart{};
constexpr std::uint32_t kClientTag = 0x11223344;
constexpr std::uint32_t kConsoleTag = 0x50607080;

ControlPacket packet_of(const std::vector<std::uint8_t>& datagram) { return parse_control(datagram).value(); }

// A data packet as the other end would send it.
ControlPacket data_packet(std::uint32_t tag, std::uint32_t sequence, std::uint8_t byte) {
    ControlPacket packet;
    packet.tag = tag;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = 1;
    packet.payload = encode_data_chunk(DataChunk{sequence, 1, data_type::kMessage, {byte}, 0});
    return packet;
}

ControlPacket ack_packet(std::uint32_t tag, std::uint32_t cumulative) {
    ControlPacket packet;
    packet.tag = tag;
    packet.chunk_type = chunk::kDataAck;
    packet.payload = encode_data_ack(DataAck{cumulative, kReceiveWindow, 0, 0});
    return packet;
}

// Both ends of a probe on a clock of our own, with a fixed delay per direction.
struct Link {
    ClientProbe client;
    ConsoleProbe console;
    std::chrono::microseconds hop{2000};
    // Returns true for a datagram that is to be lost. `to_console` tells the direction.
    std::function<bool(bool to_console, const std::vector<std::uint8_t>&)> lose;
    TimePoint now = kStart;
    std::vector<std::string> events;
    int steps = 0;

    explicit Link(ConsoleProbe::Path path = {})
        : client(kClientTag, Endpoint{kConsoleTag, kReceiveWindow, kStreams, kStreams, kConsoleTag},
                 [n = std::uint32_t{0xa0000000}]() mutable { return n++; }),
          console(kConsoleTag, Endpoint{kClientTag, kReceiveWindow, kStreams, kStreams, kClientTag}, path) {}

    void run() {
        struct InFlight {
            TimePoint arrives;
            std::vector<std::uint8_t> datagram;
        };
        std::deque<InFlight> to_console;
        std::deque<InFlight> to_client;
        const auto post = [&](std::deque<InFlight>& queue, bool towards_console, Datagrams datagrams) {
            for (auto& datagram : datagrams) {
                if (lose && lose(towards_console, datagram)) continue;
                queue.push_back(InFlight{now + hop, std::move(datagram)});
            }
        };
        post(to_console, true, client.start(now));
        while (client.state() == ClientProbe::State::running && ++steps < 200000) {
            // The next thing that happens: an arrival, or a timer of either end.
            TimePoint next = client.deadline();
            if (const auto timer = console.deadline(); timer && *timer < next) next = *timer;
            if (!to_console.empty() && to_console.front().arrives <= next) {
                now = to_console.front().arrives;
                auto datagram = std::move(to_console.front().datagram);
                to_console.pop_front();
                post(to_client, false, console.on_datagram(datagram, now));
            } else if (!to_client.empty() && to_client.front().arrives <= next) {
                now = to_client.front().arrives;
                auto datagram = std::move(to_client.front().datagram);
                to_client.pop_front();
                post(to_console, true, client.on_datagram(datagram, now));
            } else {
                now = next;
                post(to_client, false, console.on_timer(now));
                post(to_console, true, client.on_timeout(now));
            }
            for (auto& line : console.take_events()) events.push_back(std::move(line));
        }
        // What the client sent last (its goodbye) still arrives.
        for (auto& flight : to_console) {
            (void)console.on_datagram(flight.datagram, flight.arrives);
            for (auto& line : console.take_events()) events.push_back(std::move(line));
        }
    }

    [[nodiscard]] int count(std::string_view start) const {
        int n = 0;
        for (const std::string& line : events) n += line.starts_with(start) ? 1 : 0;
        return n;
    }
};

}  // namespace

PSVP_TEST(data_chunk_and_acknowledgment_bytes_are_exact) {
    Messenger messenger(kClientTag, kConsoleTag, kClientTag, kConsoleTag);
    const std::vector<std::uint8_t> message{0x08, 0x1f, 0xfa, 0x01, 0x02, 0x08, 0x09};
    const auto sent = messenger.send(1, 1, message, kStart);
    CHECK(sent.has_value());
    if (!sent) return;
    CHECK(sent->sequence == kClientTag);
    CHECK(sent->datagram.size() == 33);
    CHECK(hex(sent->datagram) == "00"                 // a control packet
                                 "50607080"           // the console's tag
                                 "00000000" "00000000"  // no authentication code or key position yet
                                 "00" "01" "0014"     // data, flags 1, sixteen bytes of payload plus four
                                 "11223344"           // sequence number
                                 "0001" "0000" "00"   // channel 1, two zero bytes, a message
                                 "081ffa01020809");
    const auto received = messenger.on_packet(data_packet(kClientTag, kConsoleTag, 0x55));
    CHECK(received.delivered.size() == 1);
    const auto reply = messenger.take_ack();
    CHECK(reply.has_value());
    // One acknowledgment per thing to acknowledge.
    CHECK(!messenger.take_ack());
    if (reply) {
        CHECK(hex(*reply) == "00" "50607080" "00000000" "00000000"
                                      "03" "00" "0010"  // data acknowledged, twelve bytes of payload plus four
                                      "50607080"        // everything up to this sequence number has arrived
                                      "00019000"        // receive window
                                      "0000" "0000");   // no gaps, no duplicates
    }
}

PSVP_TEST(messenger_hands_messages_over_in_order_once) {
    Messenger messenger(kClientTag, kConsoleTag, 1000, 0xfffffffe);
    // The third arrives first, across the wrap of the sequence numbers.
    const auto acknowledged = [&]() -> std::optional<std::uint32_t> {
        const auto ack = messenger.take_ack();
        if (!ack) return std::nullopt;
        return parse_data_ack(packet_of(*ack).payload)->cumulative;
    };
    auto r = messenger.on_packet(data_packet(kClientTag, 0, 3));
    CHECK(r.delivered.empty());
    CHECK(!acknowledged());
    r = messenger.on_packet(data_packet(kClientTag, 0xfffffffe, 1));
    CHECK(r.delivered.size() == 1);
    CHECK(acknowledged() == 0xfffffffe);
    r = messenger.on_packet(data_packet(kClientTag, 0xffffffff, 2));
    CHECK(r.delivered.size() == 2);
    if (r.delivered.size() == 2) CHECK(r.delivered[0].data[0] == 2 && r.delivered[1].data[0] == 3 && r.delivered[1].flags == 1);
    CHECK(acknowledged() == 0U);
    // A copy of one already handed over is not handed over again, and is acknowledged again.
    r = messenger.on_packet(data_packet(kClientTag, 0xffffffff, 2));
    CHECK(r.delivered.empty());
    CHECK(acknowledged() == 0U);
    // Too far ahead, or for another connection: nothing.
    r = messenger.on_packet(data_packet(kClientTag, 1 + Messenger::kWindow, 9));
    CHECK(r.delivered.empty() && !acknowledged());
    r = messenger.on_packet(data_packet(kClientTag + 1, 1, 9));
    CHECK(r.delivered.empty() && !acknowledged());
}

PSVP_TEST(messenger_sends_again_until_acknowledged) {
    Messenger messenger(kClientTag, kConsoleTag, 0xffffffff, 1);
    CHECK(!messenger.deadline());
    const std::vector<std::uint8_t> body{1};
    const auto first = messenger.send(1, 8, body, kStart);
    const auto second = messenger.send(1, 8, body, kStart + 50ms);
    CHECK(first && second && first->sequence == 0xffffffff && second->sequence == 0);
    CHECK(messenger.deadline() == kStart + 200ms);
    CHECK(messenger.on_timer(kStart + 199ms).resend.empty());
    auto timer = messenger.on_timer(kStart + 200ms);
    CHECK(timer.resend.size() == 1);
    if (timer.resend.size() == 1) CHECK(first && timer.resend[0] == first->datagram);
    CHECK(messenger.deadline() == kStart + 250ms);
    // An acknowledgment covers everything up to its number, across the wrap.
    const auto acked = messenger.on_packet(ack_packet(kClientTag, 0));
    CHECK(acked.acknowledged == (std::vector<std::uint32_t>{0xffffffff, 0}));
    CHECK(messenger.waiting() == 0);
    CHECK(!messenger.deadline());
    CHECK(messenger.on_timer(kStart + 10s).resend.empty());
}

PSVP_TEST(messenger_gives_up_after_its_resends) {
    Messenger messenger(kClientTag, kConsoleTag, 1, 1);
    CHECK(messenger.send(1, 8, {}, kStart).has_value());
    int resends = 0;
    TimePoint now = kStart;
    while (messenger.waiting() != 0 && resends < 100) {
        now = *messenger.deadline();
        const auto timer = messenger.on_timer(now);
        resends += static_cast<int>(timer.resend.size());
        if (!timer.abandoned.empty()) CHECK(timer.abandoned == std::vector<std::uint32_t>{1});
    }
    CHECK(resends == Messenger::kResends);
    CHECK(now == kStart + 200ms * (Messenger::kResends + 1));
}

PSVP_TEST(messenger_holds_no_more_than_its_window) {
    Messenger messenger(kClientTag, kConsoleTag, 1, 1);
    for (std::size_t i = 0; i < Messenger::kWindow; ++i) CHECK(messenger.send(1, 8, {}, kStart).has_value());
    CHECK(!messenger.send(1, 8, {}, kStart));
    (void)messenger.on_packet(ack_packet(kClientTag, 3));
    CHECK(messenger.waiting() == Messenger::kWindow - 3);
    CHECK(messenger.send(1, 8, {}, kStart).has_value());
}

PSVP_TEST(probe_ping_bytes_are_exact) {
    MediaHeader header;
    header.codec = 0xff;
    header.unit_index = 3;
    header.units_total = 0x800;
    const auto ping = encode_media(header, ClientProbe::kPingSize);
    CHECK(ping.size() == 0x224);
    CHECK(hex(std::vector<std::uint8_t>(ping.begin(), ping.begin() + 18)) ==
          "03"          // a sound packet
          "0000" "0000" // packet and frame index
          "007ffc00"    // unit 3 of 2048, no repair units
          "ff"          // the codec byte that marks a ping
          "00000000"
          "00000000");  // key position
    const auto read = parse_media(ping);
    CHECK(read.has_value());
    if (!read) return;
    CHECK(!read->header.video && read->header.unit_index == 3 && read->header.units_total == 0x800 && read->header.codec == 0xff);
    CHECK(read->data.size() == 0x224 - 18);

    MediaHeader video;
    video.video = true;
    video.unit_info = true;
    video.frame_index = 0x1234;
    video.video_word = 0xbeef;
    video.adaptive_stream = 5;
    const auto picture = encode_media(video, 100);
    CHECK(picture[0] == 0x12 && picture.size() == 100);
    const auto read_video = parse_media(picture);
    CHECK(read_video && read_video->header.video && read_video->header.unit_info && read_video->header.frame_index == 0x1234 &&
          read_video->header.video_word == 0xbeef && read_video->header.adaptive_stream == 5 && read_video->data.size() == 100 - 24);
    CHECK(!parse_media(std::vector<std::uint8_t>(17, 3)));
    CHECK(!parse_media(std::vector<std::uint8_t>(30, 0)));
}

PSVP_TEST(probe_measures_a_clean_link) {
    Link link;
    link.run();
    CHECK(link.client.state() == ClientProbe::State::done);
    CHECK(link.client.result().round_trip == 4000us);
    CHECK(link.client.result().mtu_in == 1454);
    CHECK(link.client.result().mtu_out == 1454);
    CHECK(link.client.pongs() == 10);
    // What the console saw, in order: the version request, the opening, echo on, ten pings, echo off, ...
    CHECK(link.events.size() >= 14);
    if (link.events.size() < 14) return;
    CHECK(link.events[0] == "message channel 1 flags 1 type 0: 081ffa01020809");
    CHECK(link.events[1] == "message channel 1 flags 1 type 0: 080012080809" "12001a002200");
    CHECK(link.events[2] == "message channel 8 flags 1 type 0: 080c7206080012020801");
    CHECK(link.events[3] == "ping 548 bytes frame 0 unit 0 units 2048 codec 0xff");
    CHECK(link.events[12] == "ping 548 bytes frame 0 unit 9 units 2048 codec 0xff");
    CHECK(link.events[13] == "message channel 8 flags 1 type 0: 080c7206080012020800");
    // One size request (the largest passes at once), the client's own test with one ping, the goodbye.
    CHECK(link.events[14] == "message channel 8 flags 1 type 0: 080c720908011a05080110ae0b");
    CHECK(link.events[15] == "message channel 8 flags 1 type 0: 080c720e08042a0a080110ae0b180120ae0b");
    CHECK(link.events[16] == "ping 1426 bytes frame 0 unit 0 units 2048 codec 0xff");
    CHECK(link.events[17] == "message channel 8 flags 1 type 0: 080c720e08042a0a080210ae0b180020ae0b");
    CHECK(link.events[19] == "goodbye: Client Disconnecting");
    CHECK(link.console.disconnected());
    // Ten pings 4 ms apart and nothing else that waits: well under a tenth of a second.
    CHECK(link.now - kStart < 100ms);
}

PSVP_TEST(probe_finds_the_largest_packet_each_way) {
    for (const std::uint32_t in : {576U, 577U, 1000U, 1453U, 1454U}) {
        for (const std::uint32_t out : {576U, 600U, 999U, 1000U, 1001U, 1454U}) {
            Link link(ConsoleProbe::Path{in, out});
            link.run();
            CHECK(link.client.state() == ClientProbe::State::done);
            CHECK(link.client.result().mtu_in == in);
            CHECK(link.client.result().mtu_out == out);
        }
    }
}

PSVP_TEST(probe_survives_lost_datagrams) {
    Link link(ConsoleProbe::Path{1300, 1203});
    // Every message but the size requests (which are asked again faster than they are sent again) is lost
    // the first time it is sent, in both directions, and two pings never arrive.
    std::vector<std::vector<std::uint8_t>> seen;
    link.lose = [&](bool to_console, const std::vector<std::uint8_t>& datagram) {
        if (const auto media = parse_media(datagram)) {
            return to_console && datagram.size() == ClientProbe::kPingSize && (media->header.unit_index == 3 || media->header.unit_index == 7);
        }
        const auto packet = parse_control(datagram);
        if (!packet || packet->chunk_type != chunk::kData) return false;
        const auto message = parse_message(parse_data_chunk(packet->payload).value().data);
        if (message && message->as<ProbeMtu>() != nullptr) return false;
        if (std::ranges::find(seen, datagram) != seen.end()) return false;
        seen.push_back(datagram);
        return true;
    };
    link.run();
    CHECK(link.client.state() == ClientProbe::State::done);
    CHECK(link.client.pongs() == 8);
    CHECK(link.client.result().round_trip == 4000us);
    CHECK(link.client.result().mtu_in == 1300);
    CHECK(link.client.result().mtu_out == 1203);
    CHECK(link.count("goodbye") == 0);  // the goodbye is sent once and was lost
}

PSVP_TEST(probe_gives_up_on_a_silent_console) {
    Link link;
    link.lose = [](bool, const std::vector<std::uint8_t>&) { return true; };
    link.run();
    CHECK(link.client.state() == ClientProbe::State::failed);
    CHECK(link.client.failure() == "the console did not answer the probe's version request");
    CHECK(link.now == kStart + 5s);
    CHECK(link.client.result() == ProbeResult{});
    CHECK(link.client.result().mtu_in == 1454 && link.client.result().round_trip == 1000us);
}

PSVP_TEST(probe_fails_when_no_ping_comes_back) {
    Link link;
    link.lose = [](bool to_console, const std::vector<std::uint8_t>& datagram) { return to_console && parse_media(datagram).has_value(); };
    link.run();
    CHECK(link.client.state() == ClientProbe::State::failed);
    CHECK(link.client.failure() == "the console answered none of the pings");
    CHECK(link.client.pongs() == 0);
}

PSVP_TEST(probe_ignores_what_is_not_its_answer) {
    ClientProbe probe(kClientTag, Endpoint{kConsoleTag, kReceiveWindow, kStreams, kStreams, kConsoleTag}, [] { return 7U; });
    CHECK(probe.start(kStart).size() == 1);
    // A bang before the version answer, a pong before any ping, a packet for another connection, noise.
    BangMessage bang;
    ControlPacket early = data_packet(kClientTag, kConsoleTag, 0);
    early.payload = encode_data_chunk(DataChunk{kConsoleTag, 1, data_type::kMessage, encode_message(make_message(bang)), 0});
    const auto out = probe.on_datagram(encode_control(early), kStart + 1ms);
    CHECK(out.size() == 1);  // acknowledged, nothing more
    CHECK(probe.on_datagram(encode_media(MediaHeader{}, 100), kStart + 1ms).empty());
    CHECK(probe.on_datagram(encode_control(data_packet(kClientTag + 1, kConsoleTag + 1, 0)), kStart + 1ms).empty());
    CHECK(probe.on_datagram(std::vector<std::uint8_t>{0xff, 0x00}, kStart + 1ms).empty());
    CHECK(probe.state() == ClientProbe::State::running);
}
