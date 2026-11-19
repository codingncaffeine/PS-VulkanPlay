#include "proto/stream_setup.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "proto/hex.h"
#include "proto/launch_spec.h"

namespace psvp::proto::transport {
namespace {

constexpr std::string_view kGoodbye = "Client Disconnecting";
// The sound format the client announces for its microphone.
constexpr AudioHeader kMicrophone{1, 16, 48000, 480, 1};

std::size_t split_size(const ProbeResult& network) {
    return std::min(network.mtu_in, network.mtu_out) - kSplitMargin;
}

std::uint8_t kind_of(std::span<const std::uint8_t> datagram) { return static_cast<std::uint8_t>(datagram[0] & 0x0f); }

// The packet a one-chunk message came in, as the sender wrote it.
std::vector<std::uint8_t> packet_of(std::uint32_t tag, const DataChunk& chunk) {
    ControlPacket packet;
    packet.tag = tag;
    packet.mac = chunk.mac;
    packet.key_position = chunk.key_position;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = chunk.flags;
    packet.payload = encode_data_chunk(chunk);
    return encode_control(packet);
}

}  // namespace

// ---- The client's side ----------------------------------------------------

ClientStream::ClientStream(std::uint32_t local_tag, const Endpoint& console, StreamRequest request, const KeyExchange& keys,
                           const Key16& handshake_key)
    : messenger_(local_tag, console.tag, local_tag, console.first_sequence, Messenger::Keys::expected),
      request_(std::move(request)),
      keys_(keys),
      handshake_key_(handshake_key),
      max_datagram_(split_size(request_.network)) {}

void ClientStream::fail(std::string why) {
    state_ = State::failed;
    reason_ = std::move(why);
}

void ClientStream::send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now) {
    const auto bytes = encode_message(message);
    auto sent = messenger_.send_message(channel, bytes, now, max_datagram_);
    if (!sent) {
        fail("too many of the stream's messages are unanswered");
        return;
    }
    for (auto& chunk : *sent) out.push_back(std::move(chunk.datagram));
}

Datagrams ClientStream::start(TimePoint now) {
    Datagrams out;
    if (state_ != State::idle) return out;

    LaunchSpec spec;
    spec.kind = request_.kind;
    spec.width = request_.video.width;
    spec.height = request_.video.height;
    spec.max_fps = request_.video.max_fps;
    spec.bitrate_kbps = request_.video.bitrate_kbps;
    spec.mtu = request_.network.mtu_in;
    spec.round_trip_ms = static_cast<std::uint32_t>(request_.network.round_trip.count() / 1000);
    spec.codec = request_.video.codec;
    spec.handshake_key = handshake_key_;

    BigMessage big;
    big.client_version = stream_version(request_.kind);
    big.session_key = request_.session_id;
    big.launch_spec = seal_launch_spec(request_.session_cipher, format_launch_spec(spec));
    big.encrypted_key.assign(4, 0);
    big.ecdh_public_key.emplace(keys_.public_key().begin(), keys_.public_key().end());
    const KeySignature signature = sign_public_key(handshake_key_, keys_.public_key());
    big.ecdh_signature.emplace(signature.begin(), signature.end());

    state_ = State::awaiting_bang;
    step_deadline_ = now + kAnswerWait;
    send(out, make_message(std::move(big)), kSessionChannel, now);
    opening_datagrams_ = out.size();
    for (const auto& datagram : out) opening_size_ += datagram.size();
    return out;
}

