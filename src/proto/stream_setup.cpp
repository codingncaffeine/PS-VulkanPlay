#include "proto/stream_setup.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#include "proto/controller_feedback.h"
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
    for (auto& chunk : *sent) {
        first_sent_[chunk.sequence] = now;
        out.push_back(std::move(chunk.datagram));
    }
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
    // A PS5 signs its key with the handshake key (seen); what a PS4 signs with has not been seen,
    // so there a mismatch is only reported.
    if (!bang_->signature_matches && request_.kind == ConsoleKind::ps5) {
        return fail("the console's key is not signed with this session's handshake key");
    }

    send_keys_ = std::make_shared<SendKeys>(StreamCipher(kClientDirection, handshake_key_, *secret));
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
    messenger_.use_keys(send_keys_, receive_keys_);

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
    next_congestion_ = now + kCongestionEvery;
    next_input_ = now + kInputEvery;
    last_input_ = now;
}

// A picture or sound packet that carried the right code.
void ClientStream::on_media(std::span<const std::uint8_t> datagram, TimePoint now) {
    const auto packet = parse_stream_media(request_.kind, datagram);
    if (!packet) {
        ++counters_.media_unreadable;
        return;
    }
    // The key stream of the payload begins sixteen bytes behind the packet's key position.
    scratch_.assign(packet->payload.begin(), packet->payload.end());
    receive_keys_->cipher.apply_key_stream(receive_keys_->positions.last() + kPayloadKeyOffset, scratch_);
    if (packet->video) {
        FrameOutput frames;
        video_.on_packet(*packet, scratch_, now, frames);
        note_lost(frames);
        for (auto& frame : frames.frames) media_.video.push_back(std::move(frame));
        return;
    }
    ++counters_.sound_streams[(std::uint32_t{packet->sound_stream} << 8) | packet->codec];
    if (packet->sound_stream != 0 && !packet->haptics) {
        // A stream this client does not know: its frames are not the sound's, and are kept out of it.
        if (++counters_.other_sound_packets <= kMaxOtherSound) {
            other_sound_.push_back(OtherSound{packet->sound_stream, packet->codec, packet->unit_size, packet->source_frames, packet->repeated_frames,
                                             packet->frame_index, scratch_});
        }
        return;
    }
    SoundReceiver& receiver = packet->haptics ? haptics_ : sound_;
    const std::uint64_t lost_before = receiver.counters().frames_lost;
    const std::uint64_t unusable_before = receiver.counters().packets_unusable;
    receiver.on_packet(*packet, scratch_, media_.sound);
    if (receiver.counters().packets_unusable == unusable_before) ++interval_received_;
    interval_lost_ += receiver.counters().frames_lost - lost_before;
}

// Frames that will not come are reported to the console. What cannot be sent yet is kept;
// when too much is kept, it becomes one range that covers all of it.
void ClientStream::note_lost(const FrameOutput& frames) {
    for (const FrameRange& range : frames.lost) {
        if (!unreported_.empty() && static_cast<std::uint16_t>(unreported_.back().last + 1) == range.first) unreported_.back().last = range.last;
        else unreported_.push_back(range);
    }
    if (unreported_.size() > kMaxUnreported) unreported_ = {FrameRange{unreported_.front().first, unreported_.back().last}};
}

void ClientStream::send_reports(Datagrams& out, TimePoint now) {
    if (state_ != State::ready) return;
    while (!unreported_.empty() && messenger_.waiting() < Messenger::kWindow) {
        const FrameRange range = unreported_.front();
        unreported_.erase(unreported_.begin());
        send(out, make_message(CorruptFrames{range.first, range.last}), kReportChannel, now);
        if (state_ != State::ready) return;
        ++counters_.loss_reports;
    }
}

Datagrams ClientStream::request_keyframe(TimePoint now) {
    Datagrams out;
    if (state_ != State::ready || messenger_.waiting() >= Messenger::kWindow) return out;
    send(out, make_plain_message(message_type::kKeyframeRequest), kReportChannel, now);
    if (state_ == State::ready) ++counters_.keyframe_requests;
    return out;
}

