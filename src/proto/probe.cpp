#include "proto/probe.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "proto/hex.h"

namespace psvp::proto::transport {
namespace {

constexpr std::uint8_t kMessageFlags = 1;
constexpr std::uint16_t kSessionChannel = 1;  // the opening messages and the goodbye
constexpr std::uint16_t kProbeChannel = 8;    // the probe's own commands
constexpr std::uint8_t kPingCodec = 0xff;
constexpr std::uint16_t kPingUnits = 0x800;
constexpr std::string_view kGoodbye = "Client Disconnecting";
// What the size pings are filled with after their tag. Other clients send
// these six letters and consoles answer them; nothing else has been tried.
constexpr std::string_view kFiller = "CHIAKI";

MediaHeader ping_header(std::uint16_t frame, std::uint16_t unit) {
    MediaHeader header;
    header.video = false;
    header.codec = kPingCodec;
    header.frame_index = frame;
    header.unit_index = unit;
    header.units_total = kPingUnits;
    return header;
}

// A ping carries four zero bytes and then its tag, most significant byte first.
void put_tag(std::vector<std::uint8_t>& datagram, std::size_t header_size, std::uint32_t tag) {
    datagram[header_size + 4] = static_cast<std::uint8_t>(tag >> 24);
    datagram[header_size + 5] = static_cast<std::uint8_t>(tag >> 16);
    datagram[header_size + 6] = static_cast<std::uint8_t>(tag >> 8);
    datagram[header_size + 7] = static_cast<std::uint8_t>(tag);
}

std::optional<std::uint32_t> tag_of(const MediaPacket& packet) {
    if (packet.data.size() < 8) return std::nullopt;
    return (std::uint32_t{packet.data[4]} << 24) | (std::uint32_t{packet.data[5]} << 16) | (std::uint32_t{packet.data[6]} << 8) |
           std::uint32_t{packet.data[7]};
}

}  // namespace

ClientProbe::ClientProbe(std::uint32_t local_tag, const Endpoint& console, std::function<std::uint32_t()> random)
    : messenger_(local_tag, console.tag, local_tag, console.first_sequence), random_(std::move(random)) {}

TimePoint ClientProbe::deadline() const noexcept {
    const auto resend = messenger_.deadline();
    return resend && *resend < step_deadline_ ? *resend : step_deadline_;
}

void ClientProbe::fail(std::string why) {
    state_ = State::failed;
    step_ = Step::none;
    failure_ = std::move(why);
    result_ = ProbeResult{};
}

void ClientProbe::send_message(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now, bool await_ack) {
    const auto bytes = encode_message(message);
    auto sent = messenger_.send(kMessageFlags, channel, bytes, now);
    if (!sent) {
        fail("too many of the probe's messages are unanswered");
        return;
    }
    awaited_ack_ = await_ack ? std::optional(sent->sequence) : std::nullopt;
    out.push_back(std::move(sent->datagram));
}

Datagrams ClientProbe::start(TimePoint now) {
    Datagrams out;
    if (state_ != State::idle) return out;
    state_ = State::running;
    step_ = Step::protocol;
    step_deadline_ = now + kAnswerWait;
    send_message(out, make_message(ProtocolRequest{{kProtocolVersion}}), kSessionChannel, now, false);
    return out;
}

void ClientProbe::send_ping(Datagrams& out, TimePoint now) {
    const MediaHeader header = ping_header(0, static_cast<std::uint16_t>(ping_index_));
    std::vector<std::uint8_t> datagram = encode_media(header, kPingSize);
    ping_tag_ = random_();
    put_tag(datagram, header.size(), ping_tag_);
    ping_sent_ = now;
    step_ = Step::ping;
    step_deadline_ = now + kPongWait;
    out.push_back(std::move(datagram));
}

void ClientProbe::begin_search(std::uint32_t first) {
    low_ = kMtuFloor;
    high_ = kMtuCeiling;
    current_ = first;
    attempt_ = 0;
    ceiling_tried_ = first == kMtuCeiling;
}

bool ClientProbe::narrow(bool passed) {
    if (passed) low_ = current_;
    else high_ = current_;
    attempt_ = 0;
    if (high_ - low_ > 1) {
        current_ = low_ + (high_ - low_) / 2;
        return true;
    }
    // A search that did not start at the ceiling has never tried it: once, at the end.
    if (!ceiling_tried_ && high_ == kMtuCeiling && low_ < high_) {
        ceiling_tried_ = true;
        current_ = high_;
        return true;
    }
    return false;
}

void ClientProbe::send_size_request(Datagrams& out, TimePoint now) {
    ProbeMtu request;
    request.id = ++request_id_;
    if (attempt_ == 0) first_id_of_size_ = request.id;
    request.mtu_request = current_;
    step_ = Step::size_in;
    step_deadline_ = now + size_wait_;
    send_message(out, make_message(request), kProbeChannel, now, false);
}

void ClientProbe::send_size_ping(Datagrams& out, TimePoint now) {
    const MediaHeader header = ping_header(0, static_cast<std::uint16_t>(attempt_));
    std::vector<std::uint8_t> datagram = encode_media(header, current_ - kIpUdpOverhead);
    ping_tag_ = random_();
    size_ping_tags_[static_cast<std::size_t>(attempt_)] = ping_tag_;
    put_tag(datagram, header.size(), ping_tag_);
    for (std::size_t i = header.size() + 8; i < datagram.size(); ++i) {
        datagram[i] = static_cast<std::uint8_t>(kFiller[(i - header.size() - 8) % kFiller.size()]);
    }
    step_ = Step::size_out;
    step_deadline_ = now + size_wait_;
    out.push_back(std::move(datagram));
}

void ClientProbe::after_size_in(Datagrams& out, TimePoint now) {
    result_.mtu_in = low_;
    // The client's own test is announced first; the console says the same back.
    step_ = Step::client_size_begin;
    step_deadline_ = now + kAnswerWait;
    send_message(out, make_message(ProbeClientMtu{1, result_.mtu_in, true, result_.mtu_in}), kProbeChannel, now, false);
}

void ClientProbe::after_size_out(Datagrams& out, TimePoint now) {
    result_.mtu_out = low_;
    step_ = Step::client_size_end;
    step_deadline_ = now + kAnswerWait;
    send_message(out, make_message(ProbeClientMtu{2, high_, false, result_.mtu_in}), kProbeChannel, now, true);
}

void ClientProbe::on_message(Datagrams& out, const StreamMessage& message, TimePoint now) {
    if (step_ == Step::protocol && message.as<ProtocolRequestAck>() != nullptr) {
        step_ = Step::big;
        step_deadline_ = now + kAnswerWait;
        BigMessage big;
        big.client_version = kProtocolVersion;
        send_message(out, make_message(std::move(big)), kSessionChannel, now, false);
    } else if (step_ == Step::big && message.as<BangMessage>() != nullptr) {
        step_ = Step::echo_on;
        step_deadline_ = now + kAnswerWait;
        send_message(out, make_message(ProbeEcho{true}), kProbeChannel, now, true);
    } else if (step_ == Step::client_size_begin) {
        const auto* echoed = message.as<ProbeClientMtu>();
        if (echoed == nullptr || echoed->id != 1) return;
        begin_search(result_.mtu_in);
        send_size_ping(out, now);
    }
}

void ClientProbe::on_acknowledged(Datagrams& out, TimePoint now) {
    awaited_ack_.reset();
    if (step_ == Step::echo_on) {
        ping_index_ = 0;
        send_ping(out, now);
    } else if (step_ == Step::echo_off) {
        if (pongs_ == 0) {
            fail("the console answered none of the pings");
            return;
        }
        result_.round_trip = round_trip_total_ / pongs_;
        size_wait_ = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(result_.round_trip * 5), kSizeWaitFloor,
                                kSizeWaitCeiling);
        begin_search(kMtuCeiling);
        send_size_request(out, now);
    } else if (step_ == Step::client_size_end) {
        step_ = Step::none;
        state_ = State::done;
        send_message(out, make_message(DisconnectMessage{std::string(kGoodbye)}), kSessionChannel, now, false);
    }
}

