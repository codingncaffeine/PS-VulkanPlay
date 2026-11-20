#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "proto/hex.h"
#include "proto/key_exchange.h"
#include "proto/launch_spec.h"
#include "proto/stream_cipher.h"
#include "proto/stream_setup.h"
#include "vectors_stream.h"

using namespace psvp::proto;
using namespace psvp::proto::transport;
using namespace std::chrono_literals;
namespace vectors = psvp::test::stream_vectors;

namespace {

std::string hex(std::span<const std::uint8_t> bytes) { return hex_encode(bytes); }

const TimePoint kStart{};
constexpr std::uint32_t kClientTag = 0x11223344;
constexpr std::uint32_t kConsoleTag = 0x50607080;
constexpr std::string_view kText = "The quick brown fox jumps over the lazy dog";

// The inputs the reference's vectors were computed for.
Key16 vector_handshake_key() {
    Key16 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(0x10 + i);
    return key;
}

SharedSecret vector_secret() {
    SharedSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<std::uint8_t>(0x80 + i);
    return secret;
}

std::array<std::uint8_t, 32> private_key(std::uint8_t first) {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(first + i);
    return key;
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

RpCipher test_session_cipher(ConsoleKind kind) {
    Key16 nonce{};
    Key16 secret{};
    for (std::size_t i = 0; i < 16; ++i) {
        nonce[i] = static_cast<std::uint8_t>(i * 17);
        secret[i] = static_cast<std::uint8_t>(0xa0 + i);
    }
    return make_session_cipher(kind, nonce, secret);
}

// Both ends of a stream's set-up on a clock of our own.
struct StreamLink {
    struct InFlight {
        TimePoint arrives;
        std::vector<std::uint8_t> datagram;
    };

    ClientStream client;
    ConsoleStream console;
    std::chrono::microseconds hop{2000};
    // True for a datagram that is to be lost. `to_console` tells the direction.
    std::function<bool(bool to_console, const std::vector<std::uint8_t>&)> lose;
    // May change a datagram on its way to the client.
    std::function<void(std::vector<std::uint8_t>&)> tamper;
    // Extra time on the way to the client, by the datagram's number in that direction (from 0).
    std::function<std::chrono::microseconds(int index)> delay_to_client;
    TimePoint now = kStart;
    std::vector<InFlight> to_console;
    std::vector<InFlight> to_client;
    std::vector<std::string> events;
    std::vector<std::size_t> client_datagram_sizes;
    int sent_to_client = 0;
    int steps = 0;

    static StreamRequest request(ConsoleKind kind, std::uint32_t mtu) {
        StreamRequest r;
        r.kind = kind;
        r.session_id = "MockSessionIdent";
        r.session_cipher = test_session_cipher(kind);
        r.network.round_trip = 3400us;
        r.network.mtu_in = mtu;
        r.network.mtu_out = mtu;
        return r;
    }

    static ConsoleStream::Options options(ConsoleKind kind) {
        ConsoleStream::Options o;
        o.kind = kind;
        o.session_cipher = test_session_cipher(kind);
        o.session_id = "MockSessionIdent";
        o.resolutions.push_back(StreamResolution{1280, 720, {0, 0, 0, 1, 0x67, 0x42}});
        return o;
    }

    explicit StreamLink(StreamRequest r = request(ConsoleKind::ps5, kMtuCeiling), ConsoleStream::Options o = options(ConsoleKind::ps5))
        : client(kClientTag, Endpoint{kConsoleTag, kReceiveWindow, kStreams, kStreams, kConsoleTag}, std::move(r),
                 KeyExchange::from_private_key(private_key(1)).value(), vector_handshake_key()),
          console(kConsoleTag, Endpoint{kClientTag, kReceiveWindow, kStreams, kStreams, kClientTag}, std::move(o),
                  KeyExchange::from_private_key(private_key(0xa1)).value()) {}

    void post(bool towards_console, Datagrams datagrams) {
        for (auto& datagram : datagrams) {
            std::chrono::microseconds extra{0};
            if (towards_console) {
                client_datagram_sizes.push_back(datagram.size());
            } else {
                const int index = sent_to_client++;
                if (delay_to_client) extra = delay_to_client(index);
            }
            if (lose && lose(towards_console, datagram)) continue;
            if (!towards_console && tamper) tamper(datagram);
            (towards_console ? to_console : to_client).push_back(InFlight{now + hop + extra, std::move(datagram)});
        }
    }

    void collect() {
        for (auto& line : console.take_events()) events.push_back(std::move(line));
    }

    void start() { post(true, client.start(now)); }

    // Runs for `duration`, or until `stop` is true.
    void run(std::chrono::milliseconds duration, const std::function<bool()>& stop = {}) {
        const TimePoint end = now + duration;
        const auto earliest = [](std::vector<InFlight>& queue) {
            return std::min_element(queue.begin(), queue.end(), [](const InFlight& a, const InFlight& b) { return a.arrives < b.arrives; });
        };
        while (++steps < 200000) {
            if (stop && stop()) return;
            const auto console_arrival = earliest(to_console);
            const auto client_arrival = earliest(to_client);
            // The next thing that happens: an arrival, a timer of either end, or the end of the run.
            enum class Next { finished, console_arrives, client_arrives, client_timer, console_timer } what = Next::finished;
            TimePoint when = end;
            if (console_arrival != to_console.end() && console_arrival->arrives <= when) {
                when = console_arrival->arrives;
                what = Next::console_arrives;
            }
            if (client_arrival != to_client.end() && client_arrival->arrives < when) {
                when = client_arrival->arrives;
                what = Next::client_arrives;
            }
            if (client.deadline() < when) {
                when = client.deadline();
                what = Next::client_timer;
            }
            if (const auto timer = console.deadline(); timer && *timer < when) {
                when = *timer;
                what = Next::console_timer;
            }
            now = std::max(now, when);
            if (what == Next::finished) return;
            if (what == Next::console_arrives) {
                auto datagram = std::move(console_arrival->datagram);
                to_console.erase(console_arrival);
                post(false, console.on_datagram(datagram, now));
            } else if (what == Next::client_arrives) {
                auto datagram = std::move(client_arrival->datagram);
                to_client.erase(client_arrival);
                post(true, client.on_datagram(datagram, now));
            } else if (what == Next::client_timer) {
                post(true, client.on_timeout(now));
            } else {
                post(false, console.on_timer(now));
            }
            collect();
        }
    }

    void run_until_settled() {
        run(20000ms, [&] { return client.state() == ClientStream::State::ready || client.state() == ClientStream::State::failed ||
                                  client.state() == ClientStream::State::closed; });
    }

    [[nodiscard]] int count(std::string_view start) const {
        int n = 0;
        for (const std::string& line : events) n += line.starts_with(start) ? 1 : 0;
        return n;
    }
    [[nodiscard]] bool has(std::string_view line) const { return std::ranges::find(events, line) != events.end(); }
    [[nodiscard]] std::string line_starting(std::string_view start) const {
        for (const std::string& line : events) {
            if (line.starts_with(start)) return line;
        }
        return {};
    }
};

// A data chunk from the console, sealed or not.
std::vector<std::uint8_t> console_chunk(std::uint32_t sequence, std::uint8_t byte, SendKeys* keys) {
    ControlPacket packet;
    packet.tag = kClientTag;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = chunk_flag::kLast;
    packet.payload = encode_data_chunk(DataChunk{sequence, 1, data_type::kMessage, {byte}});
    std::vector<std::uint8_t> datagram = encode_control(packet);
    if (keys != nullptr) (void)seal_datagram(keys->cipher, keys->take(1), datagram);
    return datagram;
}

}  // namespace

// ---- The key exchange -------------------------------------------------------

PSVP_TEST(key_exchange_agrees_with_the_reference) {
    const auto a = KeyExchange::from_private_key(private_key(1));
    const auto b = KeyExchange::from_private_key(private_key(0xa1));
    CHECK(a.has_value() && b.has_value());
    if (!a || !b) return;
    CHECK(hex(b->public_key()) == vectors::k_public_b);
    CHECK(hex(sign_public_key(vector_handshake_key(), b->public_key())) == vectors::k_signature_b);
    const auto from_a = a->shared_secret(b->public_key());
    const auto from_b = b->shared_secret(a->public_key());
    CHECK(from_a.has_value() && from_b.has_value());
    if (!from_a || !from_b) return;
    CHECK(hex(*from_a) == vectors::k_shared_secret);
    CHECK(*from_a == *from_b);
}

PSVP_TEST(key_exchange_makes_a_new_pair_each_time) {
    const KeyExchange a = KeyExchange::generate();
    const KeyExchange b = KeyExchange::generate();
    CHECK(a.public_key() != b.public_key());
    CHECK(a.public_key()[0] == 0x04);
    const auto from_a = a.shared_secret(b.public_key());
    const auto from_b = b.shared_secret(a.public_key());
    CHECK(from_a.has_value() && from_b.has_value() && *from_a == *from_b);
}

PSVP_TEST(key_exchange_refuses_what_is_not_a_key) {
    std::array<std::uint8_t, 32> zero{};
    CHECK(!KeyExchange::from_private_key(zero));
    std::array<std::uint8_t, 32> too_large{};
    too_large.fill(0xff);  // not below the order of the curve
    CHECK(!KeyExchange::from_private_key(too_large));

    const KeyExchange ours = KeyExchange::generate();
    const KeyExchange other = KeyExchange::generate();
    PublicKey point = other.public_key();
    CHECK(ours.shared_secret(point).has_value());
    CHECK(!ours.shared_secret(std::span(point).first(64)));
    point[64] ^= 0x01;  // no longer on the curve
    CHECK(!ours.shared_secret(point));
    point = other.public_key();
    point[0] = 0x02;  // the compressed form is not the one that travels
    CHECK(!ours.shared_secret(point));
    CHECK(!ours.shared_secret({}));
}

// ---- The stream keys --------------------------------------------------------

PSVP_TEST(stream_keys_match_the_reference) {
    const StreamCipher client(kClientDirection, vector_handshake_key(), vector_secret());
    const StreamCipher console(kConsoleDirection, vector_handshake_key(), vector_secret());
    CHECK(hex(client.key()) == vectors::k_client_key);
    CHECK(hex(client.iv()) == vectors::k_client_iv);
    CHECK(hex(client.mac_key(0)) == vectors::k_client_mac_key);
    // The same key up to position 45000, another one after it.
    CHECK(client.mac_key(45000) == client.mac_key(0));
    CHECK(client.mac_key(45001) != client.mac_key(0));
    CHECK(hex(client.mac_key(135001)) == vectors::k_client_mac_key_at_135001);
    CHECK(hex(console.key()) == vectors::k_console_key);
    CHECK(hex(console.iv()) == vectors::k_console_iv);
    CHECK(hex(console.mac_key(0)) == vectors::k_console_mac_key);
    CHECK(hex(console.mac_key(135001)) == vectors::k_console_mac_key_at_135001);

    CHECK(hex(client.mac(0, {bytes_of(kText)})) == vectors::k_client_code_at_0);
    CHECK(hex(client.mac(42, {bytes_of(kText)})) == vectors::k_client_code_at_42);
    CHECK(hex(client.mac(135001, {bytes_of(kText)})) == vectors::k_client_code_at_135001);
    CHECK(hex(console.mac(0, {bytes_of(kText)})) == vectors::k_console_code_at_0);
    CHECK(hex(console.mac(42, {bytes_of(kText)})) == vectors::k_console_code_at_42);
    CHECK(hex(console.mac(135001, {bytes_of(kText)})) == vectors::k_console_code_at_135001);
    // The code of bytes given in pieces is the code of the bytes.
    CHECK(client.mac(42, {bytes_of(kText.substr(0, 7)), {}, bytes_of(kText.substr(7))}) == client.mac(42, {bytes_of(kText)}));

    std::vector<std::uint8_t> text(kText.begin(), kText.end());
    client.apply_key_stream(37, text);
    CHECK(hex(text) == vectors::k_client_sealed_at_37);
    client.apply_key_stream(37, text);
    CHECK(std::string(text.begin(), text.end()) == kText);
    console.apply_key_stream(37, text);
    CHECK(hex(text) == vectors::k_console_sealed_at_37);
}

PSVP_TEST(packet_codes_match_the_reference) {
    const StreamCipher client(kClientDirection, vector_handshake_key(), vector_secret());
    const StreamCipher console(kConsoleDirection, vector_handshake_key(), vector_secret());
    std::vector<std::uint8_t> control(40);
    std::vector<std::uint8_t> sound(40);
    for (std::size_t i = 0; i < 40; ++i) {
        control[i] = static_cast<std::uint8_t>(i);
        sound[i] = static_cast<std::uint8_t>(0x40 + i);
    }
    control[0] = packet_kind::kControl;
    sound[0] = packet_kind::kAudio;
    CHECK(hex(datagram_mac(client, 42, control).value()) == vectors::k_client_control_packet_code);
    CHECK(hex(datagram_mac(client, 42, sound).value()) == vectors::k_client_sound_packet_code);
    CHECK(hex(datagram_mac(console, 42, control).value()) == vectors::k_console_control_packet_code);
    CHECK(hex(datagram_mac(console, 42, sound).value()) == vectors::k_console_sound_packet_code);

    // A control packet's code does not depend on the bytes of its code or of its key position; a sound packet's depends on its key position.
    std::vector<std::uint8_t> changed = control;
    for (std::size_t i = 5; i < 13; ++i) changed[i] = 0xee;
    CHECK(datagram_mac(client, 42, changed) == datagram_mac(client, 42, control));
    changed = sound;
    for (std::size_t i = 10; i < 14; ++i) changed[i] = 0xee;
    CHECK(datagram_mac(client, 42, changed) == datagram_mac(client, 42, sound));
    changed[14] ^= 1;
    CHECK(datagram_mac(client, 42, changed) != datagram_mac(client, 42, sound));
}

PSVP_TEST(packets_are_sealed_and_checked_by_kind) {
    CHECK(mac_layout(packet_kind::kControl).value().mac_offset == 5);
    CHECK(mac_layout(0x13).value().mac_offset == 10);  // only the low four bits say the kind
    CHECK(mac_layout(packet_kind::kCongestion).value().position_offset == 11);
    CHECK(mac_layout(packet_kind::kInputState).value().mac_offset == 8);
    CHECK(mac_layout(packet_kind::kInputHistory).value().position_offset == 4);
    CHECK(!mac_layout(packet_kind::kHandshake));
    CHECK(!mac_layout(packet_kind::kClientInfo));

    const StreamCipher cipher(kConsoleDirection, vector_handshake_key(), vector_secret());
    for (const std::uint8_t kind : {packet_kind::kControl, packet_kind::kVideo, packet_kind::kAudio, packet_kind::kCongestion,
                                    packet_kind::kInputHistory, packet_kind::kInputState}) {
        std::vector<std::uint8_t> datagram(30, 0x5a);
        datagram[0] = kind;
        CHECK(seal_datagram(cipher, 0x1'0000'0123ULL, datagram));
        const MacLayout layout = mac_layout(kind).value();
        // The low 32 bits of the key position travel in the packet.
        CHECK(hex(std::span(datagram).subspan(layout.position_offset, 4)) == "00000123");
        ReceiveKeys keys(cipher);
        // Not authentic for a receiver that has not yet seen the positions pass the 32-bit mark...
        CHECK(!verify_datagram(keys, datagram));
        // ...authentic for one that has.
        keys.positions.accept(0xffff'ff00ULL);
        CHECK(verify_datagram(keys, datagram));
        CHECK(keys.positions.last() == 0x1'0000'0123ULL);
        for (std::size_t i = 0; i < datagram.size(); ++i) {
            for (const std::uint8_t bit : {std::uint8_t{0x01}, std::uint8_t{0x10}}) {
                std::vector<std::uint8_t> changed = datagram;
                changed[i] ^= bit;
                ReceiveKeys fresh(cipher);
                fresh.positions.accept(0xffff'ff00ULL);
                // Where the position's own bytes do not count, the code depends on the position only
                // through its sixteen-byte block: a change within the low four bits goes unnoticed.
                const bool unnoticed = !layout.position_counts && i == layout.position_offset + 3 && bit == 0x01;
                CHECK(verify_datagram(fresh, changed) == unnoticed);
                CHECK(fresh.positions.last() == (unnoticed ? 0x1'0000'0122ULL : 0xffff'ff00ULL));
            }
        }
    }
    // Too short to hold its fields, or of a kind that carries none.
    std::vector<std::uint8_t> small(12, 0);
    CHECK(!seal_datagram(cipher, 0, small));
    std::vector<std::uint8_t> handshake(30, 0);
    handshake[0] = packet_kind::kHandshake;
    CHECK(!seal_datagram(cipher, 0, handshake));
    CHECK(!datagram_mac(cipher, 0, {}));
}

PSVP_TEST(key_positions_follow_the_wrap_of_their_32_bits) {
    KeyPositionTracker tracker;
    CHECK(tracker.extend(100) == 100);
    tracker.accept(0xffff'fff0ULL);
    CHECK(tracker.extend(0xffff'fff8U) == 0xffff'fff8ULL);
    CHECK(tracker.extend(0x10) == 0x1'0000'0010ULL);  // past the top
    tracker.accept(0x1'0000'0010ULL);
    CHECK(tracker.extend(0x20) == 0x1'0000'0020ULL);
    CHECK(tracker.extend(0xffff'fff8U) == 0xffff'fff8ULL);  // a late packet from before the wrap
    tracker.accept(0x2'8000'0000ULL);
    CHECK(tracker.extend(0x7fff'ffffU) == 0x2'7fff'ffffULL);
    // Never below zero.
    KeyPositionTracker fresh;
    fresh.accept(5);
    CHECK(fresh.extend(0xffff'fff0U) == 0xffff'fff0ULL);
}

PSVP_TEST(send_positions_advance_by_size_plus_its_remainder) {
    SendKeys keys(StreamCipher(kClientDirection, vector_handshake_key(), vector_secret()));
    CHECK(keys.take(29) == 0);   // an acknowledgment: 29 + 13
    CHECK(keys.take(2) == 42);   // 2 + 2
    CHECK(keys.take(16) == 46);  // 16 + 0
    CHECK(keys.take(20) == 62);  // 20 + 4
    CHECK(keys.next_position == 86);
}

// ---- The launch specification ----------------------------------------------

PSVP_TEST(launch_spec_text_is_the_reference_text) {
    LaunchSpec spec;
    spec.round_trip_ms = 3;
    spec.handshake_key = vector_handshake_key();
    spec.kind = ConsoleKind::ps4;
    CHECK(format_launch_spec(spec) == vectors::k_ps4_launch_spec);
    spec.kind = ConsoleKind::ps5;
    spec.codec = Codec::hevc;
    CHECK(format_launch_spec(spec) == vectors::k_ps5_launch_spec);
    spec.codec = Codec::hevc_hdr;
    CHECK(format_launch_spec(spec).find("\"videoCodec\":\"hevc\",\"dynamicRange\":\"HDR\",") != std::string::npos);
    spec.codec = Codec::h264;
    CHECK(format_launch_spec(spec).find("\"videoCodec\":\"avc\",\"dynamicRange\":\"SDR\",") != std::string::npos);
    spec.width = 1920;
    spec.height = 1080;
    spec.max_fps = 30;
    spec.bitrate_kbps = 15000;
    spec.mtu = 1300;
    const std::string text = format_launch_spec(spec);
    CHECK(text.find("{\"width\":1920,\"height\":1080},\"maxFps\":30,") != std::string::npos);
    CHECK(text.find("\"bwKbpsSent\":15000,\"bwLoss\":0.001000,\"mtu\":1300,\"rtt\":3,") != std::string::npos);
    CHECK(launch_spec_handshake_key(text) == vector_handshake_key());
}

PSVP_TEST(launch_spec_is_sealed_and_opened_with_the_session_cipher) {
    const RpCipher cipher = test_session_cipher(ConsoleKind::ps5);
    LaunchSpec spec;
    spec.handshake_key = vector_handshake_key();
    const std::string text = format_launch_spec(spec);
    const std::string sealed = seal_launch_spec(cipher, text);
    // Base64 of the text and one closing byte.
    CHECK(sealed.size() == (text.size() + 1 + 2) / 3 * 4);
    CHECK(sealed.find("sessionId") == std::string::npos);
    CHECK(open_launch_spec(cipher, sealed) == text);
    // Another session's cipher opens it to something else, or to nothing.
    const auto other = open_launch_spec(test_session_cipher(ConsoleKind::ps4), sealed);
    CHECK(!other || *other != text);
    CHECK(!open_launch_spec(cipher, "not base64"));
    CHECK(!open_launch_spec(cipher, ""));
    CHECK(!launch_spec_handshake_key("{\"handshakeKey\":\"AAAA\"}"));  // three bytes
    CHECK(!launch_spec_handshake_key("{\"handshakeKey\":\"EBESExQVFhcYGRobHB0eHw=="));  // never closed
    CHECK(!launch_spec_handshake_key("{}"));
}

// ---- Split messages and packet authentication in the messenger --------------

PSVP_TEST(messenger_splits_a_message_and_joins_it_again) {
    Messenger sender(kConsoleTag, kClientTag, 100, 500);
    Messenger receiver(kClientTag, kConsoleTag, 500, 100);
    std::vector<std::uint8_t> message(300);
    for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>(i * 7 + 1);
    // Datagrams of at most 100 bytes: 74 bytes in the first chunk, 75 in each later one.
    const auto sent = sender.send_message(3, message, kStart, 100);
    CHECK(sent.has_value() && sent->size() == 5);
    if (!sent || sent->size() != 5) return;
    for (std::size_t i = 0; i < 5; ++i) {
        const ControlPacket packet = parse_control((*sent)[i].datagram).value();
        CHECK((*sent)[i].datagram.size() == (i < 4 ? 100U : 26U));
        CHECK(packet.chunk_flags == (i == 4 ? 1 : 0));
        CHECK((*sent)[i].sequence == 100 + i);
    }
    // The first chunk names the kind of data after its eight header bytes; the later ones go straight on.
    CHECK(hex(std::span((*sent)[0].datagram).subspan(17, 12)) == "00000064" "0003" "0000" "00" "01080f");
    CHECK(hex(std::span((*sent)[1].datagram).subspan(17, 11)) == "00000065" "0003" "0000" + hex(std::span(message).subspan(74, 3)));

    // They arrive in another order and come out as the one message.
    std::vector<DataChunk> delivered;
    for (const std::size_t i : {std::size_t{2}, std::size_t{0}, std::size_t{4}, std::size_t{1}, std::size_t{3}}) {
        auto received = receiver.on_packet(parse_control((*sent)[i].datagram).value());
        for (auto& chunk : received.delivered) delivered.push_back(std::move(chunk));
        CHECK(receiver.mid_message() == (delivered.empty() && i != 2));
    }
    CHECK(delivered.size() == 1);
    if (delivered.size() != 1) return;
    CHECK(delivered[0].data == message);
    CHECK(delivered[0].chunks == 5 && delivered[0].channel == 3 && delivered[0].sequence == 104 && delivered[0].data_type == data_type::kMessage);
    const auto ack = receiver.take_ack();
    CHECK(ack && parse_data_ack(parse_control(*ack).value().payload).value().cumulative == 104);
}

PSVP_TEST(messenger_splits_only_what_does_not_fit) {
    Messenger sender(kConsoleTag, kClientTag, 1, 1);
    const std::vector<std::uint8_t> fits(74, 0xaa);
    const auto one = sender.send_message(1, fits, kStart, 100);
    CHECK(one && one->size() == 1 && (*one)[0].datagram.size() == 100 && parse_control((*one)[0].datagram).value().chunk_flags == 1);
    const std::vector<std::uint8_t> one_more(75, 0xaa);
    const auto two = sender.send_message(1, one_more, kStart, 100);
    CHECK(two && two->size() == 2 && (*two)[1].datagram.size() == 17 + 8 + 1);
    CHECK(sender.waiting() == 3);
    // A message that needs more chunks than the window has room for is not begun.
    const std::vector<std::uint8_t> large(74 + 75 * 13 + 1, 0xbb);  // 15 chunks, 13 free
    CHECK(!sender.send_message(1, large, kStart, 100));
    CHECK(sender.waiting() == 3);
    const std::vector<std::uint8_t> just(74 + 75 * 12, 0xbb);  // 13 chunks
    CHECK(sender.send_message(1, just, kStart, 100).has_value());
    CHECK(sender.waiting() == Messenger::kWindow);
    Messenger other(kConsoleTag, kClientTag, 1, 1);
    CHECK(!other.send_message(1, fits, kStart, Messenger::kSmallestDatagram - 1));
    CHECK(other.send_message(1, {}, kStart, Messenger::kSmallestDatagram).has_value());
}

PSVP_TEST(messenger_seals_and_checks_once_it_has_keys) {
    const StreamCipher client_cipher(kClientDirection, vector_handshake_key(), vector_secret());
    const StreamCipher console_cipher(kConsoleDirection, vector_handshake_key(), vector_secret());
    Messenger client(kClientTag, kConsoleTag, 10, 20, Messenger::Keys::expected);
    auto client_send = std::make_shared<SendKeys>(client_cipher);
    client.use_keys(client_send, std::make_shared<ReceiveKeys>(console_cipher));
    SendKeys console_keys(console_cipher);

    // A sealed chunk is handed over as authentic; the acknowledgment is sealed at the client's first position.
    auto received = client.on_packet(parse_control(console_chunk(20, 0x61, &console_keys)).value());
    CHECK(received.delivered.size() == 1 && received.delivered[0].authentic);
    const auto ack = client.take_ack();
    CHECK(ack.has_value());
    if (ack) {
        const ControlPacket packet = parse_control(*ack).value();
        CHECK(packet.key_position == 0);
        CHECK(packet.mac == datagram_mac(client_cipher, 0, *ack).value());
        CHECK(packet.mac != Mac4{});
    }
    // The next thing sent lies 29 + 13 further on, and a message of three bytes moves it by 3 + 3.
    const std::vector<std::uint8_t> body{1, 2, 3};
    const auto sent = client.send(1, 1, body, kStart);
    CHECK(sent && parse_control(sent->datagram).value().key_position == 42);
    CHECK(sent && parse_control(sent->datagram).value().mac == datagram_mac(client_cipher, 42, sent->datagram).value());
    CHECK(client_send->next_position == 48);

    // A changed chunk, and one that was never sealed, change nothing.
    std::vector<std::uint8_t> changed = console_chunk(21, 0x62, &console_keys);
    changed.back() ^= 0x01;
    received = client.on_packet(parse_control(changed).value());
    CHECK(received.delivered.empty() && client.rejected() == 1 && !client.take_ack());
    received = client.on_packet(parse_control(console_chunk(21, 0x62, nullptr)).value());
    CHECK(received.delivered.empty() && client.rejected() == 2 && !client.take_ack());
    // An unsealed acknowledgment does not release what waits.
    ControlPacket forged;
    forged.tag = kClientTag;
    forged.chunk_type = chunk::kDataAck;
    forged.payload = encode_data_ack(DataAck{10, kReceiveWindow, 0, 0});
    CHECK(client.on_packet(forged).acknowledged.empty() && client.waiting() == 1 && client.rejected() == 3);
    std::vector<std::uint8_t> real_ack = encode_control(forged);
    (void)seal_datagram(console_cipher, console_keys.take(real_ack.size()), real_ack);
    CHECK(client.on_packet(parse_control(real_ack).value()).acknowledged == std::vector<std::uint32_t>{10});
}

PSVP_TEST(messenger_checks_what_arrived_before_the_keys) {
    const StreamCipher client_cipher(kClientDirection, vector_handshake_key(), vector_secret());
    const StreamCipher console_cipher(kConsoleDirection, vector_handshake_key(), vector_secret());
    SendKeys console_keys(console_cipher);
    const std::vector<std::uint8_t> unsealed_first = console_chunk(20, 0x61, nullptr);  // the message that brings the keys
    const std::vector<std::uint8_t> sealed_second = console_chunk(21, 0x62, &console_keys);
    const std::vector<std::uint8_t> sealed_third = console_chunk(22, 0x63, &console_keys);
    std::vector<std::uint8_t> bad_second = sealed_second;
    bad_second.back() ^= 0x01;

    for (const bool tampered : {false, true}) {
        Messenger client(kClientTag, kConsoleTag, 10, 20, Messenger::Keys::expected);
        // The second and third overtake the first.
        CHECK(client.on_packet(parse_control(tampered ? bad_second : sealed_second).value()).delivered.empty());
        CHECK(client.on_packet(parse_control(sealed_third).value()).delivered.empty());
        // The first arrives: it alone is handed over, though the others are in order behind it.
        auto received = client.on_packet(parse_control(unsealed_first).value());
        CHECK(received.delivered.size() == 1 && !received.delivered[0].authentic && received.delivered[0].data[0] == 0x61);
        CHECK(client.ready());
        client.use_keys(std::make_shared<SendKeys>(client_cipher), std::make_shared<ReceiveKeys>(console_cipher));
        received = client.pump();
        if (!tampered) {
            CHECK(received.delivered.size() == 2 && client.rejected() == 0);
            if (received.delivered.size() == 2) CHECK(received.delivered[0].authentic && received.delivered[1].data[0] == 0x63);
            const auto ack = client.take_ack();
            CHECK(ack && parse_data_ack(parse_control(*ack).value().payload).value().cumulative == 22);
        } else {
            // The changed one was dropped when the keys came; the third waits for a good copy of it.
            CHECK(received.delivered.empty() && client.rejected() == 1 && !client.ready());
            const auto ack = client.take_ack();
            CHECK(ack && parse_data_ack(parse_control(*ack).value().payload).value().cumulative == 20);
            received = client.on_packet(parse_control(sealed_second).value());
            CHECK(received.delivered.size() == 2);
        }
    }
    // A connection that expects no keys hands everything over at once.
    Messenger plain(kClientTag, kConsoleTag, 10, 20);
    CHECK(plain.on_packet(parse_control(console_chunk(21, 0x62, nullptr)).value()).delivered.empty());
    CHECK(plain.on_packet(parse_control(unsealed_first).value()).delivered.size() == 2);
}

// ---- The set-up, both ends ---------------------------------------------------

PSVP_TEST(stream_setup_completes_on_a_clean_link) {
    StreamLink link;
    link.start();
    CHECK(link.client.state() == ClientStream::State::awaiting_bang);
    CHECK(link.client.opening_datagrams() == 1);
    link.run_until_settled();
    CHECK(link.client.state() == ClientStream::State::ready);
    if (link.client.state() != ClientStream::State::ready) return;
    // The opening message there; the answer and the stream information back together: two hops.
    CHECK(link.now == kStart + 2 * link.hop);

    const BangFacts facts = link.client.bang().value();
    CHECK(facts.server_version == 12 && facts.signature_matches && facts.session_key_is_ours && facts.chunks == 1);
    CHECK(facts.code == BangFacts::Code::none);
    const StreamInfo info = link.client.info().value();
    CHECK(info.resolutions.size() == 1 && info.resolutions[0].width == 1280 && info.resolutions[0].height == 720);
    CHECK(parse_audio_header(info.audio_header) == AudioHeader{});
    CHECK(link.client.control_rejected() == 0);

    // What the console saw, in order. The key positions are the ones the sizes of the packets before give:
    // the acknowledgment of the answer at 0 (29 + 13), then messages of 2, 9 and 20 bytes.
    link.run(10ms);
    CHECK(link.line_starting("big:") ==
          "big: version 12 chunks 1 key-position 0 code 00000000 session-id the one handed out encrypted-key 00000000 public-key 65 bytes signature ok");
    const std::string spec = link.line_starting("launch spec:");
    CHECK(spec.find("\"mtu\":1454,\"rtt\":3,") != std::string::npos);
    CHECK(spec.find("\"videoCodec\":\"avc\",\"dynamicRange\":\"SDR\",\"handshakeKey\":\"EBESExQVFhcYGRobHB0eHw==\"}") != std::string::npos);
    const std::vector<std::string> expected{
        "bang sent",
        "stream information sent",
        "ack key-position 0 authentic",
        "message type 14 channel 9 flags 1 key-position 42 authentic: 080e",
        "message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011802",
        "message type 13 channel 1 flags 1 key-position 64 authentic: 080d7a10120e01100000bb80000001e000000001",
        "ack key-position 88 authentic",
        // The console answers the client's stream information with two chunks in two packets; each gets
        // its acknowledgment, 29 + 13 behind the one before it.
        "ack key-position 130 authentic",
        "ack key-position 172 authentic",
    };
    std::vector<std::string> after_spec(link.events.begin() + 2, link.events.end());
    CHECK(after_spec == expected);
    // Those two chunks, as a PS5 was seen to send them, on channel 0: controller information and "stream information received".
    const std::vector<ConsoleData> data = link.client.take_console_data();
    CHECK(data.size() == 2);
    if (data.size() == 2) {
        CHECK(!data[0].message && data[0].kind == data_type::kPadInfo && data[0].channel == 0 &&
              hex(data[0].bytes) == "0000000001000001000000010101000000");
        CHECK(data[1].message && data[1].kind == message_type::kStreamInfoAck && data[1].channel == 0 && hex(data[1].bytes) == "080e");
    }
    CHECK(link.client.counters().messages.at(message_type::kStreamInfoAck) == 1 && link.client.counters().other_data.at(data_type::kPadInfo) == 1);
    CHECK(link.client.take_console_data().empty());

    // A heartbeat every second.
    link.run(3500ms);
    CHECK(link.client.counters().heartbeats_sent == 3);
    // Four acknowledgments, three messages and three heartbeats were sealed, and a congestion report and an
    // input state every 200 ms from the set-up's end at 4 ms to here at 3514 ms: seventeen of each. Each of
    // the 44 sits where the one before it ended. The input states are a resting controller's, in order.
    CHECK(link.client.counters().congestion_reports == 17 && link.client.counters().input_states == 17);
    CHECK(link.console.summary() == "heartbeats 3 control-rejected 0 before-keys 0 kind-5-authentic 17 kind-6-authentic 17 "
                                    "key-positions 44 out-of-step 0 input-states 17 at-rest 17 unreadable 0 out-of-order 0 "
                                    "history-packets 0 events 0 unreadable 0 "
                                    "frames-sent 0 sound-sent 0 withheld 0 congestion-received 0 congestion-lost 0");

    // The goodbye.
    link.post(true, link.client.disconnect(link.now));
    CHECK(link.client.state() == ClientStream::State::closed && link.client.goodbye_pending() && !link.client.closed_by_console());
    link.run(100ms);
    CHECK(link.has("goodbye: Client Disconnecting"));
    CHECK(link.console.disconnected());
    CHECK(!link.client.goodbye_pending());
    CHECK(link.count("control packet REJECTED") == 0);
}

PSVP_TEST(stream_setup_announces_the_controller_it_is_given) {
    StreamRequest request = StreamLink::request(ConsoleKind::ps5, kMtuCeiling);
    request.dualsense = true;
    request.video.codec = Codec::hevc_hdr;
    StreamLink link(std::move(request));
    link.start();
    link.run_until_settled();
    link.run(10ms);
    CHECK(link.has("message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011806"));
    CHECK(link.line_starting("launch spec:").find("\"videoCodec\":\"hevc\",\"dynamicRange\":\"HDR\",") != std::string::npos);
}

PSVP_TEST(stream_setup_speaks_version_9_to_a_ps4) {
    StreamLink link(StreamLink::request(ConsoleKind::ps4, kMtuCeiling), StreamLink::options(ConsoleKind::ps4));
    link.start();
    link.run_until_settled();
    CHECK(link.client.state() == ClientStream::State::ready);
    CHECK(link.client.bang() && link.client.bang()->server_version == 9);
    CHECK(link.line_starting("big:").starts_with("big: version 9 chunks 1 "));
    CHECK(link.line_starting("launch spec:").find("videoCodec") == std::string::npos);
    // A console of the other family does not accept the version.
    StreamLink mixed(StreamLink::request(ConsoleKind::ps4, kMtuCeiling));
    mixed.console = ConsoleStream(kConsoleTag, Endpoint{kClientTag, kReceiveWindow, kStreams, kStreams, kClientTag},
                                  [] {
                                      auto o = StreamLink::options(ConsoleKind::ps5);
                                      o.session_cipher = test_session_cipher(ConsoleKind::ps4);
                                      return o;
                                  }(),
                                  KeyExchange::from_private_key(private_key(0xa1)).value());
    mixed.start();
    mixed.run_until_settled();
    CHECK(mixed.client.state() == ClientStream::State::failed);
    CHECK(mixed.client.reason() == "the console does not speak version 9 of the stream protocol");
    CHECK(mixed.has("bang sent: version not accepted"));
}

PSVP_TEST(stream_setup_splits_the_opening_message_on_a_narrow_path) {
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuFloor));
    link.start();
    // No datagram larger than the path's largest packet less the margin.
    CHECK(link.client.opening_datagrams() == 3);
    for (const std::size_t size : link.client_datagram_sizes) CHECK(size <= kMtuFloor - kSplitMargin);
    CHECK(link.client_datagram_sizes.size() == 3 && link.client_datagram_sizes[0] == kMtuFloor - kSplitMargin);
    link.run_until_settled();
    CHECK(link.client.state() == ClientStream::State::ready);
    CHECK(link.line_starting("big:").starts_with("big: version 12 chunks 3 "));
    CHECK(link.line_starting("launch spec:").find("\"mtu\":576,") != std::string::npos);
}

PSVP_TEST(stream_setup_survives_reordering_and_loss) {
    {
        // The stream information overtakes the answer that brings its keys.
        StreamLink link;
        // The console's first three datagrams: the answer, the stream information, the acknowledgment of the opening message.
        link.delay_to_client = [](int index) { return index == 0 ? 30000us : 0us; };
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::ready);
        CHECK(link.client.control_rejected() == 0);
        CHECK(link.now == kStart + 2 * link.hop + 30000us);
        // Both were answered with one acknowledgment, after the client's three messages
        // (of 2, 9 and 20 bytes: key positions 0, 4 and 22, and 46 for the acknowledgment).
        link.run(10ms);
        CHECK(link.has("message type 14 channel 9 flags 1 key-position 0 authentic: 080e"));
        CHECK(link.has("ack key-position 46 authentic"));
        // And one each for the console's two answers to the client's stream information, 29 + 13 apart.
        CHECK(link.count("ack key-position") == 3 && link.has("ack key-position 88 authentic") && link.has("ack key-position 130 authentic"));
    }
    {
        // The first copy of every datagram is lost, both ways.
        StreamLink link;
        std::vector<std::vector<std::uint8_t>> seen;
        link.lose = [&](bool, const std::vector<std::uint8_t>& datagram) {
            // Acknowledgments and congestion reports are never sent again; only what is, is lost once.
            const auto control = parse_control(datagram);
            if (!control || control->chunk_type != chunk::kData) return false;
            if (std::ranges::find(seen, datagram) != seen.end()) return false;
            seen.push_back(datagram);
            return true;
        };
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::ready);
        CHECK(link.client.control_rejected() == 0);
        link.run(1000ms);
        CHECK(link.count("message type 13 ") == 1);
        CHECK(link.count("control packet REJECTED") == 0);
    }
}

PSVP_TEST(stream_setup_drops_a_changed_packet_and_takes_its_resend) {
    StreamLink link;
    int sealed_seen = 0;
    link.tamper = [&](std::vector<std::uint8_t>& datagram) {
        const ControlPacket packet = parse_control(datagram).value();
        // The first sealed message from the console: the stream information.
        if (packet.chunk_type == chunk::kData && packet.mac != Mac4{} && sealed_seen++ == 0) datagram.back() ^= 0x01;
    };
    link.start();
    link.run(100ms);
    CHECK(link.client.state() == ClientStream::State::awaiting_stream_info);
    CHECK(link.client.control_rejected() == 1);
    link.run_until_settled();
    CHECK(link.client.state() == ClientStream::State::ready);
    // Its resend comes 200 ms after it was first sent, one hop after the opening message.
    CHECK(link.now == kStart + link.hop + 200ms + link.hop);
}

PSVP_TEST(stream_setup_refuses_a_ps5_whose_signature_does_not_match) {
    {
        auto options = StreamLink::options(ConsoleKind::ps5);
        options.wrong_signature = true;
        StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), std::move(options));
        link.start();
        link.run_until_settled();
        // A PS5 signs its key with the handshake key: another signature ends the set-up, before any key is used.
        CHECK(link.client.state() == ClientStream::State::failed);
        CHECK(link.client.reason() == "the console's key is not signed with this session's handshake key");
        CHECK(link.client.bang() && !link.client.bang()->signature_matches);
        CHECK(!link.client.info());
        link.run(500ms);
        CHECK(link.count("message type") == 0);  // nothing of the client's followed
    }
    {
        // What a PS4 signs with has not been seen: there it is reported, and the stream goes on.
        auto options = StreamLink::options(ConsoleKind::ps4);
        options.wrong_signature = true;
        StreamLink link(StreamLink::request(ConsoleKind::ps4, kMtuCeiling), std::move(options));
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::ready);
        CHECK(link.client.bang() && !link.client.bang()->signature_matches);
    }
}

PSVP_TEST(stream_setup_gives_up_on_a_console_that_does_not_answer) {
    {
        StreamLink link;
        link.lose = [](bool to_console, const std::vector<std::uint8_t>&) { return !to_console; };
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::failed);
        CHECK(link.client.reason() == "the console did not answer the stream's opening message");
        CHECK(link.now == kStart + 5s);
        CHECK(link.client.disconnect(link.now).empty());
    }
    {
        // It answers, and then sends no stream information.
        StreamLink link;
        link.lose = [](bool to_console, const std::vector<std::uint8_t>& datagram) {
            const ControlPacket packet = parse_control(datagram).value();
            return !to_console && packet.chunk_type == chunk::kData && packet.mac != Mac4{};
        };
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::failed);
        CHECK(link.client.reason() == "the console sent no stream information");
        CHECK(link.now == kStart + 2 * link.hop + 5s);
    }
    {
        // A console whose opening message never opens (another session's cipher) stays silent.
        auto options = StreamLink::options(ConsoleKind::ps5);
        options.session_cipher = test_session_cipher(ConsoleKind::ps4);
        StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), std::move(options));
        link.start();
        link.run_until_settled();
        CHECK(link.client.state() == ClientStream::State::failed);
        CHECK(link.count("bang sent") == 0);
        CHECK(link.line_starting("big:").ends_with("signature BAD"));
    }
}