// The controller's state: encrypted from sixteen bytes behind the packet's key position on, then sealed.
void ClientStream::send_input(Datagrams& out, TimePoint now) {
    last_input_ = now;
    next_input_ = now + kInputEvery;
    input_changed_ = false;
    std::vector<std::uint8_t> state = encode_input_state(request_.kind, input_);
    const std::uint64_t position = send_keys_->take(state.size() + kPayloadKeyOffset);
    send_keys_->cipher.apply_key_stream(position + kPayloadKeyOffset, state);
    std::vector<std::uint8_t> packet = encode_input_packet(input_sequence_++, state);
    if (!seal_datagram(send_keys_->cipher, position, packet)) return;
    out.push_back(std::move(packet));
    ++counters_.input_states;
}

// Button, trigger and touchpad changes: encrypted and sealed like the state.
void ClientStream::send_history(Datagrams& out, const InputHistory::Packet& packet) {
    std::vector<std::uint8_t> payload = packet.payload;
    const std::uint64_t position = send_keys_->take(payload.size() + kPayloadKeyOffset);
    send_keys_->cipher.apply_key_stream(position + kPayloadKeyOffset, payload);
    std::vector<std::uint8_t> datagram = encode_history_packet(packet.sequence, packet.events, payload);
    if (!seal_datagram(send_keys_->cipher, position, datagram)) return;
    out.push_back(std::move(datagram));
    ++counters_.history_packets;
}

Datagrams ClientStream::set_controller(const Buttons& buttons, const InputState& state, TimePoint now) {
    Datagrams out;
    if (!(state == input_)) {
        input_ = state;
        input_changed_ = true;
    }
    if (state_ != State::ready) return out;
    const std::vector<InputHistory::Packet> packets = history_.on_change(buttons_, buttons, now);
    buttons_ = buttons;
    counters_.input_events += packets.size();
    for (const InputHistory::Packet& packet : packets) send_history(out, packet);
    if (input_changed_ && now - last_input_ >= kInputAtMostEvery) send_input(out, now);
    return out;
}

// The acknowledgment that is due, and, once the stream runs, the same again a little later: a lost
// one costs a message sent again after 200 ms, which the console counts as the round trip.
void ClientStream::send_ack(Datagrams& out, TimePoint now) {
    const std::uint64_t repeats = messenger_.repeats();
    if (state_ == State::ready) counters_.console_repeats += repeats - repeats_seen_;
    repeats_seen_ = repeats;
    auto ack = messenger_.take_ack();
    if (!ack) return;
    out.push_back(std::move(*ack));
    if (state_ != State::ready) return;
    ++counters_.acks;
    // The newest acknowledgment covers every older one: its copies replace theirs.
    ack_copies_due_.clear();
    for (std::size_t i = std::min<std::size_t>(request_.ack_copies, kAckCopiesAfter.size()); i > 0; --i) {
        ack_copies_due_.push_back(now + kAckCopiesAfter[i - 1]);
    }
}

// How many picture and sound packets arrived and how many did not, since the report before; the
// loss no higher than the ceiling.
void ClientStream::send_congestion(Datagrams& out) {
    const UnitStats units = video_.take_unit_stats();
    const PacketCounts measured{units.received + std::exchange(interval_received_, 0), units.missing + std::exchange(interval_lost_, 0)};
    const PacketCounts told = with_loss_ceiling(measured, request_.reported_loss_ceiling);
    std::vector<std::uint8_t> report = encode_congestion(static_cast<std::uint16_t>(std::min<std::uint64_t>(told.received, 0xffff)),
                                                         static_cast<std::uint16_t>(std::min<std::uint64_t>(told.lost, 0xffff)));
    if (!seal_datagram(send_keys_->cipher, send_keys_->take(kCongestionSize), report)) return;
    out.push_back(std::move(report));
    ++counters_.congestion_reports;
    counters_.congestion_received += measured.received;
    counters_.congestion_lost += measured.lost;
    counters_.congestion_lost_told += told.lost;
    if (told != measured) ++counters_.congestion_lowered;
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
    case State::ready:
        ++counters_.messages[message.type];
        if (const auto* quality = message.as<ConnectionQuality>()) quality_ = *quality;
        // Kept for whoever wants to look at them; the console's heartbeats are only counted.
        if (message.type != message_type::kHeartbeat && console_data_.size() < kMaxConsoleData) {
            console_data_.push_back(ConsoleData{true, message.type, chunk.channel, chunk.data});
        }
        break;
    default: break;
    }
}

