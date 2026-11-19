// Stream set-up: our key exchange, stream keys, packet codes, launch
// specification and message coding against the reference's.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <chiaki/base64.h>
#include <chiaki/ecdh.h>
#include <chiaki/gkcrypt.h>
#include <chiaki/launchspec.h>
#include <chiaki/log.h>
#include <chiaki/rpcrypt.h>
#include <chiaki/takion.h>

#include "oracle.h"
#include "proto/hex.h"
#include "proto/key_exchange.h"
#include "proto/launch_spec.h"
#include "proto/rp_cipher.h"
#include "proto/stream_cipher.h"
#include "proto/stream_message.h"
#include "proto/transport.h"
#include "reference_messages.h"

namespace psvp::oracle {
namespace {

using proto::ConsoleKind;
using proto::Key16;

ChiakiTarget target_of(ConsoleKind kind) { return kind == ConsoleKind::ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10; }

ChiakiCodec codec_of(proto::Codec codec) {
    return codec == proto::Codec::h264 ? CHIAKI_CODEC_H264 : codec == proto::Codec::hevc ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H265_HDR;
}

template <std::size_t N> std::array<std::uint8_t, N> random_bytes(Random& random) {
    std::array<std::uint8_t, N> bytes{};
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random.below(256));
    return bytes;
}

std::vector<std::uint8_t> random_vector(Random& random, std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random.below(256));
    return bytes;
}

ChiakiLog* quiet_log() {
    static ChiakiLog log;
    static bool ready = false;
    if (!ready) {
        chiaki_log_init(&log, 0, nullptr, nullptr);
        ready = true;
    }
    return &log;
}

std::string reference_launch_spec(const proto::LaunchSpec& spec) {
    Key16 key = spec.handshake_key;
    ChiakiLaunchSpec reference{};
    reference.target = target_of(spec.kind);
    reference.mtu = spec.mtu;
    reference.rtt = spec.round_trip_ms;
    reference.handshake_key = key.data();
    reference.width = spec.width;
    reference.height = spec.height;
    reference.max_fps = spec.max_fps;
    reference.codec = codec_of(spec.codec);
    reference.bw_kbps_sent = spec.bitrate_kbps;
    char text[2048];
    const int size = chiaki_launchspec_format(text, sizeof(text), &reference);
    return size < 0 ? std::string() : std::string(text, static_cast<std::size_t>(size));
}

// The launch specification as the reference seals it: the text and its closing
// zero, masked with the session cipher run over zero bytes, in base64.
std::string reference_sealed(ConsoleKind kind, const Key16& nonce, const Key16& secret, const std::string& text) {
    ChiakiRPCrypt crypt{};
    chiaki_rpcrypt_init_auth(&crypt, target_of(kind), nonce.data(), secret.data());
    const std::size_t size = text.size() + 1;
    std::vector<std::uint8_t> sealed(size, 0);
    chiaki_rpcrypt_encrypt(&crypt, 0, sealed.data(), sealed.data(), size);
    for (std::size_t i = 0; i < text.size(); ++i) sealed[i] ^= static_cast<std::uint8_t>(text[i]);
    std::vector<char> encoded(size * 2 + 8);
    if (chiaki_base64_encode(sealed.data(), size, encoded.data(), encoded.size()) != CHIAKI_ERR_SUCCESS) return {};
    return std::string(encoded.data());
}

proto::LaunchSpec random_spec(Random& random) {
    proto::LaunchSpec spec;
    spec.kind = random.below(2) != 0 ? ConsoleKind::ps5 : ConsoleKind::ps4;
    spec.width = random.below(4000);
    spec.height = random.below(2200);
    spec.max_fps = random.below(121);
    spec.bitrate_kbps = random.below(100000);
    spec.mtu = 576 + random.below(900);
    spec.round_trip_ms = random.below(500);
    spec.codec = static_cast<proto::Codec>(random.below(3));
    spec.handshake_key = random_bytes<16>(random);
    return spec;
}

}  // namespace