PSVP_TEST(stream_counts_media_packets_by_their_code) {
    for (const bool corrupt : {false, true}) {
        auto options = StreamLink::options(ConsoleKind::ps5);
        options.media_every = 100ms;
        options.corrupt_media = corrupt;
        StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), std::move(options));
        link.start();
        link.run_until_settled();
        link.run(1050ms);
        const StreamCounters& counters = link.client.counters();
        // One every 100 ms from the moment the stream information went out.
        CHECK((corrupt ? counters.media_rejected : counters.audio_packets) == 10);
        CHECK((corrupt ? counters.audio_packets : counters.media_rejected) == 0);
        CHECK(counters.video_packets == 0 && counters.media_before_keys == 0 && counters.other_datagrams == 0);
    }
    // Media that arrives before there is a key is counted apart; other kinds too.
    StreamLink link;
    link.start();
    std::vector<std::uint8_t> early(40, 0);
    early[0] = packet_kind::kVideo;
    CHECK(link.client.on_datagram(early, link.now).empty());
    early[0] = packet_kind::kClientInfo;
    CHECK(link.client.on_datagram(early, link.now).empty());
    CHECK(link.client.on_datagram({}, link.now).empty());
    CHECK(link.client.counters().media_before_keys == 1 && link.client.counters().other_datagrams == 1);
}