void ClientStream::handle(Datagrams& out, Messenger::Received& received, TimePoint now) {
    for (const std::uint32_t sequence : received.acknowledged) {
        const auto sent = first_sent_.find(sequence);
        if (sent == first_sent_.end()) continue;
        if (state_ == State::ready) {
            const auto wait = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - sent->second).count());
            ++counters_.our_acknowledged;
            counters_.our_ack_wait_us_total += wait;
            counters_.our_ack_wait_us_most = std::max(counters_.our_ack_wait_us_most, wait);
            if (wait > 150'000) {
                ++counters_.our_ack_waits_over_150ms;
                if (slow_acks_.size() < kMaxConsoleData) slow_acks_.push_back(wait);
            }
        }
        first_sent_.erase(sent);
    }
    for (const DataChunk& chunk : received.delivered) {
        if (chunk.chunks > 1) ++counters_.split_messages;
        if (chunk.data_type != data_type::kMessage) {
            ++counters_.other_data[chunk.data_type];
            if (console_data_.size() < kMaxConsoleData) {
                console_data_.push_back(ConsoleData{false, chunk.data_type, chunk.channel, chunk.data});
            }
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
        send_ack(out, now);
    } else if (kind == packet_kind::kVideo || kind == packet_kind::kAudio) {
        if (!receive_keys_) {
            ++counters_.media_before_keys;
        } else if (!verify_datagram(*receive_keys_, datagram)) {
            ++counters_.media_rejected;
        } else {
            ++(kind == packet_kind::kVideo ? counters_.video_packets : counters_.audio_packets);
            // The picture may begin before the stream information is in: its packets need only the keys.
            if (state_ == State::ready || state_ == State::awaiting_stream_info) {
                on_media(datagram, now);
                send_reports(out, now);
            }
        }
    } else {
        ++counters_.other_datagrams;
    }
    return out;
}

Datagrams ClientStream::on_timeout(TimePoint now) {
    Datagrams out;
    Messenger::Timer timer = messenger_.on_timer(now);
    if (state_ == State::ready) counters_.our_resends += timer.resend.size();
    for (const std::uint32_t sequence : timer.abandoned) first_sent_.erase(sequence);
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
            lost_ = true;
            fail("the console stopped acknowledging the stream's messages");
        } else if (now >= next_heartbeat_) {
            // With every place for an unanswered message taken, this one is left out: the
            // messages that wait are sent again, and given up on when the console stays silent.
            if (messenger_.waiting() < Messenger::kWindow) {
                send(out, make_plain_message(message_type::kHeartbeat), kSessionChannel, now);
                if (state_ == State::ready) ++counters_.heartbeats_sent;
            }
            next_heartbeat_ = now + kHeartbeatEvery;
        }
    }
    if (state_ == State::ready) {
        FrameOutput frames;
        video_.on_timeout(now, frames);
        note_lost(frames);
        for (auto& frame : frames.frames) media_.video.push_back(std::move(frame));
        send_reports(out, now);
        if (now >= next_congestion_) {
            send_congestion(out);
            next_congestion_ = now + kCongestionEvery;
        }
        if (now >= next_input_ || (input_changed_ && now - last_input_ >= kInputAtMostEvery)) send_input(out, now);
        while (!ack_copies_due_.empty() && now >= ack_copies_due_.back()) {
            ack_copies_due_.pop_back();
            if (auto again = messenger_.ack_again()) {
                out.push_back(std::move(*again));
                ++counters_.ack_copies;
            }
        }
        // The last history packet again, while nothing new follows it.
        if (const auto again = history_.on_timeout(now)) send_history(out, *again);
    }
    return out;
}

