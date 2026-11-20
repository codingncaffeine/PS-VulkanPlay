// A stand-in for a console, for tests: it answers discovery, takes a wake-up,
// accepts a registration and sets a stream up the way the protocol pages
// describe a console doing it. It is not a console: the stream it sets up
// carries no picture and no sound.
//
//   psvp-mock-console [options]
//     --kind ps5|ps4          which family to stand in for (default ps5)
//     --name NAME             the console's name (default "Mock Console")
//     --state ready|standby   on, or in rest mode (default ready)
//     --pin NUMBER            the eight-digit PIN it "shows"; without one it takes no registration
//     --account-id BASE64     only this account may register (default: any)
//     --regist-key KEY        the registration key it hands out and wakes for (default 1a2b3c4d)
//     --system-version TEXT   as discovery reports it (default 09000004 or 12020000)
//     --bind ADDRESS          where it listens (default 127.0.0.1; 0.0.0.0 to hear broadcasts)
//     --dump DIR              keep every registration request it receives in DIR
//     --ready-file FILE       created once it is listening
//     --session-nonce HEX     the nonce it hands out for a session (default: random)
//     --passcode DIGITS       ask for this login passcode before it names the session
//     --in-use yes            refuse a session because Remote Play is already in use
//     --heartbeat-ms N        how often it asks the client for a heartbeat (default 500)
//     --mtu-in N              the largest packet that reaches the client (default 1454)
//     --mtu-out N             the largest packet that reaches the console (default 1454)
//     --probe no              complete the probe port's handshake, then stay silent
//     --stream no             complete the stream port's handshake, then stay silent
//     --refuse-version yes    answer the stream's opening message with "version not accepted"
//     --wrong-signature yes   sign its public key with a key that is not the handshake key
//     --media-ms N            once a stream is set up, send a sound packet every N ms
//     --haptics-every N       with every Nth sound packet one for the haptics (PS5)
//     --lose-sound-every N    do not send the sound packets whose frame index N divides
//     --frame-ms N            send a picture frame every N ms
//     --frames N              stop after N picture frames (default: go on)
//     --media-seed N          what the frames hold follows from this number and their index
//     --frame-bytes N         a picture frame is half to one and a half times this (default 6000)
//     --repair-units N        repair packets per picture frame (default 2)
//     --lose-every N          the picture frames whose index N divides lose packets...
//     --lose-units N          ...this many, from their first on
//     --late-every N          the last packet of such a frame goes out behind the next frame's first
//     --corrupt-media yes     change a byte of each picture and sound packet after sealing it
//
//   psvp-mock-console --check-dump FOLDER [--media-seed N] [--frame-bytes N]
//     compares what 'session --dump FOLDER' kept with what a mock with these options sends;
//     prints what it found and exits 0 when every frame kept is one that was sent
//
// It listens on the consoles' real ports, so it needs a network of its own
// (tools/wall.sh, or a container) and the right to open port 987 for a PS4.
// One line per event goes to standard output.
#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>

#include "net/socket.h"
#include "proto/base64.h"
#include "proto/discovery.h"
#include "proto/hex.h"
#include "proto/regist.h"
#include "proto/rp_cipher.h"
#include "proto/key_exchange.h"
#include "proto/probe.h"
#include "proto/session.h"
#include "proto/stream_setup.h"
#include "proto/text_message.h"
#include "proto/transport.h"