// What the client handed on, against what the mock console says it sends for this seed.
struct Taken {
    MediaOutput media;
    int video_wrong = 0;
    int sound_wrong = 0;
    std::vector<std::uint16_t> video;
    std::vector<std::uint16_t> sound;
    std::vector<std::uint16_t> haptics;

    Taken(StreamLink& link, const ConsoleStream::Options& options) : media(link.client.take_media()) {
        for (const AssembledFrame& frame : media.video) {
            video.push_back(frame.index);
            // A keyframe is told by its fifth byte.
            const bool keyframe = frame.data.size() > 4 && frame.data[4] == 0x65;
            if (frame.data != pattern_video_frame(options.seed, frame.index, options.frame_bytes, keyframe)) ++video_wrong;
        }
        for (const SoundFrame& frame : media.sound) {
            (frame.haptics ? haptics : sound).push_back(frame.index);
            if (frame.data != pattern_sound_frame(options.seed, frame.index, options.sound_bytes, frame.haptics)) ++sound_wrong;
        }
    }
};

std::vector<std::uint16_t> counting(std::uint16_t from, std::uint16_t to) {
    std::vector<std::uint16_t> out;
    for (std::uint16_t i = from; i <= to; ++i) out.push_back(i);
    return out;
}

ConsoleStream::Options media_options(ConsoleKind kind) {
    auto options = StreamLink::options(kind);
    options.frame_every = 16ms;
    options.frames = 30;
    options.seed = 77;
    return options;
}