void ClientProbe::on_media(Datagrams& out, const MediaPacket& packet, TimePoint now) {
    const MediaHeader& header = packet.header;
    if (step_ == Step::ping) {
        if (header.video || header.frame_index != 0 || header.unit_index != ping_index_ || tag_of(packet) != ping_tag_) return;
        round_trip_total_ += std::chrono::duration_cast<std::chrono::microseconds>(now - ping_sent_);
        ++pongs_;
        if (++ping_index_ < kPings) {
            send_ping(out, now);
        } else {
            step_ = Step::echo_off;
            step_deadline_ = now + kAnswerWait;
            send_message(out, make_message(ProbeEcho{false}), kProbeChannel, now, true);
        }
    } else if (step_ == Step::size_out) {
        // The answer to any of this size's pings counts, also one that comes late.
        if (header.video || header.frame_index != 0 || header.unit_index > attempt_) return;
        if (tag_of(packet) != size_ping_tags_[header.unit_index]) return;
        if (narrow(true)) send_size_ping(out, now);
        else after_size_out(out, now);
    } else if (step_ == Step::size_in) {
        if (!header.video || header.frame_index < first_id_of_size_ || header.frame_index > request_id_) return;
        if (narrow(true)) send_size_request(out, now);
        else after_size_in(out, now);
    }
}