void ClientStream::on_bang(Datagrams& out, const DataChunk& chunk, const BangMessage& bang, TimePoint now) {
    BangFacts facts;
    facts.server_version = bang.server_version;
    facts.session_key_size = bang.session_key.size();
    facts.session_key_is_ours = bang.session_key == request_.session_id;
    facts.chunks = chunk.chunks;
    bang_ = facts;
    if (!bang.version_accepted) return fail(std::format("the console does not speak version {} of the stream protocol", stream_version(request_.kind)));
    if (!bang.encrypted_key_accepted) return fail("the console did not accept the opening message's key");
    if (!bang.ecdh_public_key || !bang.ecdh_signature) return fail("the console's answer carries no key");

    const auto secret = keys_.shared_secret(*bang.ecdh_public_key);
    if (!secret) return fail("the console's key is not usable");
    const KeySignature expected = sign_public_key(handshake_key_, *bang.ecdh_public_key);
    bang_->signature_matches = std::ranges::equal(expected, *bang.ecdh_signature);

    auto send_keys = std::make_shared<SendKeys>(StreamCipher(kClientDirection, handshake_key_, *secret));
    receive_keys_ = std::make_shared<ReceiveKeys>(StreamCipher(kConsoleDirection, handshake_key_, *secret));
    // Whether the answer itself was sealed with the keys it brought.
    if (chunk.chunks == 1) {
        if (chunk.mac == Mac4{}) {
            bang_->code = BangFacts::Code::none;
        } else {
            const auto packet = packet_of(messenger_.local_tag(), chunk);
            const auto code = datagram_mac(receive_keys_->cipher, receive_keys_->positions.extend(chunk.key_position), packet);
            bang_->code = code && *code == chunk.mac ? BangFacts::Code::valid : BangFacts::Code::invalid;
        }
    }
    messenger_.use_keys(std::move(send_keys), receive_keys_);

    state_ = State::awaiting_stream_info;
    step_deadline_ = now + kAnswerWait;
    if (early_info_) {
        const StreamInfo info = std::move(*early_info_);
        early_info_.reset();
        on_stream_info(out, info, now);
    }
}

void ClientStream::on_stream_info(Datagrams& out, const StreamInfo& info, TimePoint now) {
    if (!parse_audio_header(info.audio_header)) return fail("the stream information names no usable sound format");
    info_ = info;
    send(out, make_plain_message(message_type::kStreamInfoAck), kStreamInfoAckChannel, now);
    ControllerConnection controller;
    controller.connected = true;
    controller.type = request_.dualsense ? controller_type::kDualSense : controller_type::kDualShock4;
    send(out, make_message(controller), kSessionChannel, now);
    StreamInfo microphone;
    microphone.audio_header = encode_audio_header(kMicrophone);
    send(out, make_message(std::move(microphone)), kSessionChannel, now);
    if (state_ == State::failed) return;
    state_ = State::ready;
    next_heartbeat_ = now + kHeartbeatEvery;
}

void ClientStream::on_message(Datagrams& out, const DataChunk& chunk, const StreamMessage& message, TimePoint now) {
    if (const auto* goodbye = message.as<DisconnectMessage>()) {
        state_ = State::closed;
        closed_by_console_ = true;
        reason_ = goodbye->reason;
        return;
    }
    switch (state_) {
    case State::awaiting_bang:
        if (const auto* bang = message.as<BangMessage>()) on_bang(out, chunk, *bang, now);
        // The stream information may overtake the handling of the answer; it is kept for then.
        else if (const auto* info = message.as<StreamInfo>(); info != nullptr && !early_info_) early_info_ = *info;
        break;
    case State::awaiting_stream_info:
        if (const auto* info = message.as<StreamInfo>()) on_stream_info(out, *info, now);
        break;
    case State::ready: ++counters_.messages[message.type]; break;
    default: break;
    }
}

void ClientStream::handle(Datagrams& out, Messenger::Received& received, TimePoint now) {
    for (const DataChunk& chunk : received.delivered) {
        if (chunk.chunks > 1) ++counters_.split_messages;
        if (chunk.data_type != data_type::kMessage) {
            ++counters_.other_data[chunk.data_type];
            continue;
        }
        if (state_ == State::failed || state_ == State::closed) continue;
        if (const auto message = parse_message(chunk.data)) on_message(out, chunk, *message, now);
    }
}