PSVP_TEST(stream_hands_on_picture_sound_and_haptics) {
    for (const ConsoleKind kind : {ConsoleKind::ps5, ConsoleKind::ps4}) {
        auto options = media_options(kind);
        options.media_every = 10ms;
        options.haptics_every = 4;
        StreamLink link(StreamLink::request(kind, kMtuCeiling), options);
        link.start();
        link.run_until_settled();
        link.run(605ms);
        const Taken taken(link, options);
        // Thirty frames, each of them the bytes that were sent, in order; the first one a keyframe.
        CHECK(taken.video == counting(1, 30) && taken.video_wrong == 0);
        CHECK(taken.media.video.size() == 30 && taken.media.video[0].data[4] == 0x65 && taken.media.video[1].data[4] == 0x41);
        CHECK(link.client.video().frames == 30 && link.client.video().frames_repaired == 0 && link.client.video().frames_lost == 0);
        // A sound frame every 10 ms from 2 ms on; the last one sent at 602 ms arrives at 604 ms.
        CHECK(taken.sound == counting(1, 60) && taken.sound_wrong == 0);
        CHECK(link.client.sound().frames == 60 && link.client.sound().frames_recovered == 0 && link.client.sound().frames_lost == 0);
        // Haptics with every fourth sound packet, from a PS5 only.
        if (kind == ConsoleKind::ps5) CHECK(taken.haptics == counting(1, 15) && link.client.haptics().frames == 15);
        else CHECK(taken.haptics.empty() && link.client.haptics().packets == 0);
        const StreamCounters& counters = link.client.counters();
        CHECK(counters.media_rejected == 0 && counters.media_unreadable == 0 && counters.loss_reports == 0);
        CHECK(counters.audio_packets == (kind == ConsoleKind::ps5 ? 75U : 60U));
        CHECK(link.count("frames ") == 0 && link.count("keyframe requested") == 0);
        CHECK(link.client.take_media().video.empty());
        // The congestion reports told the console of packets that arrived and of none that did not.
        const std::string summary = link.console.summary();
        CHECK(summary.find("congestion-lost 0") != std::string::npos && summary.find("congestion-received 0 ") == std::string::npos);
        CHECK(summary.find("out-of-step 0") != std::string::npos);
    }
}