TimePoint ClientStream::deadline() const {
    TimePoint when = TimePoint::max();
    if (const auto resend = messenger_.deadline()) when = *resend;
    if (state_ == State::awaiting_bang || state_ == State::awaiting_stream_info) when = std::min(when, step_deadline_);
    else if (state_ == State::ready) {
        when = std::min({when, next_heartbeat_, next_congestion_, next_input_});
        if (input_changed_) when = std::min(when, last_input_ + kInputAtMostEvery);
        if (const auto again = history_.deadline()) when = std::min(when, *again);
        if (const auto frame = video_.deadline()) when = std::min(when, *frame);
        if (!ack_copies_due_.empty()) when = std::min(when, ack_copies_due_.back());
    }
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
    media_.emplace(options_.kind, send_keys_);
    if (options_.media_every.count() > 0) next_media_ = now + options_.media_every;
    if (options_.frame_every.count() > 0) next_frame_ = now + options_.frame_every;
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
    if (const auto* frames = message->as<CorruptFrames>()) {
        events_.push_back(std::format("frames {} to {} reported as not arrived (channel {}, {})", frames->start, frames->end, chunk.channel,
                                      chunk.authentic ? "authentic" : "NOT SEALED"));
        if (options_.picture_lost) options_.picture_lost(static_cast<std::uint16_t>(frames->start), static_cast<std::uint16_t>(frames->end));
        return;
    }
    if (message->type == message_type::kKeyframeRequest) {
        keyframe_wanted_ = true;
        events_.push_back(std::format("keyframe requested (channel {}, {})", chunk.channel, chunk.authentic ? "authentic" : "NOT SEALED"));
        return;
    }
    events_.push_back(std::format("message type {} channel {} flags {} key-position {} {}: {}", message->type, chunk.channel, chunk.flags,
                                  chunk.key_position, chunk.authentic ? "authentic" : "NOT SEALED", hex_encode(chunk.data)));
    if (message->type == message_type::kStreamInfo && messenger_.keyed() && !std::exchange(client_info_answered_, true)) {
        // What a PS5 was seen to send once a client's stream was set up, on channel 0: one chunk of
        // information for the controller (kind 9, seventeen bytes), then "stream information received".
        constexpr std::array<std::uint8_t, 17> kPadInfo{0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0};
        if (auto sent = messenger_.send(chunk_flag::kLast, kOwnChannel, kPadInfo, now, data_type::kPadInfo)) {
            out.push_back(std::move(sent->datagram));
        }
        send(out, make_plain_message(message_type::kStreamInfoAck), kOwnChannel, now);
        if (options_.heartbeat_every.count() > 0) next_heartbeat_ = now + options_.heartbeat_every;
        if (options_.quality_every.count() > 0) next_quality_ = now + options_.quality_every;
        if (options_.feedback_at.count() > 0) next_feedback_ = now + options_.feedback_at;
    }
    if (const auto* goodbye = message->as<DisconnectMessage>()) {
        disconnected_ = true;
        next_media_.reset();
        next_frame_.reset();
    next_heartbeat_.reset();
    next_quality_.reset();
    next_feedback_.reset();
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
            if (kind == packet_kind::kCongestion) {
                positions_[receive_keys_->positions.last()] = 15;
                if (const auto report = parse_congestion(datagram)) {
                    congestion_received_ += report->received;
                    congestion_lost_ += report->lost;
                }
            }
            else if (kind == packet_kind::kInputState || kind == packet_kind::kInputHistory) {
                positions_[receive_keys_->positions.last()] = datagram.size() - 12 + 16;
                if (kind == packet_kind::kInputHistory && datagram.size() > kInputHeaderSize) {
                    std::vector<std::uint8_t> payload(datagram.begin() + kInputHeaderSize, datagram.end());
                    receive_keys_->cipher.apply_key_stream(receive_keys_->positions.last() + kPayloadKeyOffset, payload);
                    const auto sequence = static_cast<std::uint16_t>((datagram[1] << 8) | datagram[2]);
                    const auto events = parse_history_events(payload);
                    ++history_packets_;
                    if (!events || events->size() != datagram[3] + 1U) {
                        ++history_unreadable_;
                    } else {
                        // Newest first in the packet: event j has the number sequence - j. Those not seen yet are told, oldest first.
                        for (std::size_t j = events->size(); j-- > 0;) {
                            const auto number = static_cast<std::uint16_t>(sequence - j);
                            const bool is_new = !history_sequence_ || (number != *history_sequence_ && static_cast<std::uint16_t>(number - *history_sequence_) < 0x8000U);
                            if (!is_new) continue;
                            ++history_events_;
                            events_.push_back(std::format("input {}: {}", number, describe((*events)[j])));
                            if (options_.input_event) options_.input_event((*events)[j]);
                        }
                        if (!history_sequence_ || static_cast<std::uint16_t>(sequence - *history_sequence_) < 0x8000U) history_sequence_ = sequence;
                    }
                }
                if (kind == packet_kind::kInputState && datagram.size() > kInputHeaderSize) {
                    // Its state is encrypted from sixteen bytes behind its key position on.
                    std::vector<std::uint8_t> state(datagram.begin() + kInputHeaderSize, datagram.end());
                    receive_keys_->cipher.apply_key_stream(receive_keys_->positions.last() + kPayloadKeyOffset, state);
                    const auto sequence = static_cast<std::uint16_t>((datagram[1] << 8) | datagram[2]);
                    if (input_sequence_ && static_cast<std::uint16_t>(*input_sequence_ + 1) != sequence) ++input_out_of_order_;
                    input_sequence_ = sequence;
                    if (!parse_input_state(options_.kind, state) || datagram[3] != 0) ++input_unreadable_;
                    else {
                        ++input_states_;
                        if (state == encode_input_state(options_.kind, InputState{})) ++input_at_rest_;
                    }
                }
            }
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
    while (next_heartbeat_ && now >= *next_heartbeat_) {
        send(out, make_plain_message(message_type::kHeartbeat), kOwnChannel, now);
        *next_heartbeat_ += options_.heartbeat_every;
    }
    while (next_quality_ && now >= *next_quality_) {
        ConnectionQuality quality;
        quality.target_bitrate = 9708000;
        quality.upstream_bitrate = 2;
        quality.upstream_loss = 0.0F;
        quality.disable_upstream_audio = false;
        quality.rtt = 200.0;
        quality.loss = 0;
        send(out, make_message(quality), kOwnChannel, now);
        *next_quality_ += options_.quality_every;
    }
    while (next_feedback_ && now >= *next_feedback_) {
        const auto data = [&](std::uint8_t kind, const std::vector<std::uint8_t>& content) {
            if (auto sent = messenger_.send(chunk_flag::kLast, kOwnChannel, content, now, kind)) out.push_back(std::move(sent->datagram));
        };
        if (feedback_step_++ == 0) {
            data(data_type::kRumble, encode_rumble(kMockRumble));
            data(data_type::kTriggerEffects, encode_trigger_effects(kMockTriggerEffects));
            data(data_type::kPadInfo, encode_controller_info(kMockControllerInfo));
            *next_feedback_ += std::chrono::milliseconds(400);
        } else {
            data(data_type::kRumble, encode_rumble(Rumble{}));
            next_feedback_.reset();
        }
    }
    while (next_frame_ && media_ && now >= *next_frame_) {
        send_frame(out);
        *next_frame_ += options_.frame_every;
        if (options_.frames != 0 && frames_sent_ >= options_.frames) {
            next_frame_.reset();
            // No frame follows for a held packet to go out behind.
            if (held_) out.push_back(std::move(*std::exchange(held_, std::nullopt)));
        }
    }
    while (next_media_ && media_ && now >= *next_media_) {
        send_sound(out);
        *next_media_ += options_.media_every;
    }
    return out;
}

// One picture frame, with what the options make go wrong with it.
void ConsoleStream::send_frame(Datagrams& out) {
    const std::uint16_t index = ++frame_index_;
    const bool keyframe = std::exchange(keyframe_wanted_, false);
    const auto frame = options_.picture_source ? options_.picture_source(index, keyframe)
                                               : pattern_video_frame(options_.seed, index, options_.frame_bytes, keyframe);
    const std::uint8_t codec = options_.picture_codec;
    Datagrams packets = media_->video_frame(index, 0, codec, frame, options_.unit_data, options_.repair_units);
    ++frames_sent_;
    events_.push_back(std::format("frame {} sent: {} bytes in {} packets{}", index, frame.size(), packets.size(), keyframe ? ", a keyframe" : ""));
    std::size_t first = 0;
    if (options_.lose_every != 0 && index % options_.lose_every == 0) {
        first = std::min<std::size_t>(options_.lose_units, packets.size());
        packets_withheld_ += first;
    }
    const bool late = options_.late_every != 0 && index % options_.late_every == 0 && packets.size() - first > 1;
    for (std::size_t i = first; i < packets.size(); ++i) {
        if (options_.corrupt_media) packets[i].back() ^= 0x01;
        if (late && i + 1 == packets.size()) {
            held_ = std::move(packets[i]);
            break;
        }
        out.push_back(std::move(packets[i]));
        // A packet held back from the frame before goes out behind this frame's first.
        if (i == first && held_ && !late) out.push_back(std::move(*std::exchange(held_, std::nullopt)));
    }
}

// One sound packet with a new frame and the frames before it again; now and then one for the haptics too.
void ConsoleStream::send_sound(Datagrams& out) {
    const auto packet_for = [&](std::uint16_t index, bool haptics) {
        if (options_.sound_source && !haptics) {
            // A source gives every frame once; the frames before it are sent again from what was
            // kept, and before there are any, the first frame stands in for them.
            const std::vector<std::vector<std::uint8_t>> fresh{options_.sound_source(index)};
            std::vector<std::vector<std::uint8_t>> repeated;
            for (unsigned back = options_.sound_repeats; back >= 1; --back) {
                repeated.push_back(back <= recent_sound_.size() ? recent_sound_[recent_sound_.size() - back] : fresh.front());
            }
            recent_sound_.push_back(fresh.front());
            if (recent_sound_.size() > options_.sound_repeats) recent_sound_.pop_front();
            return media_->sound_packet(index, fresh, repeated, haptics);
        }
        if (options_.haptics_source && haptics) {
            const std::vector<std::vector<std::uint8_t>> fresh{options_.haptics_source(index)};
            std::vector<std::vector<std::uint8_t>> repeated;
            for (unsigned back = options_.sound_repeats; back >= 1; --back) repeated.push_back(options_.haptics_source(static_cast<std::uint16_t>(index - back)));
            return media_->sound_packet(index, fresh, repeated, haptics);
        }
        const std::vector<std::vector<std::uint8_t>> fresh{pattern_sound_frame(options_.seed, index, options_.sound_bytes, haptics)};
        std::vector<std::vector<std::uint8_t>> repeated;
        for (unsigned back = options_.sound_repeats; back >= 1; --back) {
            repeated.push_back(pattern_sound_frame(options_.seed, static_cast<std::uint16_t>(index - back), options_.sound_bytes, haptics));
        }
        return media_->sound_packet(index, fresh, repeated, haptics);
    };
    const std::uint16_t index = ++sound_index_;
    ++sound_packets_;
    auto packet = packet_for(index, false);
    if (packet && options_.lose_sound_every != 0 && index % options_.lose_sound_every == 0) {
        ++packets_withheld_;
    } else if (packet) {
        if (options_.corrupt_media) packet->back() ^= 0x01;
        out.push_back(std::move(*packet));
    }
    if (options_.haptics_every != 0 && options_.kind == ConsoleKind::ps5 && sound_packets_ % options_.haptics_every == 0) {
        if (auto haptics = packet_for(++haptics_index_, true)) out.push_back(std::move(*haptics));
    }
    if (options_.other_stream_every != 0 && options_.kind == ConsoleKind::ps5 && sound_packets_ % options_.other_stream_every == 0) {
        const std::vector<std::vector<std::uint8_t>> fresh{pattern_sound_frame(options_.seed + 99, ++other_index_, 24, false)};
        if (auto other = media_->sound_packet(other_index_, fresh, {}, false, kMockOtherStream)) out.push_back(std::move(*other));
    }
}

std::optional<TimePoint> ConsoleStream::deadline() const {
    std::optional<TimePoint> when = messenger_.deadline();
    if (next_media_ && (!when || *next_media_ < *when)) when = next_media_;
    if (next_frame_ && (!when || *next_frame_ < *when)) when = next_frame_;
    if (next_heartbeat_ && (!when || *next_heartbeat_ < *when)) when = next_heartbeat_;
    if (next_quality_ && (!when || *next_quality_ < *when)) when = next_quality_;
    if (next_feedback_ && (!when || *next_feedback_ < *when)) when = next_feedback_;
    return when;
}

Datagrams ConsoleStream::disconnect(const std::string& reason, TimePoint now) {
    Datagrams out;
    send(out, make_message(DisconnectMessage{reason}), ClientStream::kSessionChannel, now);
    next_media_.reset();
    next_frame_.reset();
    next_heartbeat_.reset();
    next_quality_.reset();
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
    text += std::format(" input-states {} at-rest {} unreadable {} out-of-order {}", input_states_, input_at_rest_, input_unreadable_,
                        input_out_of_order_);
    text += std::format(" history-packets {} events {} unreadable {}", history_packets_, history_events_, history_unreadable_);
    text += std::format(" frames-sent {} sound-sent {} withheld {} congestion-received {} congestion-lost {}", frames_sent_, sound_packets_,
                        packets_withheld_, congestion_received_, congestion_lost_);
    return text;
}

}  // namespace psvp::proto::transport