Outcome diff_stream(bool plant) {
    Outcome outcome;
    Random random(0x73747265'616d2121ULL);
    const auto compare = [&](bool equal, const char* what) {
        ++outcome.compared;
        if (!equal && ++outcome.mismatches <= 8) std::fprintf(stderr, "    MISMATCH stream %s\n", what);
    };
    // With `plant`, the first of each of these results of ours is changed by one bit before it is compared.
    bool plant_secret = plant;
    bool plant_stream = plant;
    bool plant_code = plant;
    bool plant_packet = plant;
    bool plant_spec = plant;
    bool plant_sealed = plant;
    bool plant_message = plant;
    const auto planted = [](bool& pending) { return std::exchange(pending, false); };

    // The key exchange: each side's key pair is its own library's; the two must
    // sign alike and arrive at the same secret.
    int exchanges = 0;
    for (int round = 0; round < 200; ++round) {
        const Key16 handshake_key = random_bytes<16>(random);
        ChiakiECDH reference{};
        if (chiaki_ecdh_init(&reference) != CHIAKI_ERR_SUCCESS) {
            compare(false, "reference key pair");
            continue;
        }
        std::uint8_t their_public[128];
        std::size_t their_public_size = sizeof(their_public);
        std::uint8_t their_signature[32];
        std::size_t their_signature_size = 0;
        chiaki_ecdh_get_local_pub_key(&reference, their_public, &their_public_size, handshake_key.data(), their_signature, &their_signature_size);
        compare(their_public_size == proto::kPublicKeySize, "public key size");
        const std::span<const std::uint8_t> their_key(their_public, their_public_size);
        const proto::KeySignature our_signature_of_theirs = proto::sign_public_key(handshake_key, their_key);
        compare(std::memcmp(our_signature_of_theirs.data(), their_signature, 32) == 0, "public key signature");

        const proto::KeyExchange ours = proto::KeyExchange::generate();
        const proto::KeySignature our_signature = proto::sign_public_key(handshake_key, ours.public_key());
        std::uint8_t their_secret[CHIAKI_ECDH_SECRET_SIZE];
        const bool derived = chiaki_ecdh_derive_secret(&reference, their_secret, ours.public_key().data(), ours.public_key().size(),
                                                       handshake_key.data(), our_signature.data(), our_signature.size()) == CHIAKI_ERR_SUCCESS;
        auto our_secret = ours.shared_secret(their_key);
        if (our_secret && planted(plant_secret)) (*our_secret)[31] ^= 1;
        compare(derived && our_secret && std::memcmp(our_secret->data(), their_secret, 32) == 0, "shared secret");
        chiaki_ecdh_fini(&reference);
        ++exchanges;
    }

    // The stream keys, the key stream and the authentication codes.
    int ciphers = 0;
    for (int round = 0; round < 3000; ++round) {
        const Key16 handshake_key = random_bytes<16>(random);
        const proto::SharedSecret secret = random_bytes<32>(random);
        const std::uint8_t direction = (round & 1) != 0 ? proto::kClientDirection : proto::kConsoleDirection;
        ChiakiGKCrypt reference{};
        if (chiaki_gkcrypt_init(&reference, quiet_log(), 0, direction, handshake_key.data(), secret.data()) != CHIAKI_ERR_SUCCESS) {
            compare(false, "reference stream keys");
            continue;
        }
        const proto::StreamCipher ours(direction, handshake_key, secret);
        compare(std::memcmp(ours.key().data(), reference.key_base, 16) == 0, "stream key");
        compare(std::memcmp(ours.iv().data(), reference.iv, 16) == 0, "stream starting vector");
        compare(std::memcmp(ours.mac_key(0).data(), reference.key_gmac_base, 16) == 0, "authentication key");

        // Key positions near the start, around the changes of the authentication key, and far out.
        const auto position = [&]() -> std::uint64_t {
            switch (random.below(4)) {
            case 0: return random.below(200);
            case 1: return std::uint64_t{proto::kMacKeySpan} * (1 + random.below(40)) + random.below(5) - 2;
            case 2: return random.next() % 0x100000000ULL;
            default: return 0xfffffff0ULL + random.below(0x10000);
            }
        };
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t at = position();
            std::vector<std::uint8_t> data = random_vector(random, 1 + random.below(300));
            std::vector<std::uint8_t> theirs = data;
            chiaki_gkcrypt_decrypt(&reference, at, theirs.data(), theirs.size());
            const std::vector<std::uint8_t> plain = data;
            ours.apply_key_stream(at, data);
            if (planted(plant_stream)) data.back() ^= 1;
            // A key stream that changes nothing would agree with another that changes nothing.
            compare(data == theirs && (data.size() < 8 || data != plain), "key stream");

            const std::vector<std::uint8_t> message = random_vector(random, random.below(200));
            std::uint8_t their_code[4];
            chiaki_gkcrypt_gmac(&reference, at, message.data(), message.size(), their_code);
            proto::Mac4 our_code = ours.mac(at, {message});
            if (planted(plant_code)) our_code[3] ^= 1;
            compare(std::memcmp(our_code.data(), their_code, 4) == 0, "authentication code");
            std::uint8_t their_key[16];
            const std::uint64_t index = (at > 0 ? at - 1 : 0) / proto::kMacKeySpan;
            chiaki_gkcrypt_gen_tmp_gmac_key(&reference, index, their_key);
            compare(std::memcmp(ours.mac_key(at).data(), their_key, 16) == 0, "authentication key at a position");
        }
        // Whole packets of the kinds the reference knows a layout for.
        for (const std::uint8_t kind : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{5}}) {
            const std::uint64_t at = position();
            std::vector<std::uint8_t> packet = random_vector(random, 18 + random.below(120));
            packet[0] = static_cast<std::uint8_t>(kind | (random.below(16) << 4));
            std::vector<std::uint8_t> theirs = packet;
            std::uint8_t their_code[4];
            const bool made = chiaki_takion_packet_mac(&reference, theirs.data(), theirs.size(), at, their_code, nullptr) == CHIAKI_ERR_SUCCESS;
            auto our_code = proto::transport::datagram_mac(ours, at, packet);
            if (our_code && planted(plant_packet)) (*our_code)[0] ^= 1;
            compare(made && our_code && std::memcmp(our_code->data(), their_code, 4) == 0, "packet code");
        }
        chiaki_gkcrypt_fini(&reference);
        ++ciphers;
    }

    // Following a key position through the wrap of its 32 bits.
    for (int round = 0; round < 200; ++round) {
        ChiakiKeyState reference{};
        chiaki_key_state_init(&reference);
        proto::KeyPositionTracker ours;
        std::uint64_t true_position = random.below(3) == 0 ? 0xffff0000ULL + random.below(0x10000) : random.below(1000);
        for (int step = 0; step < 100; ++step) {
            // Mostly forwards, sometimes a late packet, sometimes a long jump.
            const std::uint32_t kind = random.below(10);
            if (kind < 7) true_position += random.below(3000);
            else if (kind < 9) true_position -= std::min<std::uint64_t>(true_position, random.below(2000));
            else true_position += 0x10000000ULL + random.below(0x70000000);
            const auto low = static_cast<std::uint32_t>(true_position);
            const std::uint64_t theirs = chiaki_key_state_request_pos(&reference, low, true);
            const std::uint64_t mine = ours.extend(low);
            ours.accept(mine);
            compare(mine == theirs, "key position");
        }
    }

    // The launch specification, and its sealed form.
    for (int round = 0; round < 2000; ++round) {
        const proto::LaunchSpec spec = random_spec(random);
        const std::string ours = proto::format_launch_spec(spec);
        const std::string theirs = reference_launch_spec(spec);
        compare(!theirs.empty() && (planted(plant_spec) ? ours + " " : ours) == theirs, "launch specification");
        const Key16 nonce = random_bytes<16>(random);
        const Key16 secret = random_bytes<16>(random);
        const proto::RpCipher cipher = proto::make_session_cipher(spec.kind, nonce, secret);
        const std::string sealed = proto::seal_launch_spec(cipher, ours);
        compare(!sealed.empty() && (planted(plant_sealed) ? sealed + "=" : sealed) == reference_sealed(spec.kind, nonce, secret, ours),
                "sealed launch specification");
        compare(proto::open_launch_spec(cipher, sealed) == ours, "launch specification opens again");
    }

    // The messages of the set-up, in the reference's generated coder.
    for (int round = 0; round < 2000; ++round) {
        std::uint8_t buffer[4096];
        proto::BigMessage big;
        big.client_version = random.below(20);
        big.session_key = std::string(random.below(80), 'k');
        for (char& c : big.session_key) c = static_cast<char>('A' + random.below(26));
        big.launch_spec = std::string(random.below(1400), 's');
        for (char& c : big.launch_spec) c = static_cast<char>('a' + random.below(26));
        big.encrypted_key = random_vector(random, random.below(8));
        big.ecdh_public_key = random_vector(random, 65);
        big.ecdh_signature = random_vector(random, 32);
        std::size_t size = psvp_ref_encode_big(big.client_version, big.session_key.c_str(), big.launch_spec.c_str(), big.encrypted_key.data(),
                                               big.encrypted_key.size(), big.ecdh_public_key->data(), big.ecdh_public_key->size(),
                                               big.ecdh_signature->data(), big.ecdh_signature->size(), buffer, sizeof(buffer));
        std::vector<std::uint8_t> ours = proto::encode_message(proto::make_message(big));
        if (planted(plant_message)) ours.back() ^= 1;
        compare(size == ours.size() && std::equal(ours.begin(), ours.end(), buffer), "opening message");

        // What we write for the console's side, the reference reads back.
        proto::BangMessage bang;
        bang.server_version = random.below(20);
        bang.token = static_cast<std::uint32_t>(random.next());
        bang.encrypted_key_accepted = random.below(2) != 0;
        bang.version_accepted = random.below(2) != 0;
        bang.session_key = big.session_key;
        bang.ecdh_public_key = random_vector(random, 65);
        bang.ecdh_signature = random_vector(random, 32);
        const std::vector<std::uint8_t> bang_bytes = proto::encode_message(proto::make_message(bang));
        psvp_ref_bang read{};
        const bool decoded = psvp_ref_decode_bang(bang_bytes.data(), bang_bytes.size(), &read);
        compare(decoded && read.server_version == bang.server_version && read.token == bang.token &&
                    read.encrypted_key_accepted == bang.encrypted_key_accepted && read.version_accepted == bang.version_accepted &&
                    read.public_key_size == 65 && std::memcmp(read.public_key, bang.ecdh_public_key->data(), 65) == 0 &&
                    read.signature_size == 32 && std::memcmp(read.signature, bang.ecdh_signature->data(), 32) == 0,
                "the console's answer, read by the reference");

        proto::StreamInfo info;
        const std::size_t resolutions = random.below(5);
        for (std::size_t i = 0; i < resolutions; ++i) {
            info.resolutions.push_back(proto::StreamResolution{random.below(4000), random.below(2200), random_vector(random, 1 + random.below(200))});
        }
        info.audio_header = random_vector(random, proto::AudioHeader::kSize);
        if (random.below(2) != 0) info.start_timeout = random.below(100000);
        if (random.below(2) != 0) info.congestion_control_interval = random.below(1000);
        const std::vector<std::uint8_t> info_bytes = proto::encode_message(proto::make_message(info));
        static psvp_ref_stream_info read_info;
        bool same = psvp_ref_decode_stream_info(info_bytes.data(), info_bytes.size(), &read_info) && read_info.resolutions == resolutions &&
                    read_info.audio_header_size == info.audio_header.size() &&
                    std::memcmp(read_info.audio_header, info.audio_header.data(), info.audio_header.size()) == 0;
        for (std::size_t i = 0; same && i < resolutions; ++i) {
            same = read_info.width[i] == info.resolutions[i].width && read_info.height[i] == info.resolutions[i].height &&
                   read_info.video_header_size[i] == info.resolutions[i].video_header.size() &&
                   std::memcmp(read_info.video_header[i], info.resolutions[i].video_header.data(), info.resolutions[i].video_header.size()) == 0;
        }
        compare(same, "the stream information, read by the reference");

        // The client's short messages.
        const std::vector<std::uint8_t> audio = random_vector(random, proto::AudioHeader::kSize);
        size = psvp_ref_encode_audio_info(audio.data(), audio.size(), buffer, sizeof(buffer));
        proto::StreamInfo microphone;
        microphone.audio_header = audio;
        const std::vector<std::uint8_t> microphone_bytes = proto::encode_message(proto::make_message(microphone));
        compare(size == microphone_bytes.size() && std::equal(microphone_bytes.begin(), microphone_bytes.end(), buffer), "the client's sound format");
    }
    for (const bool dualsense : {false, true}) {
        std::uint8_t buffer[64];
        const std::size_t size = psvp_ref_encode_controller(dualsense, buffer, sizeof(buffer));
        proto::ControllerConnection controller;
        controller.connected = true;
        controller.type = dualsense ? proto::controller_type::kDualSense : proto::controller_type::kDualShock4;
        const std::vector<std::uint8_t> ours = proto::encode_message(proto::make_message(controller));
        compare(size == ours.size() && std::equal(ours.begin(), ours.end(), buffer), "controller connection");
    }
    for (const std::uint32_t type : {proto::message_type::kHeartbeat, proto::message_type::kStreamInfoAck, proto::message_type::kKeyframeRequest}) {
        std::uint8_t buffer[16];
        const std::size_t size = psvp_ref_encode_plain(type, buffer, sizeof(buffer));
        const std::vector<std::uint8_t> ours = proto::encode_message(proto::make_plain_message(type));
        compare(size == ours.size() && std::equal(ours.begin(), ours.end(), buffer), "a message that is its type alone");
    }
    {
        std::uint8_t buffer[64];
        const std::size_t size = psvp_ref_encode_disconnect("Client Disconnecting", buffer, sizeof(buffer));
        const std::vector<std::uint8_t> ours = proto::encode_message(proto::make_message(proto::DisconnectMessage{"Client Disconnecting"}));
        compare(size == ours.size() && std::equal(ours.begin(), ours.end(), buffer), "goodbye");
    }

    std::fprintf(stderr, "  stream: %d comparisons, %d mismatches (%d key exchanges, %d sets of stream keys)\n", outcome.compared,
                 outcome.mismatches, exchanges, ciphers);
    return outcome;
}