PSVP_TEST(stream_restores_what_the_repair_units_cover) {
    auto options = media_options(ConsoleKind::ps5);
    options.lose_every = 3;   // frames 3, 6, ... lose their first two packets: as many as they have repair units
    options.lose_units = 2;
    options.late_every = 4;   // frames 4, 8, ... : the last packet comes behind the next frame's first
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), options);
    link.start();
    link.run_until_settled();
    link.run(605ms);
    const Taken taken(link, options);
    CHECK(taken.video == counting(1, 30) && taken.video_wrong == 0);
    CHECK(link.client.video().frames_repaired == 10 && link.client.video().units_repaired == 20 && link.client.video().frames_lost == 0);
    for (const AssembledFrame& frame : taken.media.video) CHECK(frame.repaired == (frame.index % 3 == 0) && frame.lost_before == 0);
    CHECK(link.client.counters().loss_reports == 0 && link.count("frames ") == 0);
}

PSVP_TEST(stream_reports_frames_that_cannot_be_restored) {
    auto options = media_options(ConsoleKind::ps5);
    options.lose_every = 5;   // frames 5, 10, ... lose three packets: one more than their repair units cover
    options.lose_units = 3;
    options.media_every = 10ms;
    options.lose_sound_every = 7;  // every seventh sound packet is lost: the next one brings its frame again
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), options);
    link.start();
    link.run_until_settled();
    link.run(605ms);
    const Taken taken(link, options);
    std::vector<std::uint16_t> expected;
    for (std::uint16_t i = 1; i <= 30; ++i) {
        if (i % 5 != 0) expected.push_back(i);
    }
    CHECK(taken.video == expected && taken.video_wrong == 0);
    CHECK(link.client.video().frames == 24 && link.client.video().frames_lost == 6);
    for (const AssembledFrame& frame : taken.media.video) CHECK(frame.lost_before == (frame.index % 5 == 1 && frame.index > 1 ? 1U : 0U));
    // The console was told of each, on channel 2, in a sealed message. The last frame of all has no
    // frame behind it; its own last packet came, so its wait ran out all the same.
    CHECK(link.client.counters().loss_reports == 6 && link.count("frames ") == 6);
    for (const int lost : {5, 10, 15, 20, 25, 30}) {
        CHECK(link.has(std::format("frames {} to {} reported as not arrived (channel 2, authentic)", lost, lost)));
    }
    // The sound has no gap, and eight of its frames came from a later packet.
    CHECK(taken.sound == counting(1, 60) && taken.sound_wrong == 0);
    CHECK(link.client.sound().frames_recovered == 8 && link.client.sound().frames_lost == 0);
    // The reports of what did not arrive reached the console.
    CHECK(link.console.summary().find("congestion-lost 0") == std::string::npos);
}