Datagrams ClientStream::on_datagram(std::span<const std::uint8_t> datagram, TimePoint now) {
    Datagrams out;
    if (datagram.empty() || state_ == State::idle) return out;
    const std::uint8_t kind = kind_of(datagram);
    if (kind == packet_kind::kControl) {
        const auto packet = parse_control(datagram);
        if (!packet) {
            ++counters_.other_datagrams;
            return out;
        }
        Messenger::Received received = messenger_.on_packet(*packet);
        handle(out, received, now);
        // One message at a time until the keys are there (see Messenger::Keys).
        while (messenger_.ready() && state_ != State::failed) {
            received = messenger_.pump();
            if (received.delivered.empty()) break;
            handle(out, received, now);
        }
        // The acknowledgment goes last: sealed, if the message it answers brought the keys.
        if (auto ack = messenger_.take_ack()) out.push_back(std::move(*ack));
    } else if (kind == packet_kind::kVideo || kind == packet_kind::kAudio) {
        if (!receive_keys_) ++counters_.media_before_keys;
        else if (!verify_datagram(*receive_keys_, datagram)) ++counters_.media_rejected;
        else if (kind == packet_kind::kVideo) ++counters_.video_packets;
        else ++counters_.audio_packets;
    } else {
        ++counters_.other_datagrams;
    }
    return out;
}

Datagrams ClientStream::on_timeout(TimePoint now) {
    Datagrams out;
    Messenger::Timer timer = messenger_.on_timer(now);
    out = std::move(timer.resend);
    if (state_ == State::awaiting_bang || state_ == State::awaiting_stream_info) {
        if (!timer.abandoned.empty()) {
            fail("the console stopped answering during the stream's set-up");
        } else if (now >= step_deadline_) {
            fail(state_ == State::awaiting_bang ? "the console did not answer the stream's opening message"
                                                : "the console sent no stream information");
        }
    } else if (state_ == State::ready) {
        if (!timer.abandoned.empty()) {
            fail("the console stopped acknowledging the stream's messages");
        } else if (now >= next_heartbeat_) {
            send(out, make_plain_message(message_type::kHeartbeat), kSessionChannel, now);
            if (state_ == State::ready) ++counters_.heartbeats_sent;
            next_heartbeat_ = now + kHeartbeatEvery;
        }
    }
    return out;
}

TimePoint ClientStream::deadline() const {
    TimePoint when = TimePoint::max();
    if (const auto resend = messenger_.deadline()) when = *resend;
    if (state_ == State::awaiting_bang || state_ == State::awaiting_stream_info) when = std::min(when, step_deadline_);
    else if (state_ == State::ready) when = std::min(when, next_heartbeat_);
    return when;
}

Datagrams ClientStream::disconnect(TimePoint now) {
    Datagrams out;
    if (state_ != State::awaiting_bang && state_ != State::awaiting_stream_info && state_ != State::ready) return out;
    send(out, make_message(DisconnectMessage{std::string(kGoodbye)}), kSessionChannel, now);
    if (state_ == State::failed) return out;
    goodbye_ = true;
    state_ = State::closed;
    return out;
}

// ---- The console's side ---------------------------------------------------

ConsoleStream::ConsoleStream(std::uint32_t local_tag, const Endpoint& client, Options options, const KeyExchange& keys)
    : messenger_(local_tag, client.tag, local_tag, client.first_sequence, Messenger::Keys::expected),
      options_(std::move(options)),
      keys_(keys) {}

void ConsoleStream::send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now) {
    const auto bytes = encode_message(message);
    if (auto sent = messenger_.send_message(channel, bytes, now, options_.max_datagram)) {
        for (auto& chunk : *sent) out.push_back(std::move(chunk.datagram));
    } else {
        events_.emplace_back("a message could not be sent: too many are unanswered");
    }
}