void print_stream_vectors() {
    const auto hex = [](const std::uint8_t* data, std::size_t size) { return proto::hex_encode(std::span<const std::uint8_t>(data, size)); };
    // Handshake key 10 11 .. 1f, shared secret 80 81 .. 9f.
    Key16 handshake_key{};
    proto::SharedSecret secret{};
    for (std::size_t i = 0; i < 16; ++i) handshake_key[i] = static_cast<std::uint8_t>(0x10 + i);
    for (std::size_t i = 0; i < 32; ++i) secret[i] = static_cast<std::uint8_t>(0x80 + i);
    const std::string text = "The quick brown fox jumps over the lazy dog";
    for (const std::uint8_t direction : {proto::kClientDirection, proto::kConsoleDirection}) {
        const char* name = direction == proto::kClientDirection ? "client" : "console";
        ChiakiGKCrypt reference{};
        chiaki_gkcrypt_init(&reference, quiet_log(), 0, direction, handshake_key.data(), secret.data());
        std::printf("// keys of the %s's direction, computed by the reference\n", name);
        std::printf("constexpr std::string_view k_%s_key = \"%s\";\n", name, hex(reference.key_base, 16).c_str());
        std::printf("constexpr std::string_view k_%s_iv = \"%s\";\n", name, hex(reference.iv, 16).c_str());
        std::printf("constexpr std::string_view k_%s_mac_key = \"%s\";\n", name, hex(reference.key_gmac_base, 16).c_str());
        std::uint8_t later_key[16];
        chiaki_gkcrypt_gen_tmp_gmac_key(&reference, 3, later_key);
        std::printf("constexpr std::string_view k_%s_mac_key_at_135001 = \"%s\";\n", name, hex(later_key, 16).c_str());
        for (const std::uint64_t at : {std::uint64_t{0}, std::uint64_t{42}, std::uint64_t{135001}}) {
            std::uint8_t code[4];
            chiaki_gkcrypt_gmac(&reference, at, reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), code);
            std::printf("constexpr std::string_view k_%s_code_at_%llu = \"%s\";\n", name, static_cast<unsigned long long>(at), hex(code, 4).c_str());
        }
        std::vector<std::uint8_t> sealed(text.begin(), text.end());
        chiaki_gkcrypt_decrypt(&reference, 37, sealed.data(), sealed.size());
        std::printf("constexpr std::string_view k_%s_sealed_at_37 = \"%s\";\n", name, hex(sealed.data(), sealed.size()).c_str());
        // A control packet and a sound packet with the code the reference gives them.
        std::vector<std::uint8_t> control(40);
        std::vector<std::uint8_t> sound(40);
        for (std::size_t i = 0; i < 40; ++i) {
            control[i] = static_cast<std::uint8_t>(i);
            sound[i] = static_cast<std::uint8_t>(0x40 + i);
        }
        control[0] = 0;
        sound[0] = 3;
        std::uint8_t code[4];
        chiaki_takion_packet_mac(&reference, control.data(), control.size(), 0x2a, code, nullptr);
        std::printf("constexpr std::string_view k_%s_control_packet_code = \"%s\";\n", name, hex(code, 4).c_str());
        chiaki_takion_packet_mac(&reference, sound.data(), sound.size(), 0x2a, code, nullptr);
        std::printf("constexpr std::string_view k_%s_sound_packet_code = \"%s\";\n", name, hex(code, 4).c_str());
        chiaki_gkcrypt_fini(&reference);
    }

    // The shared secret of two fixed private keys: 01 02 .. 20 and a1 a2 .. c0.
    std::array<std::uint8_t, 32> private_a{};
    std::array<std::uint8_t, 32> private_b{};
    for (std::size_t i = 0; i < 32; ++i) {
        private_a[i] = static_cast<std::uint8_t>(1 + i);
        private_b[i] = static_cast<std::uint8_t>(0xa1 + i);
    }
    const auto ours_a = proto::KeyExchange::from_private_key(private_a);
    const auto ours_b = proto::KeyExchange::from_private_key(private_b);
    if (ours_a && ours_b) {
        ChiakiECDH reference{};
        chiaki_ecdh_init(&reference);
        chiaki_ecdh_set_local_key(&reference, private_b.data(), private_b.size(), ours_b->public_key().data(), ours_b->public_key().size());
        std::uint8_t their_public[128];
        std::size_t their_public_size = sizeof(their_public);
        std::uint8_t signature[32];
        std::size_t signature_size = 0;
        chiaki_ecdh_get_local_pub_key(&reference, their_public, &their_public_size, handshake_key.data(), signature, &signature_size);
        std::uint8_t shared[32];
        chiaki_ecdh_derive_secret(&reference, shared, ours_a->public_key().data(), ours_a->public_key().size(), handshake_key.data(), signature, 32);
        std::printf("// the key exchange of two fixed private keys; the reference holds the second\n");
        std::printf("constexpr std::string_view k_public_b = \"%s\";\n", hex(their_public, their_public_size).c_str());
        std::printf("constexpr std::string_view k_signature_b = \"%s\";\n", hex(signature, 32).c_str());
        std::printf("constexpr std::string_view k_shared_secret = \"%s\";\n", hex(shared, 32).c_str());
        chiaki_ecdh_fini(&reference);
    }

    // The launch specification for 1280x720 at 60, 10000 kbit/s, largest packet 1454, round trip 3 ms.
    for (const ConsoleKind kind : {ConsoleKind::ps4, ConsoleKind::ps5}) {
        proto::LaunchSpec spec;
        spec.kind = kind;
        spec.round_trip_ms = 3;
        spec.codec = kind == ConsoleKind::ps5 ? proto::Codec::hevc : proto::Codec::h264;
        spec.handshake_key = handshake_key;
        const char* name = kind == ConsoleKind::ps5 ? "ps5" : "ps4";
        std::printf("// the launch specification, formatted by the reference (%s)\n", name);
        std::printf("constexpr std::string_view k_%s_launch_spec = R\"spec(%s)spec\";\n", name, reference_launch_spec(spec).c_str());
    }
}

}  // namespace psvp::oracle