PSVP_TEST(stream_asks_for_a_keyframe) {
    auto options = media_options(ConsoleKind::ps5);
    options.frames = 0;
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), options);
    // Not before the stream is set up.
    CHECK(link.client.request_keyframe(link.now).empty());
    link.start();
    link.run_until_settled();
    link.run(100ms);
    link.post(true, link.client.request_keyframe(link.now));
    CHECK(link.client.counters().keyframe_requests == 1);
    link.run(100ms);
    CHECK(link.has("keyframe requested (channel 2, authentic)"));
    const Taken taken(link, options);
    int keyframes = 0;
    for (const AssembledFrame& frame : taken.media.video) keyframes += frame.data[4] == 0x65 ? 1 : 0;
    // The first frame of the stream, and the one after the request.
    CHECK(keyframes == 2 && taken.video_wrong == 0 && taken.media.video.size() >= 10);
}

PSVP_TEST(stream_keeps_loss_reports_it_cannot_send_yet) {
    auto options = media_options(ConsoleKind::ps5);
    options.frames = 0;
    options.lose_every = 2;
    options.lose_units = 3;
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), options);
    link.start();
    link.run_until_settled();
    // Nothing of the client's reaches the console any more, so nothing it sends is acknowledged.
    link.lose = [](bool to_console, const std::vector<std::uint8_t>&) { return to_console; };
    link.run(2000ms);
    // Sixteen messages wait; the stream goes on, and what could not be reported is kept as ranges.
    CHECK(link.client.state() == ClientStream::State::ready);
    CHECK(link.client.video().frames_lost > 40);
    CHECK(link.client.counters().loss_reports + link.client.counters().heartbeats_sent == Messenger::kWindow);
    // A console that stays silent ends the stream once those messages were sent again often enough.
    link.run(5000ms);
    CHECK(link.client.state() == ClientStream::State::failed);
    CHECK(link.client.reason() == "the console stopped acknowledging the stream's messages");
}