Datagrams ClientProbe::on_datagram(std::span<const std::uint8_t> datagram, TimePoint now) {
    Datagrams out;
    if (state_ != State::running) return out;
    if (const auto packet = parse_control(datagram)) {
        Messenger::Received received = messenger_.on_packet(*packet);
        if (received.reply) out.push_back(std::move(*received.reply));
        for (const std::uint32_t sequence : received.acknowledged) {
            if (state_ == State::running && awaited_ack_ == sequence) on_acknowledged(out, now);
        }
        for (const DataChunk& chunk : received.delivered) {
            if (state_ != State::running || chunk.data_type != data_type::kMessage) continue;
            if (const auto message = parse_message(chunk.data)) on_message(out, *message, now);
        }
    } else if (const auto media = parse_media(datagram)) {
        on_media(out, *media, now);
    }
    return out;
}

Datagrams ClientProbe::on_timeout(TimePoint now) {
    Datagrams out;
    if (state_ != State::running) return out;
    Messenger::Timer timer = messenger_.on_timer(now);
    for (auto& datagram : timer.resend) out.push_back(std::move(datagram));
    if (now < step_deadline_) return out;

    switch (step_) {
    case Step::protocol: fail("the console did not answer the probe's version request"); break;
    case Step::big: fail("the console did not answer the probe's opening message"); break;
    case Step::echo_on:
    case Step::echo_off: fail("the console did not acknowledge the probe's echo command"); break;
    case Step::client_size_begin: fail("the console did not answer the start of the outbound size test"); break;
    case Step::client_size_end: fail("the console did not acknowledge the end of the outbound size test"); break;
    case Step::ping:
        // A lost ping is skipped; the average is over those that came back.
        if (++ping_index_ < kPings) {
            send_ping(out, now);
        } else {
            step_ = Step::echo_off;
            step_deadline_ = now + kAnswerWait;
            send_message(out, make_message(ProbeEcho{false}), kProbeChannel, now, true);
        }
        break;
    case Step::size_in:
        if (++attempt_ < kSizeTries) send_size_request(out, now);
        else if (narrow(false)) send_size_request(out, now);
        else after_size_in(out, now);
        break;
    case Step::size_out:
        if (++attempt_ < kSizeTries) {
            send_size_ping(out, now);
        } else if (narrow(false)) {
            send_size_ping(out, now);
        } else {
            after_size_out(out, now);
        }
        break;
    case Step::none: break;
    }
    return out;
}