namespace {

using namespace std::chrono_literals;
using psvp::net::Clock;
using psvp::proto::ConsoleKind;
using psvp::proto::ConsoleState;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

template <class... Args>
void event(std::format_string<Args...> format, Args&&... args) {
    std::fputs(("mock: " + std::format(format, std::forward<Args>(args)...) + "\n").c_str(), stdout);
    std::fflush(stdout);
}

struct Options {
    ConsoleKind kind = ConsoleKind::ps5;
    std::string name = "Mock Console";
    ConsoleState state = ConsoleState::ready;
    std::optional<std::uint32_t> pin;
    std::string account_id;
    std::string regist_key = "1a2b3c4d";
    std::string system_version;
    std::uint32_t bind_ip = psvp::net::kLoopbackIp;
    std::string dump_dir;
    std::string ready_file;
    std::optional<psvp::proto::Key16> session_nonce;
    std::string passcode;
    bool in_use = false;
    int heartbeat_ms = 500;
    psvp::proto::transport::ConsoleProbe::Path path;
    bool probe = true;
    bool stream = true;
    bool refuse_version = false;
    bool wrong_signature = false;
    int media_ms = 0;
    bool corrupt_media = false;
    psvp::proto::transport::ConsoleStream::Options media;  // the picture and sound it sends
    int frame_ms = 0;
    std::string check_dump;
};

// The header of the one picture size the mock offers: a placeholder, not a decoder's.
constexpr std::array<std::uint8_t, 8> kPictureHeader{0x00, 0x00, 0x00, 0x01, 0x4d, 0x4f, 0x43, 0x4b};

// The control channel of the one session the mock can hold.
struct Control {
    psvp::net::TcpStream stream;
    std::optional<psvp::proto::CtrlChannel> channel;
    Clock::time_point next_heartbeat;
    int heartbeats_answered = 0;
    bool session_named = false;
};

constexpr std::array<std::uint8_t, 6> kMac{0x02, 0x00, 0x5e, 0x10, 0x20, 0x30};
constexpr psvp::proto::Key16 kSecret{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
// The reason a console gives when a registration fails.
constexpr std::string_view kReasonRegistFailed = "80108b09";

std::string upper_hex(std::span<const std::uint8_t> bytes) {
    std::string text = psvp::proto::hex_encode(bytes);
    std::ranges::transform(text, text.begin(), [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
    return text;
}

std::string discovery_reply(const Options& options, ConsoleState state) {
    const std::string family(psvp::proto::to_string(options.kind));
    std::string text = state == ConsoleState::ready ? "HTTP/1.1 200 Ok\n" : "HTTP/1.1 620 Server Standby\n";
    text += std::format("host-id:{}\nhost-type:{}\nhost-name:{}\nhost-request-port:997\n"
                        "device-discovery-protocol-version:{}\nsystem-version:{}\n",
                        upper_hex(kMac), family, options.name, psvp::proto::discovery_version(options.kind), options.system_version);
    return text;
}

std::string refusal() {
    return std::format("HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: {}\r\nContent-Length: 0\r\n\r\n", kReasonRegistFailed);
}

// Reads the rest of a registration request and answers it. Returns the line to log.
std::string serve_registration(const Options& options, psvp::net::TcpStream& stream, const std::string& head,
                               std::vector<std::uint8_t> data, int number) {
    using namespace psvp::proto::regist_layout;
    const auto deadline = Clock::now() + 3s;
    std::size_t body_size = 0;
    {
        const std::size_t field = head.find("Content-Length: ");
        if (field == std::string::npos) return "registration without a length";
        std::from_chars(head.data() + field + 16, head.data() + head.size(), body_size);
        if (body_size == 0 || body_size > 4096) return "registration with a bad length";
    }
    while (data.size() < body_size) {
        std::uint8_t chunk[1024];
        const auto got = stream.receive(chunk, deadline);
        if (!got || *got == 0) return "registration cut short";
        data.insert(data.end(), chunk, chunk + *got);
        if (data.size() > 8192) return "registration too long";
    }
    const std::span<const std::uint8_t> payload(data.data(), body_size);
    if (!options.dump_dir.empty()) {
        std::ofstream(std::format("{}/regist-{}.head", options.dump_dir, number), std::ios::binary) << head;
        std::ofstream(std::format("{}/regist-{}.payload", options.dump_dir, number), std::ios::binary)
            .write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    const auto answer = [&](const std::string& response) {
        (void)stream.send_all(psvp::net::bytes_of(response), Clock::now() + 3s);
    };
    const std::string path = options.kind == ConsoleKind::ps5 ? "POST /sie/ps5/rp/sess/rgst " : "POST /sie/ps4/rp/sess/rgst ";
    if (!head.starts_with(path)) {
        answer(refusal());
        return "registration refused: not this console's request path";
    }
    if (!options.pin || payload.size() <= kInnerOffset) {
        answer(refusal());
        return "registration refused: no PIN is being shown";
    }

    // The request says which table entries it used and carries the wrapped nonce.
    const std::size_t key_offset = payload[kKeyOffsetByte] & 0x1f;
    const std::size_t wrap_offset = payload[kWrapOffsetByte] >> 3;
    psvp::proto::Key16 wrapped{};
    std::copy_n(payload.begin() + kWrappedLowAt, 8, wrapped.begin());
    std::copy_n(payload.begin() + kWrappedHighAt, 8, wrapped.begin() + 8);
    const auto nonce = psvp::proto::unwrap_nonce(options.kind, wrapped, wrap_offset);
    const auto cipher = nonce ? psvp::proto::make_regist_cipher(options.kind, *nonce, key_offset, *options.pin) : std::nullopt;
    if (!cipher) {
        answer(refusal());
        return "registration refused: bad table offsets";
    }
    const std::vector<std::uint8_t> opened = psvp::proto::decrypt(*cipher, 0, payload.subspan(kInnerOffset));
    const auto inner = psvp::proto::parse_header_block(std::string_view(reinterpret_cast<const char*>(opened.data()), opened.size()));
    const auto account = inner ? inner->find("Np-AccountId") : std::nullopt;
    if (!inner || !inner->find("Client-Type") || !account) {
        // With the wrong PIN the text does not open to anything readable.
        answer(refusal());
        return "registration refused: wrong PIN";
    }
    if (!options.account_id.empty() && *account != options.account_id) {
        answer(refusal());
        return std::format("registration refused: account {} is not this console's", *account);
    }

    const std::string family(psvp::proto::to_string(options.kind));
    const std::string mac = psvp::proto::hex_encode(kMac);
    const std::string regist_key = psvp::proto::hex_encode(psvp::net::bytes_of(options.regist_key));
    const std::string secret = psvp::proto::hex_encode(kSecret);
    // The PS5 lines are the ones a PS5 on system software 14.23 sent, in its order.
    // The PS4 lines have only been read in other clients.
    const std::string body =
        options.kind == psvp::proto::ConsoleKind::ps5
            ? std::format("AP-Bssid: 0000000000\r\n"
                          "AP-Name: PS5\r\n"
                          "PS5-Mac: {}\r\n"
                          "PS5-RegistKey: {}\r\n"
                          "PS5-Nickname: {}\r\n"
                          "RP-KeyType: 2\r\n"
                          "RP-Key: {}\r\n"
                          "RP-SupportCmd: 142300\r\n",
                          mac, regist_key, options.name, secret)
            : std::format("AP-Ssid: {0}-MOCK\r\n"
                          "AP-Bssid: {1}\r\n"
                          "AP-Key: mockapkey\r\n"
                          "AP-Name: {0}\r\n"
                          "{0}-Mac: {1}\r\n"
                          "{0}-RegistKey: {2}\r\n"
                          "{0}-Nickname: {3}\r\n"
                          "RP-KeyType: 2\r\n"
                          "RP-Key: {4}\r\n",
                          family, mac, regist_key, options.name, secret);
    const std::vector<std::uint8_t> sealed = psvp::proto::encrypt(*cipher, 0, psvp::net::bytes_of(body));
    // A console pads the length with spaces to eleven characters.
    std::string response =
        std::format("HTTP/1.1 200 OK\r\nConnection: close\r\nRP-Feature: 3\r\nContent-Length: {:>10}\r\n\r\n", sealed.size());
    response.append(reinterpret_cast<const char*>(sealed.data()), sealed.size());
    answer(response);
    return std::format("registration accepted: account {} key-offset {} wrap-offset {}", *account, key_offset, wrap_offset);
}

std::string header_names(const psvp::proto::TextMessage& message) {
    std::string names;
    for (const auto& header : message.headers) names += (names.empty() ? "" : ",") + header.name;
    return names;
}

// The headers of a request: everything after its first line.
std::optional<psvp::proto::TextMessage> request_headers(const std::string& head) {
    const std::size_t line_end = head.find('\n');
    if (line_end == std::string::npos) return std::nullopt;
    const auto message = psvp::proto::parse_header_block(std::string_view(head).substr(line_end + 1));
    if (!message) return std::nullopt;
    return *message;
}

// Answers a session request. Sets the session cipher when it accepts.
std::string serve_session_request(const Options& options, psvp::net::TcpStream& stream, const std::string& head,
                                  std::optional<psvp::proto::RpCipher>& session_cipher) {
    const auto answer = [&](const std::string& response) {
        (void)stream.send_all(psvp::net::bytes_of(response), Clock::now() + 3s);
    };
    const auto refuse = [&](std::string_view reason) {
        answer(std::format("HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: {}\r\nRP-Version: {}\r\n\r\n", reason,
                           psvp::proto::rp_version(options.kind)));
    };
    const auto headers = request_headers(head);
    const auto key = headers ? headers->find("RP-Registkey") : std::nullopt;
    const auto version = headers ? headers->find("Rp-Version") : std::nullopt;
    if (!headers || !key || !version) {
        refuse("80108bff");
        return "session request refused: not readable";
    }
    if (options.in_use) {
        refuse("80108b10");
        return "session request refused: already in use";
    }
    if (*version != psvp::proto::rp_version(options.kind)) {
        refuse("80108b11");
        return std::format("session request refused: version {}", *version);
    }
    if (*key != psvp::proto::hex_encode(psvp::net::bytes_of(options.regist_key))) {
        refuse(kReasonRegistFailed);
        return "session request refused: not a registered client";
    }
    const psvp::proto::Key16 nonce = options.session_nonce ? *options.session_nonce : psvp::proto::random_key();
    session_cipher = psvp::proto::make_session_cipher(options.kind, nonce, kSecret);
    answer(std::format("HTTP/1.1 200 OK\r\nRP-Nonce: {}\r\nRP-Version: {}\r\n\r\n", psvp::proto::base64_encode(nonce),
                       psvp::proto::rp_version(options.kind)));
    return std::format("session request accepted: registration key ok version {} headers {}", *version, header_names(*headers));
}

// Answers the control channel's opening request. On success the stream becomes
// the control connection.
std::string serve_control_request(const Options& options, psvp::net::TcpStream& stream, const std::string& head,
                                  const std::optional<psvp::proto::RpCipher>& session_cipher, Control& control) {
    const auto refuse = [&]() {
        (void)stream.send_all(psvp::net::bytes_of(std::string("HTTP/1.1 403 Forbidden\r\n\r\n")), Clock::now() + 3s);
    };
    const auto headers = request_headers(head);
    if (!headers || !session_cipher) {
        refuse();
        return "control request refused: no session was requested first";
    }
    const psvp::proto::RpCipher& cipher = *session_cipher;
    // Each encrypted header uses the client's next number, in the order they are written.
    std::uint64_t number = 0;
    const auto opened = [&](std::string_view name) -> std::optional<std::vector<std::uint8_t>> {
        const auto value = headers->find(name);
        const auto sealed = value ? psvp::proto::base64_decode(*value) : std::unexpected(psvp::proto::Base64Error::bad_length);
        if (!sealed) return std::nullopt;
        return psvp::proto::decrypt(cipher, number++, *sealed);
    };
    const auto auth = opened("RP-Auth");
    const auto did = opened("RP-Did");
    const auto os_type = opened("RP-OSType");
    const auto bitrate = opened("RP-StartBitrate");
    const auto streaming = options.kind == ConsoleKind::ps5 ? opened("RP-StreamingType") : std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{});
    if (!auth || !did || !os_type || !bitrate || !streaming) {
        refuse();
        return "control request refused: an encrypted header is missing";
    }
    std::vector<std::uint8_t> expected_auth(16, 0);
    std::copy_n(options.regist_key.begin(), std::min<std::size_t>(options.regist_key.size(), 16), expected_auth.begin());
    if (*auth != expected_auth) {
        refuse();
        return "control request refused: the registration key did not open (wrong secret or nonce)";
    }
    const psvp::proto::DeviceId shape = psvp::proto::make_device_id(psvp::proto::Key16{});
    const bool did_ok = did->size() == shape.size() && std::equal(did->begin(), did->begin() + 10, shape.begin()) &&
                        std::all_of(did->end() - 6, did->end(), [](std::uint8_t b) { return b == 0; });
    const std::string os_text(os_type->begin(), os_type->end());
    const auto plain = [&](std::string_view name) { return std::string(headers->find(name).value_or("-")); };

    std::array<std::uint8_t, 16> server_type{};
    server_type[0] = options.kind == ConsoleKind::ps5 ? 2 : 0;
    const std::string response = std::format("HTTP/1.1 200 OK\r\nRP-Server-Type: {}\r\n\r\n",
                                             psvp::proto::base64_encode(psvp::proto::encrypt(cipher, 0, server_type)));
    (void)stream.send_all(psvp::net::bytes_of(response), Clock::now() + 3s);

    control.channel.emplace(cipher, 1, number);
    control.next_heartbeat = Clock::now() + std::chrono::milliseconds(options.heartbeat_ms);
    control.heartbeats_answered = 0;
    control.session_named = false;
    return std::format("control request accepted: auth ok device-id {} os-type {} bitrate {} streaming-type {} version {} "
                       "controller-type {} client-type {} con-path {} pad-proc-no {} headers {}",
                       did_ok ? "ok" : "BAD", psvp::proto::hex_encode(psvp::net::bytes_of(os_text)),
                       psvp::proto::hex_encode(*bitrate), psvp::proto::hex_encode(*streaming), plain("RP-Version"),
                       plain("RP-ControllerType"), plain("RP-ClientType"), plain("RP-ConPath"), plain("RP-PadProcNo"),
                       header_names(*headers));
}

void send_control(Control& control, std::uint16_t type, std::span<const std::uint8_t> payload = {}) {
    if (const auto bytes = control.channel->encode(type, payload)) {
        (void)control.stream.send_all(*bytes, Clock::now() + 3s);
    }
}

// The session ID it hands out. A PS5 on system software 14.23 sent sixteen letters;
// the longer form is the one other clients describe.
std::string_view session_id(ConsoleKind kind) {
    return kind == ConsoleKind::ps5 ? "MockSessionIdent" : "MOCKSESSION0123456789abcdefghijkl";
}

void name_session(Control& control, ConsoleKind kind) {
    // The ID's length and the ID.
    const std::string id = std::string(1, static_cast<char>(session_id(kind).size())) + std::string(session_id(kind));
    send_control(control, psvp::proto::ctrl_type::kSessionId, psvp::net::bytes_of(id));
    control.session_named = true;
    event("session ID sent");
}

int usage(std::string_view problem) {
    std::fputs(std::format("psvp-mock-console: {}\n", problem).c_str(), stderr);
    return 64;
}

// The lines of a tab-separated list behind its heading, as numbers.
std::optional<std::vector<std::vector<std::uint64_t>>> read_list(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::vector<std::vector<std::uint64_t>> rows;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::vector<std::uint64_t> row;
        const char* at = line.data();
        const char* const end = at + line.size();
        while (at < end) {
            std::uint64_t number = 0;
            const auto [next, error] = std::from_chars(at, end, number);
            if (error != std::errc{}) return std::nullopt;
            row.push_back(number);
            at = next < end ? next + 1 : next;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Compares what 'session --dump' kept with what a mock with these options sends.
int check_dump(const Options& options) {
    using namespace psvp::proto::transport;
    const std::string folder = options.check_dump + "/";
    const auto video_list = read_list(folder + "video.tsv");
    const auto video = read_file(folder + "video.bin");
    const auto sound_list = read_list(folder + "sound.tsv");
    const auto sound = read_file(folder + "sound.bin");
    if (!video_list || !video || !sound_list || !sound) return usage(std::format("{}: not a folder that 'session --dump' wrote", options.check_dump));

    // Picture: every frame kept must be the frame of its index, behind the header where one is announced.
    std::size_t at = 0;
    std::uint64_t wrong = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t repaired = 0;
    std::uint64_t headers = 0;
    std::string missing;
    std::optional<std::uint64_t> first;
    std::uint64_t last = 0;
    for (const auto& row : *video_list) {
        if (row.size() != 6 || at + row[5] + row[1] > video->size()) return usage("video.tsv does not fit video.bin");
        if (row[5] != 0) {
            ++headers;
            if (row[5] != kPictureHeader.size() || !std::equal(kPictureHeader.begin(), kPictureHeader.end(), video->begin() + static_cast<std::ptrdiff_t>(at))) ++wrong;
            at += row[5];
        }
        const std::span<const std::uint8_t> kept(video->data() + at, row[1]);
        at += row[1];
        const bool keyframe = kept.size() > 4 && kept[4] == 0x65;
        const auto sent = pattern_video_frame(options.media.seed, static_cast<std::uint16_t>(row[0]), options.media.frame_bytes, keyframe);
        if (!std::ranges::equal(kept, sent)) ++wrong;
        keyframes += keyframe ? 1 : 0;
        repaired += row[2];
        if (first) {
            for (std::uint64_t gone = last + 1; gone < row[0]; ++gone) missing += std::format(" {}", gone);
        }
        if (!first) first = row[0];
        last = row[0];
    }
    if (at != video->size()) ++wrong;
    std::fputs(std::format("dump: picture frames {} first {} last {} wrong {} keyframes {} repaired {} headers {} missing:{}\n", video_list->size(),
                           first.value_or(0), last, wrong, keyframes, repaired, headers, missing.empty() ? " none" : missing)
                   .c_str(),
               stdout);

    // Sound and haptics: each a row of frames that count up.
    at = 0;
    std::uint64_t sound_wrong = 0;
    std::uint64_t counts[2] = {};
    std::uint64_t recovered = 0;
    std::uint64_t gaps = 0;
    for (const auto& row : *sound_list) {
        if (row.size() != 5 || row[2] > 1 || at + row[1] > sound->size()) return usage("sound.tsv does not fit sound.bin");
        const std::span<const std::uint8_t> kept(sound->data() + at, row[1]);
        at += row[1];
        if (!std::ranges::equal(kept, pattern_sound_frame(options.media.seed, static_cast<std::uint16_t>(row[0]), row[1], row[2] == 1))) ++sound_wrong;
        ++counts[row[2]];
        recovered += row[3];
        gaps += row[4];
    }
    if (at != sound->size()) ++sound_wrong;
    std::fputs(std::format("dump: sound frames {} haptics frames {} wrong {} from-a-later-packet {} lost {}\n", counts[0], counts[1], sound_wrong,
                           recovered, gaps)
                   .c_str(),
               stdout);
    return wrong == 0 && sound_wrong == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };
        const auto text = value();
        if (!text) return usage(std::format("{} needs a value", arg));
        if (arg == "--kind") {
            if (*text == "ps5") options.kind = ConsoleKind::ps5;
            else if (*text == "ps4") options.kind = ConsoleKind::ps4;
            else return usage("--kind is ps5 or ps4");
        } else if (arg == "--name") {
            options.name = *text;
        } else if (arg == "--state") {
            if (*text == "ready") options.state = ConsoleState::ready;
            else if (*text == "standby") options.state = ConsoleState::standby;
            else return usage("--state is ready or standby");
        } else if (arg == "--pin") {
            std::uint32_t pin = 0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), pin);
            if (error != std::errc{} || end != text->data() + text->size()) return usage("--pin is a number");
            options.pin = pin;
        } else if (arg == "--account-id") {
            options.account_id = *text;
        } else if (arg == "--regist-key") {
            options.regist_key = *text;
        } else if (arg == "--system-version") {
            options.system_version = *text;
        } else if (arg == "--bind") {
            const auto ip = psvp::net::parse_ipv4(*text);
            if (!ip) return usage("--bind is an IPv4 address");
            options.bind_ip = *ip;
        } else if (arg == "--dump") {
            options.dump_dir = *text;
        } else if (arg == "--ready-file") {
            options.ready_file = *text;
        } else if (arg == "--session-nonce") {
            const auto bytes = psvp::proto::hex_decode(*text);
            if (!bytes || bytes->size() != 16) return usage("--session-nonce is 32 hex digits");
            psvp::proto::Key16 nonce{};
            std::copy(bytes->begin(), bytes->end(), nonce.begin());
            options.session_nonce = nonce;
        } else if (arg == "--passcode") {
            options.passcode = *text;
        } else if (arg == "--in-use") {
            options.in_use = *text == "yes";
        } else if (arg == "--heartbeat-ms") {
            if (std::from_chars(text->data(), text->data() + text->size(), options.heartbeat_ms).ec != std::errc{} || options.heartbeat_ms < 10) {
                return usage("--heartbeat-ms is a number of at least 10");
            }
        } else if (arg == "--mtu-in" || arg == "--mtu-out") {
            std::uint32_t& size = arg == "--mtu-in" ? options.path.mtu_in : options.path.mtu_out;
            if (std::from_chars(text->data(), text->data() + text->size(), size).ec != std::errc{} ||
                size < psvp::proto::transport::kMtuFloor) {
                return usage(std::format("{} is a packet size of at least {}", arg, psvp::proto::transport::kMtuFloor));
            }
        } else if (arg == "--probe") {
            options.probe = *text != "no";
        } else if (arg == "--stream") {
            options.stream = *text != "no";
        } else if (arg == "--refuse-version") {
            options.refuse_version = *text == "yes";
        } else if (arg == "--wrong-signature") {
            options.wrong_signature = *text == "yes";
        } else if (arg == "--corrupt-media") {
            options.corrupt_media = *text == "yes";
        } else if (arg == "--media-ms") {
            if (std::from_chars(text->data(), text->data() + text->size(), options.media_ms).ec != std::errc{} || options.media_ms < 1) {
                return usage("--media-ms is a number of at least 1");
            }
        } else if (arg == "--check-dump") {
            options.check_dump = *text;
        } else if (arg == "--frame-ms" || arg == "--frames" || arg == "--media-seed" || arg == "--frame-bytes" || arg == "--repair-units" ||
                   arg == "--lose-every" || arg == "--lose-units" || arg == "--late-every" || arg == "--lose-sound-every" ||
                   arg == "--haptics-every") {
            std::uint64_t number = 0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), number);
            if (error != std::errc{} || end != text->data() + text->size() || number > 1000000) return usage(std::format("{} is a number", arg));
            const auto count = static_cast<unsigned>(number);
            if (arg == "--frame-ms") options.frame_ms = static_cast<int>(number);
            else if (arg == "--frames") options.media.frames = count;
            else if (arg == "--media-seed") options.media.seed = number;
            else if (arg == "--frame-bytes") options.media.frame_bytes = number;
            else if (arg == "--repair-units") options.media.repair_units = count;
            else if (arg == "--lose-every") options.media.lose_every = count;
            else if (arg == "--lose-units") options.media.lose_units = count;
            else if (arg == "--late-every") options.media.late_every = count;
            else if (arg == "--lose-sound-every") options.media.lose_sound_every = count;
            else options.media.haptics_every = count;
        } else {
            return usage(std::format("unknown option {}", arg));
        }
    }
    if (options.system_version.empty()) options.system_version = options.kind == ConsoleKind::ps5 ? "09000004" : "12020000";
    if (!options.check_dump.empty()) return check_dump(options);

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    const std::uint16_t port = psvp::proto::discovery_port(options.kind);
    auto discovery = psvp::net::UdpSocket::open(options.bind_ip, port, port, true);
    auto search = psvp::net::UdpSocket::open(options.bind_ip, psvp::proto::kSessionPort, psvp::proto::kSessionPort, true);
    auto listener = psvp::net::TcpListener::open(options.bind_ip, psvp::proto::kSessionPort);
    if (!discovery) return usage(std::format("discovery port {}: {}", port, discovery.error()));
    if (!search) return usage(std::format("session port: {}", search.error()));
    if (!listener) return usage(std::format("session port: {}", listener.error()));
    // The stream transport's two ports: the network probe and the stream.
    auto probe_port = psvp::net::UdpSocket::open(options.bind_ip, psvp::proto::transport::kProbePort, psvp::proto::transport::kProbePort, false);
    auto stream_port = psvp::net::UdpSocket::open(options.bind_ip, psvp::proto::transport::kStreamPort, psvp::proto::transport::kStreamPort, false);
    if (!probe_port) return usage(std::format("probe port: {}", probe_port.error()));
    if (!stream_port) return usage(std::format("stream port: {}", stream_port.error()));
    // One handshake per port and client address.
    std::map<std::pair<std::uint16_t, std::uint64_t>, psvp::proto::transport::ConsoleHandshake> transports;
    // The network probe of each client that finished the handshake on the probe port.
    struct Probe {
        psvp::net::Address client;
        psvp::proto::transport::ConsoleProbe probe;
    };
    std::map<std::uint64_t, Probe> probes;
    const auto log_probe = [&](Probe& probe) {
        for (const std::string& line : probe.probe.take_events()) event("probe {}", line);
    };
    // The stream of each client that finished the handshake on the stream port.
    struct Stream {
        psvp::net::Address client;
        psvp::proto::transport::ConsoleStream stream;
        bool summarized = false;
    };
    std::map<std::uint64_t, Stream> streams;
    const auto log_stream = [&](Stream& stream) {
        for (const std::string& line : stream.stream.take_events()) event("stream {}", line);
        if (stream.stream.disconnected() && !stream.summarized) {
            stream.summarized = true;
            event("stream summary: {}", stream.stream.summary());
        }
    };
    std::optional<psvp::proto::RpCipher> session_cipher;
    const auto serve_transport = [&](psvp::net::UdpSocket& socket, std::uint16_t transport_port) {
        const auto datagram = socket.receive(Clock::now());
        if (!datagram || !*datagram) return;
        const psvp::net::Address from = (*datagram)->from;
        const auto key = std::pair(transport_port, (std::uint64_t{from.ip} << 16) | from.port);
        auto found = transports.find(key);
        if (found == transports.end()) {
            std::array<std::uint8_t, psvp::proto::transport::kCookieSize> cookie{};
            const psvp::proto::Key16 half_a = psvp::proto::random_key();
            const psvp::proto::Key16 half_b = psvp::proto::random_key();
            std::copy(half_a.begin(), half_a.end(), cookie.begin());
            std::copy(half_b.begin(), half_b.end(), cookie.begin() + 16);
            const std::uint32_t tag = 0x50000000U | (std::uint32_t{half_a[0]} << 8) | half_b[0];
            found = transports.emplace(key, psvp::proto::transport::ConsoleHandshake(tag, cookie)).first;
        }
        psvp::proto::transport::ConsoleHandshake& handshake = found->second;
        if (const auto probe = probes.find(key.second); transport_port == psvp::proto::transport::kProbePort && probe != probes.end()) {
            for (const auto& out : probe->second.probe.on_datagram((*datagram)->data, Clock::now())) (void)socket.send_to(from, out);
            log_probe(probe->second);
            return;
        }
        if (const auto stream = streams.find(key.second); transport_port == psvp::proto::transport::kStreamPort && stream != streams.end()) {
            for (const auto& out : stream->second.stream.on_datagram((*datagram)->data, Clock::now())) (void)socket.send_to(from, out);
            log_stream(stream->second);
            return;
        }
        const bool was_connected = handshake.connected();
        const bool had_init = handshake.client().has_value();
        const auto answer = handshake.on_datagram((*datagram)->data);
        if (answer) (void)socket.send_to(from, *answer);
        if (!had_init && handshake.client()) {
            const auto& client = *handshake.client();
            const auto& header = *handshake.init_header();
            event("transport init on port {}: header-tag {} mac {} key-position {} flags {} window {:#x} streams {}/{} sequence-is-tag {}",
                  transport_port, header.tag, psvp::proto::hex_encode(header.mac), header.key_position, header.chunk_flags,
                  client.receive_window, client.outbound_streams, client.inbound_streams,
                  client.first_sequence == client.tag ? "yes" : "no");
        }
        if (!was_connected && handshake.connected()) {
            event("transport connected on port {}", transport_port);
            if (transport_port == psvp::proto::transport::kProbePort && options.probe) {
                probes.insert_or_assign(key.second, Probe{from, psvp::proto::transport::ConsoleProbe(handshake.tag(), *handshake.client(), options.path)});
            }
            if (transport_port == psvp::proto::transport::kStreamPort && options.stream && session_cipher) {
                psvp::proto::transport::ConsoleStream::Options stream_options = options.media;
                stream_options.frame_every = std::chrono::milliseconds(options.frame_ms);
                stream_options.kind = options.kind;
                stream_options.session_cipher = *session_cipher;
                stream_options.session_id = std::string(session_id(options.kind));
                // One picture size; its header is a placeholder, not a decoder's.
                stream_options.resolutions.push_back(psvp::proto::StreamResolution{1280, 720, {kPictureHeader.begin(), kPictureHeader.end()}});
                stream_options.max_datagram = std::min(options.path.mtu_in, options.path.mtu_out) - psvp::proto::transport::kSplitMargin;
                stream_options.accept_version = !options.refuse_version;
                stream_options.wrong_signature = options.wrong_signature;
                stream_options.media_every = std::chrono::milliseconds(options.media_ms);
                stream_options.corrupt_media = options.corrupt_media;
                streams.insert_or_assign(key.second, Stream{from, psvp::proto::transport::ConsoleStream(handshake.tag(), *handshake.client(), std::move(stream_options),
                                                                                                         psvp::proto::KeyExchange::generate())});
            }
        }
        if (!answer && was_connected) event("transport datagram on port {} after the handshake ({} bytes)", transport_port, (*datagram)->data.size());
    };