PSVP_TEST(stream_takes_what_the_console_says_on_its_own) {
    // The message, byte for byte: type 16, then field 17 with the six values in their codings.
    ConnectionQuality quality;
    quality.target_bitrate = 300;
    quality.upstream_bitrate = 2;
    quality.upstream_loss = 0.5F;
    quality.disable_upstream_audio = true;
    quality.rtt = 2.0;
    quality.loss = 7;
    const auto bytes = encode_message(make_message(quality));
    CHECK(hex(bytes) == "0810" "8a0117"              // type 16; field 17, 23 bytes (3 + 2 + 5 + 2 + 9 + 2)
                        "08ac02" "1002"              // 300, 2
                        "1d0000003f"                 // 0.5 in four bytes, least significant first
                        "2001"                       // true
                        "290000000000000040"         // 2.0 in eight bytes
                        "3007");
    const auto back = parse_message(bytes);
    CHECK(back && back->as<ConnectionQuality>() != nullptr && *back->as<ConnectionQuality>() == quality);
    CHECK(parse_message(encode_message(make_message(ConnectionQuality{}))).value().as<ConnectionQuality>() != nullptr);
    // A value in another coding than its field's is not this message.
    CHECK(!parse_message(std::vector<std::uint8_t>{0x08, 0x10, 0x8a, 0x01, 0x02, 0x18, 0x01}));

    // A console that sends heartbeats of its own and its view of the connection, as a PS5 does.
    auto options = StreamLink::options(ConsoleKind::ps5);
    options.heartbeat_every = 330ms;
    options.quality_every = 1200ms;
    StreamLink link(StreamLink::request(ConsoleKind::ps5, kMtuCeiling), options);
    link.start();
    link.run_until_settled();
    CHECK(!link.client.quality());
    // From 6 ms on, when the client's stream information arrived: heartbeats at 336, 666, ... and the view at 1206, 2406.
    link.run(2500ms);
    const StreamCounters& counters = link.client.counters();
    CHECK(counters.messages.at(message_type::kHeartbeat) == 7 && counters.messages.at(message_type::kConnectionQuality) == 2);
    CHECK(link.client.quality() && link.client.quality()->target_bitrate == 9708000U && link.client.quality()->rtt == 200.0);
    // The heartbeats are counted, not kept: what is kept is the controller information, the answer and the two views.
    const std::vector<ConsoleData> data = link.client.take_console_data();
    CHECK(data.size() == 4 && data[2].message && data[2].kind == message_type::kConnectionQuality && data[2].channel == 0);
    CHECK(link.client.state() == ClientStream::State::ready && link.console.summary().find("out-of-step 0") != std::string::npos);
}