ConsoleProbe::ConsoleProbe(std::uint32_t local_tag, const Endpoint& client, Path path)
    : messenger_(local_tag, client.tag, local_tag, client.first_sequence), path_(path) {}

std::vector<std::string> ConsoleProbe::take_events() { return std::exchange(events_, {}); }

Datagrams ConsoleProbe::on_timer(TimePoint now) {
    Messenger::Timer timer = messenger_.on_timer(now);
    return std::move(timer.resend);
}

void ConsoleProbe::on_message(Datagrams& out, const DataChunk& chunk, std::uint8_t flags, TimePoint now) {
    events_.push_back(std::format("message channel {} flags {} type {}: {}", chunk.channel, flags, chunk.data_type, hex_encode(chunk.data)));
    if (chunk.data_type != data_type::kMessage) return;
    const auto message = parse_message(chunk.data);
    if (!message) {
        events_.emplace_back("message not understood");
        return;
    }
    const auto answer = [&](const StreamMessage& reply, std::uint16_t channel) {
        const auto bytes = encode_message(reply);
        if (auto sent = messenger_.send(kMessageFlags, channel, bytes, now)) out.push_back(std::move(sent->datagram));
    };
    if (const auto* request = message->as<ProtocolRequest>()) {
        const bool offered = std::ranges::find(request->versions, ClientProbe::kProtocolVersion) != request->versions.end();
        answer(make_message(ProtocolRequestAck{offered ? std::optional(ClientProbe::kProtocolVersion) : std::nullopt}), kSessionChannel);
    } else if (message->as<BigMessage>() != nullptr) {
        BangMessage bang;
        bang.server_version = ClientProbe::kProtocolVersion;
        bang.encrypted_key_accepted = true;
        bang.version_accepted = true;
        answer(make_message(std::move(bang)), kSessionChannel);
    } else if (const auto* echo = message->as<ProbeEcho>()) {
        echo_ = echo->state;
    } else if (const auto* size = message->as<ProbeMtu>()) {
        // One picture packet of the size asked for, if the way to the client lets it through.
        if (size->mtu_request >= kMtuFloor && size->mtu_request <= path_.mtu_in) {
            MediaHeader header;
            header.video = true;
            header.frame_index = static_cast<std::uint16_t>(size->id);
            // Picture packets are counted: a client may hand them on in the order of this number only.
            header.packet_index = next_picture_index_++;
            out.push_back(encode_media(header, size->mtu_request - kIpUdpOverhead));
        }
    } else if (const auto* client_size = message->as<ProbeClientMtu>()) {
        client_size_test_ = client_size->state;
        if (client_size->state) answer(make_message(*client_size), kProbeChannel);
    } else if (const auto* goodbye = message->as<DisconnectMessage>()) {
        disconnected_ = true;
        events_.push_back(std::format("goodbye: {}", goodbye->reason));
    }
}

Datagrams ConsoleProbe::on_datagram(std::span<const std::uint8_t> datagram, TimePoint now) {
    Datagrams out;
    // A datagram too large for the way here never arrived.
    if (datagram.size() + kIpUdpOverhead > path_.mtu_out) return out;
    if (const auto packet = parse_control(datagram)) {
        Messenger::Received received = messenger_.on_packet(*packet);
        if (received.reply) out.push_back(std::move(*received.reply));
        for (const DataChunk& chunk : received.delivered) on_message(out, chunk, chunk.flags, now);
    } else if (const auto media = parse_media(datagram)) {
        events_.push_back(std::format("ping {} bytes frame {} unit {} units {} codec {:#04x}", datagram.size(), media->header.frame_index,
                                      media->header.unit_index, media->header.units_total, media->header.codec));
        // While echo is on, and during the client's size test, a sound packet goes back as it came.
        if (!media->header.video && (echo_ || client_size_test_)) out.emplace_back(datagram.begin(), datagram.end());
    }
    return out;
}

}  // namespace psvp::proto::transport