void ConsoleStream::on_big(Datagrams& out, const DataChunk& chunk, const BigMessage& big, TimePoint now) {
    const std::string id = big.session_key == options_.session_id ? std::string("the one handed out")
                                                                   : std::format("another, of {} characters", big.session_key.size());
    const auto spec = open_launch_spec(options_.session_cipher, big.launch_spec);
    const auto handshake_key = spec ? launch_spec_handshake_key(*spec) : std::nullopt;
    const bool have_key = big.ecdh_public_key && big.ecdh_signature;
    const bool signature_ok =
        have_key && handshake_key && std::ranges::equal(sign_public_key(*handshake_key, *big.ecdh_public_key), *big.ecdh_signature);
    events_.push_back(std::format("big: version {} chunks {} key-position {} code {} session-id {} encrypted-key {} public-key {} bytes signature {}",
                                  big.client_version, chunk.chunks, chunk.key_position, hex_encode(chunk.mac), id,
                                  hex_encode(big.encrypted_key), big.ecdh_public_key ? big.ecdh_public_key->size() : 0,
                                  signature_ok ? "ok" : "BAD"));
    events_.push_back(std::format("launch spec: {}", spec ? *spec : std::string("does not open")));
    if (!spec || !handshake_key || !signature_ok) return;

    BangMessage bang;
    bang.server_version = stream_version(options_.kind);
    bang.session_key = options_.session_id;
    if (!options_.accept_version || big.client_version != stream_version(options_.kind)) {
        bang.encrypted_key_accepted = true;
        bang.version_accepted = false;
        send(out, make_message(std::move(bang)), ClientStream::kSessionChannel, now);
        events_.emplace_back("bang sent: version not accepted");
        return;
    }
    const auto secret = keys_.shared_secret(*big.ecdh_public_key);
    if (!secret) {
        events_.emplace_back("big: the public key is not a point of the curve");
        return;
    }
    Key16 signing_key = *handshake_key;
    if (options_.wrong_signature) signing_key[0] ^= 0xff;
    const KeySignature signature = sign_public_key(signing_key, keys_.public_key());
    bang.encrypted_key_accepted = true;
    bang.version_accepted = true;
    bang.ecdh_public_key.emplace(keys_.public_key().begin(), keys_.public_key().end());
    bang.ecdh_signature.emplace(signature.begin(), signature.end());
    // The answer goes out before the keys are in use; everything after it is sealed.
    send(out, make_message(std::move(bang)), ClientStream::kSessionChannel, now);
    events_.emplace_back("bang sent");

    send_keys_ = std::make_shared<SendKeys>(StreamCipher(kConsoleDirection, *handshake_key, *secret));
    receive_keys_ = std::make_shared<ReceiveKeys>(StreamCipher(kClientDirection, *handshake_key, *secret));
    messenger_.use_keys(send_keys_, receive_keys_);

    StreamInfo info;
    info.resolutions = options_.resolutions;
    info.audio_header = encode_audio_header(options_.audio);
    send(out, make_message(std::move(info)), ClientStream::kSessionChannel, now);
    events_.emplace_back("stream information sent");
    if (options_.media_every.count() > 0) next_media_ = now + options_.media_every;
}

void ConsoleStream::on_message(Datagrams& out, const DataChunk& chunk, TimePoint now) {
    const auto message = chunk.data_type == data_type::kMessage ? parse_message(chunk.data) : std::nullopt;
    if (!message) {
        events_.push_back(std::format("data of kind {} not understood ({} bytes)", chunk.data_type, chunk.data.size()));
        return;
    }
    if (const auto* big = message->as<BigMessage>()) {
        if (!messenger_.keyed()) on_big(out, chunk, *big, now);
        return;
    }
    if (message->type == message_type::kHeartbeat) {
        ++heartbeats_;
        return;
    }
    events_.push_back(std::format("message type {} channel {} flags {} key-position {} {}: {}", message->type, chunk.channel, chunk.flags,
                                  chunk.key_position, chunk.authentic ? "authentic" : "NOT SEALED", hex_encode(chunk.data)));
    if (const auto* goodbye = message->as<DisconnectMessage>()) {
        disconnected_ = true;
        next_media_.reset();
        events_.push_back(std::format("goodbye: {}", goodbye->reason));
    }
}