PSVP_TEST(stream_tells_the_console_what_the_controller_does) {
    StreamLink link;
    Buttons buttons;
    buttons.set(Button::cross, true);
    // Before the stream is set up nothing is sent.
    CHECK(link.client.set_controller(buttons, InputState{}, link.now).empty());
    link.start();
    link.run_until_settled();
    link.run(96ms);  // to 100 ms
    // Now the same state is news to the console.
    link.post(true, link.client.set_controller(buttons, InputState{}, link.now));
    link.run(10ms);
    CHECK(link.has("input 0: cross pressed"));
    buttons.set(Button::cross, false);
    buttons.set(Button::ps, true);
    buttons.r2 = 200;
    link.post(true, link.client.set_controller(buttons, InputState{}, link.now));
    link.run(300ms);
    const std::vector<std::string> expected{"input 0: cross pressed", "input 1: cross released", "input 2: ps pressed", "input 3: r2 at 200"};
    std::vector<std::string> told;
    for (const std::string& line : link.events) {
        if (line.starts_with("input ")) told.push_back(line);
    }
    CHECK(told == expected);
    CHECK(link.client.counters().input_events == 4);
    // One packet per event. The first one was not sent again: the next change came 10 ms after it,
    // before its 20 ms were over. The last one was, four times.
    CHECK(link.client.counters().history_packets == 1 + 3 + 4);
    CHECK(link.console.summary().find(" history-packets 8 events 4 unreadable 0") != std::string::npos);
    CHECK(link.console.summary().find("out-of-step 0") != std::string::npos);
    CHECK(link.console.summary().find("REJECTED") == std::string::npos);
}

PSVP_TEST(stream_repeats_cover_a_lost_history_packet) {
    StreamLink link;
    link.start();
    link.run_until_settled();
    // The first two history packets are lost: the press's and the release's. (A packet that is sent
    // again is sealed anew, so it is told from the first by the count, not by its bytes.)
    int history_packets = 0;
    link.lose = [&](bool to_console, const std::vector<std::uint8_t>& datagram) {
        return to_console && (datagram[0] & 0x0f) == packet_kind::kInputHistory && ++history_packets <= 2;
    };
    Buttons buttons;
    buttons.set(Button::left, true);
    link.post(true, link.client.set_controller(buttons, InputState{}, link.now));
    link.run(5ms);
    buttons.set(Button::left, false);
    link.post(true, link.client.set_controller(buttons, InputState{}, link.now));
    link.run(15ms);
    // Neither has arrived yet: the release's packet goes out again 20 ms after it was first sent.
    CHECK(link.count("input ") == 0);
    link.run(10ms);
    // That one packet tells both, in order: it carries the press again.
    CHECK(link.has("input 0: left pressed") && link.has("input 1: left released") && link.count("input ") == 2);
    link.run(300ms);
    CHECK(link.count("input ") == 2);
}

PSVP_TEST(stream_sends_the_sticks_when_they_move_but_not_all_the_time) {
    StreamLink link;
    link.start();
    link.run_until_settled();
    link.run(96ms);  // to 100 ms; nothing but resting states so far, none yet (the first is due at 204 ms)
    CHECK(link.client.counters().input_states == 0);
    InputState state;
    // The stick moves every millisecond for twenty: a state goes out at once, then at most every 8 ms,
    // and what was held back goes out when its 8 ms are over: at 100, 108, 116 and 124 ms.
    for (int i = 0; i <= 20; ++i) {
        state.left_x = static_cast<std::int16_t>(1000 + i);
        link.post(true, link.client.set_controller(Buttons{}, state, link.now));
        if (i < 20) link.run(1ms);
    }
    CHECK(link.client.counters().input_states == 3);
    link.run(10ms);
    CHECK(link.client.counters().input_states == 4);
    // Then every 200 ms from the last one: 324 ms.
    link.run(189ms);
    CHECK(link.client.counters().input_states == 4);
    link.run(10ms);
    CHECK(link.client.counters().input_states == 5);
    // The console read them all, in order; none of them is a resting controller's.
    CHECK(link.console.summary().find(" input-states 5 at-rest 0 unreadable 0 out-of-order 0 ") != std::string::npos);
}

PSVP_TEST(stream_closes_when_the_console_says_goodbye) {
    StreamLink link;
    link.start();
    link.run_until_settled();
    link.post(false, link.console.disconnect("Server shutting down", link.now));
    link.run(100ms);
    CHECK(link.client.state() == ClientStream::State::closed);
    CHECK(link.client.closed_by_console() && link.client.reason() == "Server shutting down");
    CHECK(!link.client.goodbye_pending());
    // Nothing more is sent on its own.
    link.run(3000ms);
    CHECK(link.client.counters().heartbeats_sent == 0);
}

PSVP_TEST(stream_messages_of_the_setup_read_back) {
    StreamInfo info;
    info.resolutions.push_back(StreamResolution{1920, 1080, {1, 2, 3}});
    info.resolutions.push_back(StreamResolution{1280, 720, {}});
    info.audio_header = encode_audio_header(AudioHeader{2, 16, 48000, 480, 1});
    info.start_timeout = 30;
    info.congestion_control_interval = 200;
    const StreamMessage message = make_message(info);
    CHECK(message.type == message_type::kStreamInfo);
    const auto back = parse_message(encode_message(message));
    CHECK(back && *back == message);
    // No more than eight picture sizes are kept.
    StreamInfo many;
    many.audio_header = info.audio_header;
    for (std::uint32_t i = 0; i < 11; ++i) many.resolutions.push_back(StreamResolution{i, i, {}});
    const auto kept = parse_message(encode_message(make_message(many)));
    CHECK(kept && kept->as<StreamInfo>() != nullptr && kept->as<StreamInfo>()->resolutions.size() == StreamInfo::kMaxResolutions);
    // The sound format is required; a picture size needs all three of its fields.
    CHECK(!parse_message(std::vector<std::uint8_t>{0x08, 0x0d, 0x7a, 0x02, 0x18, 0x1e}));
    CHECK(!parse_message(std::vector<std::uint8_t>{0x08, 0x0d, 0x7a, 0x06, 0x0a, 0x02, 0x08, 0x01, 0x12, 0x00}));

    ControllerConnection controller;
    controller.connected = false;
    controller.controller_id = 2;
    controller.type = controller_type::kDualSense;
    const auto controller_back = parse_message(encode_message(make_message(controller)));
    CHECK(controller_back && *controller_back == make_message(controller));
    CHECK(hex(encode_message(make_plain_message(message_type::kHeartbeat))) == "0803");
    CHECK(hex(encode_message(make_plain_message(message_type::kKeyframeRequest))) == "0819");

    CHECK(hex(encode_audio_header(AudioHeader{2, 16, 48000, 480, 1})) == "0210" "0000bb80" "000001e0" "00000001");
    CHECK((parse_audio_header(encode_audio_header(AudioHeader{1, 16, 48000, 480, 1})) == AudioHeader{1, 16, 48000, 480, 1}));
    CHECK(!parse_audio_header(std::vector<std::uint8_t>(13, 0)));
    CHECK(!parse_audio_header(std::vector<std::uint8_t>(15, 0)));
}