    ConsoleState state = options.state;
    int registrations = 0;
    event("listening as {} '{}' ({}) on {}, ports {} and {}", psvp::proto::to_string(options.kind), options.name,
          psvp::proto::to_string(state), psvp::net::ip_to_string(options.bind_ip), port, psvp::proto::kSessionPort);
    if (!options.ready_file.empty()) std::ofstream(options.ready_file) << "ready\n";

    Control control;
    const auto close_control = [&](std::string_view why) {
        event("control connection closed ({}; heartbeats answered: {})", why, control.heartbeats_answered);
        control.stream.close();
        control.channel.reset();
    };

    while (g_stop == 0) {
        pollfd waits[6] = {{discovery->fd(), POLLIN, 0}, {search->fd(), POLLIN, 0}, {listener->fd(), POLLIN, 0},
                           {control.stream.open() ? control.stream.fd() : -1, POLLIN, 0},
                           {probe_port->fd(), POLLIN, 0}, {stream_port->fd(), POLLIN, 0}};
        // Short waits while a stream sends on a timer of its own.
        const int ready = ::poll(waits, 6, streams.empty() ? 50 : 10);
        if (ready > 0 && (waits[4].revents & POLLIN) != 0) serve_transport(*probe_port, psvp::proto::transport::kProbePort);
        if (ready > 0 && (waits[5].revents & POLLIN) != 0) serve_transport(*stream_port, psvp::proto::transport::kStreamPort);
        for (auto& [client, probe] : probes) {
            for (const auto& out : probe.probe.on_timer(Clock::now())) (void)probe_port->send_to(probe.client, out);
        }
        for (auto& [client, stream] : streams) {
            for (const auto& out : stream.stream.on_timer(Clock::now())) (void)stream_port->send_to(stream.client, out);
        }

        // The control channel: a heartbeat request now and then, and whatever the client sends.
        if (control.stream.open() && control.channel && Clock::now() >= control.next_heartbeat) {
            send_control(control, psvp::proto::ctrl_type::kHeartbeatRequest);
            control.next_heartbeat = Clock::now() + std::chrono::milliseconds(options.heartbeat_ms);
        }
        if (ready > 0 && control.stream.open() && (waits[3].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            std::uint8_t chunk[1024];
            const auto got = control.stream.receive(chunk, Clock::now());
            if (!got || *got == 0) {
                close_control("by the client");
            } else if (!control.channel->feed(std::span(chunk).first(*got))) {
                close_control("unreadable message");
            } else {
                while (const auto message = control.channel->next()) {
                    using namespace psvp::proto::ctrl_type;
                    if (message->type == kHeartbeatReply) {
                        ++control.heartbeats_answered;
                    } else if (message->type == kLoginPinReply) {
                        const std::string given(message->payload.begin(), message->payload.end());
                        const bool right = given == options.passcode;
                        const std::uint8_t result = right ? 0 : 1;
                        send_control(control, kLoginResult, std::span(&result, 1));
                        event("passcode {}", right ? "accepted" : "refused");
                        if (right && !control.session_named) name_session(control, options.kind);
                    } else if (message->type == kGoToRest) {
                        event("rest mode requested");
                        state = ConsoleState::standby;
                    } else {
                        event("control message type {:#x} with {} bytes", message->type, message->payload.size());
                    }
                }
            }
        }
        if (ready <= 0) continue;

        if ((waits[0].revents & POLLIN) != 0) {
            const auto datagram = discovery->receive(Clock::now());
            if (datagram && *datagram) {
                const std::string_view text(reinterpret_cast<const char*>((*datagram)->data.data()), (*datagram)->data.size());
                const bool for_us = text.find(psvp::proto::discovery_version(options.kind)) != std::string_view::npos;
                if (text.starts_with("SRCH * HTTP/1.1") && for_us) {
                    (void)discovery->send_to((*datagram)->from, psvp::net::bytes_of(discovery_reply(options, state)));
                    event("search answered as {}", psvp::proto::to_string(state));
                } else if (text.starts_with("WAKEUP * HTTP/1.1") && for_us) {
                    const std::size_t field = text.find("user-credential:");
                    std::uint64_t credential = 0;
                    bool have = false;
                    if (field != std::string_view::npos) {
                        const char* begin = text.data() + field + 16;
                        const char* end = text.data() + std::min(text.find('\n', field), text.size());
                        have = std::from_chars(begin, end, credential).ec == std::errc{};
                    }
                    if (have && credential == psvp::proto::wake_credential(options.regist_key)) {
                        if (state != ConsoleState::ready) event("wake-up accepted, now on");
                        state = ConsoleState::ready;
                    } else {
                        event("wake-up refused: credential {} is not a registered client's", credential);
                    }
                } else {
                    event("datagram ignored on the discovery port ({} bytes)", text.size());
                }
            }
        }
        if ((waits[1].revents & POLLIN) != 0) {
            const auto datagram = search->receive(Clock::now());
            if (datagram && *datagram) {
                const std::string wanted = psvp::proto::regist_search_packet(options.kind);
                const std::string_view text(reinterpret_cast<const char*>((*datagram)->data.data()), (*datagram)->data.size());
                if (text == wanted && options.pin && state == ConsoleState::ready) {
                    const std::string reply = options.kind == ConsoleKind::ps5 ? "RES3" : "RES2";
                    (void)search->send_to((*datagram)->from, psvp::net::bytes_of(reply));
                    event("registration search answered");
                } else {
                    event("registration search ignored");
                }
            }
        }
        if ((waits[2].revents & POLLIN) != 0) {
            auto accepted = listener->accept(Clock::now());
            if (accepted && *accepted) {
                psvp::net::TcpStream& stream = (*accepted)->stream;
                auto head = psvp::net::read_text_head(stream, Clock::now() + 3s, 2048);
                if (!head) {
                    event("connection dropped: {}", head.error());
                } else if (head->head.starts_with("POST ")) {
                    event("{}", serve_registration(options, stream, head->head, std::move(head->rest), ++registrations));
                } else if (head->head.find("/rp/sess/init ") != std::string::npos) {
                    if (!options.dump_dir.empty()) std::ofstream(options.dump_dir + "/session-init.head", std::ios::binary) << head->head;
                    event("{}", serve_session_request(options, stream, head->head, session_cipher));
                } else if (head->head.find("/rp/sess/ctrl ") != std::string::npos) {
                    if (control.stream.open()) close_control("replaced by a new one");
                    const std::string line = serve_control_request(options, stream, head->head, session_cipher, control);
                    event("{}", line);
                    if (control.channel) {
                        control.stream = std::move(stream);
                        if (options.passcode.empty()) {
                            // A console reports the login also when it asks for no passcode.
                            const std::uint8_t logged_in = 0;
                            send_control(control, psvp::proto::ctrl_type::kLoginResult, std::span(&logged_in, 1));
                            name_session(control, options.kind);
                        }
                        else send_control(control, psvp::proto::ctrl_type::kLoginPinRequest);
                    }
                } else {
                    event("request ignored: {}", head->head.substr(0, head->head.find('\r')));
                }
            }
        }
    }
    // What the streams that never said goodbye counted.
    for (auto& [client, stream] : streams) {
        if (!stream.summarized) event("stream summary: {}", stream.stream.summary());
    }
    event("stopped");
    return 0;
}
