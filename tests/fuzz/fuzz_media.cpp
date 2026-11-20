// Fuzz target for the stream's picture and sound: any bytes as a packet of
// either kind for both families, as a decrypted payload for the frame
// assembler and the sound receiver, and as a sealed packet of a stream that is
// set up, whose payload then decrypts to anything.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "proto/erasure.h"
#include "proto/frame_assembler.h"
#include "proto/input.h"
#include "proto/media_packet.h"
#include "proto/sound.h"
#include "proto/stream_setup.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

using namespace psvp::proto;
using namespace psvp::proto::transport;

constexpr std::uint32_t kClientTag = 0x01020304;
constexpr std::uint32_t kConsoleTag = 0x0a0b0c0d;

struct Fixture {
    KeyExchange client_keys;
    KeyExchange console_keys;
    RpCipher session_cipher;
    Key16 handshake_key{};
    SharedSecret secret{};
};

const Fixture& fixture() {
    static const Fixture made = [] {
        std::array<std::uint8_t, 32> a{};
        std::array<std::uint8_t, 32> b{};
        for (std::size_t i = 0; i < 32; ++i) {
            a[i] = static_cast<std::uint8_t>(1 + i);
            b[i] = static_cast<std::uint8_t>(0xa1 + i);
        }
        Key16 nonce{};
        Key16 registration{};
        Key16 handshake{};
        for (std::size_t i = 0; i < 16; ++i) {
            nonce[i] = static_cast<std::uint8_t>(i * 17);
            registration[i] = static_cast<std::uint8_t>(0xa0 + i);
            handshake[i] = static_cast<std::uint8_t>(0x10 + i);
        }
        Fixture f{KeyExchange::from_private_key(a).value(), KeyExchange::from_private_key(b).value(),
                  make_session_cipher(ConsoleKind::ps5, nonce, registration), handshake, {}};
        f.secret = f.client_keys.shared_secret(f.console_keys.public_key()).value();
        return f;
    }();
    return made;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    const TimePoint start{};

    // The bytes as one packet, read and written back.
    for (const ConsoleKind kind : {ConsoleKind::ps5, ConsoleKind::ps4}) {
        if (const auto packet = parse_stream_media(kind, input)) {
            const auto written = encode_stream_media(kind, *packet, packet->payload);
            if (!written || written->size() != size) std::abort();
            const auto again = parse_stream_media(kind, *written);
            if (!again || again->frame_index != packet->frame_index || again->unit_index != packet->unit_index ||
                again->units_total != packet->units_total || again->payload.size() != packet->payload.size()) {
                std::abort();
            }
        }
    }

    // The bytes as a row of packets whose payloads are taken as they are: two length bytes, then the packet.
    // Every frame handed on is counted once, and nothing is handed on twice.
    FrameAssembler assembler;
    SoundReceiver sound;
    FrameOutput frames;
    std::vector<SoundFrame> sound_frames;
    TimePoint now = start;
    std::size_t at = 0;
    while (at + 2 <= size) {
        const std::size_t length = (std::size_t{data[at]} << 8 | data[at + 1]) % 1500;
        at += 2;
        if (length > size - at) break;
        const auto bytes = input.subspan(at, length);
        at += length;
        now += std::chrono::milliseconds(length % 7);
        if (const auto packet = parse_stream_media(length % 2 == 0 ? ConsoleKind::ps5 : ConsoleKind::ps4, bytes)) {
            if (packet->video) assembler.on_packet(*packet, packet->payload, now, frames);
            else sound.on_packet(*packet, packet->payload, sound_frames);
        }
        if (const auto deadline = assembler.deadline(); deadline && *deadline <= now) assembler.on_timeout(now, frames);
    }
    assembler.on_timeout(now + std::chrono::seconds(1), frames);
    if (assembler.deadline()) std::abort();  // nothing is left waiting once every wait has run out
    if (frames.frames.size() != assembler.counters().frames) std::abort();
    std::uint64_t lost = 0;
    for (const FrameRange& range : frames.lost) lost += static_cast<std::uint16_t>(range.last - range.first) + 1U;
    if (lost != assembler.counters().frames_lost) std::abort();
    for (std::size_t i = 1; i < frames.frames.size(); ++i) {
        // In order: each frame comes after the one before it.
        const auto ahead = static_cast<std::uint16_t>(frames.frames[i].index - frames.frames[i - 1].index);
        if (ahead == 0 || ahead >= 0x8000U) std::abort();
    }
    if (sound_frames.size() != sound.counters().frames) std::abort();

    // The bytes as the events of an input history packet: what reads back is the same events when written again.
    if (const auto events = parse_history_events(input)) {
        std::vector<std::uint8_t> written;
        for (const ReadEvent& event : *events) {
            const HistoryEvent again = event.what == ReadEvent::What::button    ? button_event(event.button, event.pressed)
                                       : event.what == ReadEvent::What::trigger ? trigger_event(event.right, event.value)
                                                                                : touch_event(event.touch);
            written.insert(written.end(), again.data().begin(), again.data().end());
            (void)describe(event);
        }
        if (written.size() != size || !std::equal(written.begin(), written.end(), data)) std::abort();
    }
    for (const ConsoleKind kind : {ConsoleKind::ps5, ConsoleKind::ps4}) (void)parse_input_state(kind, input);

    // The repair code on whatever sizes the bytes name.
    if (size >= 4) {
        const unsigned source = 1 + data[0] % 20;
        const unsigned repair = data[1] % 6;
        const std::size_t unit = 1 + data[2] % 40;
        std::vector<std::uint8_t> units((source + repair) * unit, 0);
        for (std::size_t i = 0; i < source * unit; ++i) units[i] = data[3 + i % (size - 3)];
        const std::vector<std::uint8_t> whole_source(units.begin(), units.begin() + static_cast<std::ptrdiff_t>(source * unit));
        if (repair > 0 && !make_repair_units(units, unit, unit, source, repair)) std::abort();
        // Lose as many units as there are repair units, chosen by the bytes; the frame must come back.
        std::vector<unsigned> missing;
        for (unsigned i = 0; i < source + repair && missing.size() < repair; ++i) {
            if ((data[3 + i % (size - 3)] & 1) != 0) missing.push_back(i);
        }
        for (const unsigned index : missing) std::fill_n(units.begin() + static_cast<std::ptrdiff_t>(index * unit), unit, std::uint8_t{0xee});
        if (!repair_units(units, unit, unit, source, repair, missing)) std::abort();
        if (!std::equal(whole_source.begin(), whole_source.end(), units.begin())) std::abort();
    }

    // A stream that is set up takes the bytes as a sealed picture packet and as a sealed sound packet:
    // the code is right, and what the payload decrypts to is anything.
    const Fixture& f = fixture();
    StreamRequest request;
    request.session_id = "MockSessionIdent";
    request.session_cipher = f.session_cipher;
    ConsoleStream::Options options;
    options.session_cipher = f.session_cipher;
    options.session_id = "MockSessionIdent";
    options.resolutions.push_back(StreamResolution{1280, 720, {1, 2, 3}});
    const Endpoint console_end{kConsoleTag, kReceiveWindow, kStreams, kStreams, kConsoleTag};
    const Endpoint client_end{kClientTag, kReceiveWindow, kStreams, kStreams, kClientTag};
    ClientStream client(kClientTag, console_end, request, f.client_keys, f.handshake_key);
    ConsoleStream console(kConsoleTag, client_end, options, f.console_keys);
    Datagrams to_console = client.start(start);
    for (int round = 0; round < 6 && !to_console.empty(); ++round) {
        Datagrams to_client;
        for (const auto& datagram : to_console) {
            for (auto& answer : console.on_datagram(datagram, start)) to_client.push_back(std::move(answer));
        }
        to_console.clear();
        for (const auto& datagram : to_client) {
            for (auto& answer : client.on_datagram(datagram, start)) to_console.push_back(std::move(answer));
        }
    }
    if (client.state() != ClientStream::State::ready) std::abort();
    const StreamCipher console_cipher(kConsoleDirection, f.handshake_key, f.secret);
    std::uint64_t position = 1 << 20;
    for (const std::uint8_t kind : {packet_kind::kVideo, packet_kind::kAudio}) {
        if (size < 26 || size > 1500) break;
        std::vector<std::uint8_t> sealed(input.begin(), input.end());
        sealed[0] = static_cast<std::uint8_t>((sealed[0] & 0x10) | kind);
        if (!seal_datagram(console_cipher, position, sealed)) std::abort();
        position += 4096;
        const std::uint64_t before = client.counters().video_packets + client.counters().audio_packets;
        (void)client.on_datagram(sealed, start);
        if (client.counters().video_packets + client.counters().audio_packets != before + 1) std::abort();
    }
    (void)client.on_timeout(client.deadline());
    (void)client.take_media();
    return 0;
}