Datagrams ConsoleStream::on_datagram(std::span<const std::uint8_t> datagram, TimePoint now) {
    Datagrams out;
    if (datagram.empty()) return out;
    const std::uint8_t kind = kind_of(datagram);
    if (kind == packet_kind::kControl) {
        const auto packet = parse_control(datagram);
        if (!packet) return out;
        const std::uint64_t rejected_before = messenger_.rejected();
        const bool keyed_before = messenger_.keyed();
        Messenger::Received received = messenger_.on_packet(*packet);
        if (messenger_.rejected() != rejected_before) {
            events_.push_back(std::format("control packet REJECTED: chunk type {} key-position {} code {}", packet->chunk_type,
                                          packet->key_position, hex_encode(packet->mac)));
        } else if (packet->chunk_type == chunk::kDataAck) {
            if (const auto ack = parse_data_ack(packet->payload)) {
                events_.push_back(std::format("ack key-position {} {}", packet->key_position, keyed_before ? "authentic" : "not sealed"));
            }
        }
        if (keyed_before && messenger_.rejected() == rejected_before && receive_keys_) {
            // An acknowledgment counts for its whole packet, a message for its own bytes.
            if (packet->chunk_type == chunk::kDataAck) {
                positions_[receive_keys_->positions.last()] = datagram.size();
            } else if (packet->chunk_type == chunk::kData && packet->payload.size() >= 9 && (packet->chunk_flags & chunk_flag::kLast) != 0) {
                positions_[receive_keys_->positions.last()] = packet->payload.size() - 9;
            } else {
                positions_checkable_ = false;  // a split message: which chunk is its first is not known here
            }
        }
        for (const DataChunk& chunk : received.delivered) on_message(out, chunk, now);
        while (messenger_.ready()) {
            received = messenger_.pump();
            if (received.delivered.empty()) break;
            for (const DataChunk& chunk : received.delivered) on_message(out, chunk, now);
        }
        if (auto ack = messenger_.take_ack()) out.push_back(std::move(*ack));
    } else if (mac_layout(datagram[0])) {
        // Input, congestion reports and the like: read by nobody here, but their codes are checked.
        if (!receive_keys_) {
            ++before_keys_;
        } else if (verify_datagram(*receive_keys_, datagram)) {
            ++authentic_[kind];
            // A congestion report counts for its fifteen bytes; an input packet for what follows
            // its twelve header bytes and sixteen more.
            if (kind == packet_kind::kCongestion) positions_[receive_keys_->positions.last()] = 15;
            else if (kind == packet_kind::kInputState || kind == packet_kind::kInputHistory) positions_[receive_keys_->positions.last()] = datagram.size() - 12 + 16;
            else positions_checkable_ = false;
        } else {
            ++rejected_[kind];
        }
    }
    return out;
}

Datagrams ConsoleStream::on_timer(TimePoint now) {
    Messenger::Timer timer = messenger_.on_timer(now);
    Datagrams out = std::move(timer.resend);
    if (next_media_ && send_keys_ && now >= *next_media_) {
        // A sound packet as far as its header goes; what follows it is not a sound unit.
        MediaHeader header;
        header.video = false;
        header.packet_index = media_index_;
        header.frame_index = media_index_++;
        std::vector<std::uint8_t> datagram = encode_media(header, 120);
        (void)seal_datagram(send_keys_->cipher, send_keys_->take(datagram.size()), datagram);
        if (options_.corrupt_media) datagram.back() ^= 0x01;
        out.push_back(std::move(datagram));
        *next_media_ += options_.media_every;
    }
    return out;
}

std::optional<TimePoint> ConsoleStream::deadline() const {
    std::optional<TimePoint> when = messenger_.deadline();
    if (next_media_ && (!when || *next_media_ < *when)) when = next_media_;
    return when;
}

Datagrams ConsoleStream::disconnect(const std::string& reason, TimePoint now) {
    Datagrams out;
    send(out, make_message(DisconnectMessage{reason}), ClientStream::kSessionChannel, now);
    next_media_.reset();
    return out;
}

std::vector<std::string> ConsoleStream::take_events() { return std::exchange(events_, {}); }

std::string ConsoleStream::summary() const {
    std::string text = std::format("heartbeats {} control-rejected {} before-keys {}", heartbeats_, messenger_.rejected(), before_keys_);
    for (const auto& [kind, count] : authentic_) text += std::format(" kind-{}-authentic {}", kind, count);
    for (const auto& [kind, count] : rejected_) text += std::format(" kind-{}-REJECTED {}", kind, count);
    if (positions_checkable_) {
        // Each packet's key position is the one before it plus that packet's size and the size's remainder by sixteen.
        std::uint64_t expected = 0;
        std::uint64_t out_of_step = 0;
        for (const auto& [position, size] : positions_) {
            if (position != expected) ++out_of_step;
            expected = position + size + size % 16;
        }
        text += std::format(" key-positions {} out-of-step {}", positions_.size(), out_of_step);
    } else {
        text += " key-positions not checked";
    }
    return text;
}

}  // namespace psvp::proto::transport
