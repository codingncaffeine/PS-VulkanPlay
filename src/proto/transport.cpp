#include "proto/transport.h"

#include <algorithm>

namespace psvp::proto::transport {
namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEndpointSize = 16;

// Writers into a buffer that already has its final size.
void put32(std::span<std::uint8_t> out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 24);
    out[at + 1] = static_cast<std::uint8_t>(value >> 16);
    out[at + 2] = static_cast<std::uint8_t>(value >> 8);
    out[at + 3] = static_cast<std::uint8_t>(value);
}

void put16(std::span<std::uint8_t> out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 8);
    out[at + 1] = static_cast<std::uint8_t>(value);
}

std::uint32_t get32(std::span<const std::uint8_t> in, std::size_t at) {
    return (std::uint32_t{in[at]} << 24) | (std::uint32_t{in[at + 1]} << 16) | (std::uint32_t{in[at + 2]} << 8) | std::uint32_t{in[at + 3]};
}

std::uint16_t get16(std::span<const std::uint8_t> in, std::size_t at) {
    return static_cast<std::uint16_t>((std::uint16_t{in[at]} << 8) | in[at + 1]);
}

// The five numbers of an endpoint, followed by `extra` bytes for the caller to fill.
std::vector<std::uint8_t> endpoint_payload(const Endpoint& endpoint, std::size_t extra = 0) {
    std::vector<std::uint8_t> out(kEndpointSize + extra);
    put32(out, 0, endpoint.tag);
    put32(out, 4, endpoint.receive_window);
    put16(out, 8, endpoint.outbound_streams);
    put16(out, 10, endpoint.inbound_streams);
    put32(out, 12, endpoint.first_sequence);
    return out;
}

Endpoint get_endpoint(std::span<const std::uint8_t> in) {
    return Endpoint{get32(in, 0), get32(in, 4), get16(in, 8), get16(in, 10), get32(in, 12)};
}

}  // namespace

std::vector<std::uint8_t> encode_control(const ControlPacket& packet) {
    std::vector<std::uint8_t> out(1 + kHeaderSize + packet.payload.size());
    out[0] = packet_kind::kControl;
    put32(out, 1, packet.tag);
    std::copy(packet.mac.begin(), packet.mac.end(), out.begin() + 5);
    put32(out, 9, packet.key_position);
    out[13] = packet.chunk_type;
    out[14] = packet.chunk_flags;
    put16(out, 15, static_cast<std::uint16_t>(packet.payload.size() + 4));
    std::copy(packet.payload.begin(), packet.payload.end(), out.begin() + 1 + kHeaderSize);
    return out;
}

std::optional<ControlPacket> parse_control(std::span<const std::uint8_t> datagram) {
    if (datagram.size() < 1 + kHeaderSize || (datagram[0] & 0x0f) != packet_kind::kControl) return std::nullopt;
    const auto message = datagram.subspan(1);
    const std::size_t size_field = get16(message, 14);
    if (size_field < 4 || message.size() != size_field + 12) return std::nullopt;

    ControlPacket packet;
    packet.tag = get32(message, 0);
    std::copy_n(message.begin() + 4, 4, packet.mac.begin());
    packet.key_position = get32(message, 8);
    packet.chunk_type = message[12];
    packet.chunk_flags = message[13];
    packet.payload.assign(message.begin() + kHeaderSize, message.end());
    return packet;
}

ClientHandshake::ClientHandshake(std::uint32_t tag) : tag_(tag) {}

std::vector<std::uint8_t> ClientHandshake::init_packet() const {
    ControlPacket packet;
    packet.tag = 0;  // the console's tag is not known yet
    packet.chunk_type = chunk::kInit;
    packet.payload = endpoint_payload(Endpoint{tag_, kReceiveWindow, kStreams, kStreams, tag_});
    return encode_control(packet);
}

std::vector<std::uint8_t> ClientHandshake::cookie_packet() const {
    ControlPacket packet;
    packet.tag = console_.tag;
    packet.chunk_type = chunk::kCookie;
    packet.payload.assign(cookie_.begin(), cookie_.end());
    return encode_control(packet);
}

std::optional<std::vector<std::uint8_t>> ClientHandshake::fail(std::string why) {
    state_ = State::failed;
    failure_ = std::move(why);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> ClientHandshake::start(TimePoint now) {
    if (state_ != State::idle) return std::nullopt;
    state_ = State::awaiting_init_ack;
    tries_ = 1;
    deadline_ = now + kAnswerWait;
    return init_packet();
}

std::optional<std::vector<std::uint8_t>> ClientHandshake::on_datagram(std::span<const std::uint8_t> datagram, TimePoint now) {
    if (state_ != State::awaiting_init_ack && state_ != State::awaiting_cookie_ack) return std::nullopt;
    const auto packet = parse_control(datagram);
    // Anything that is not for this connection is not an answer; the wait goes on.
    if (!packet || packet->tag != tag_) return std::nullopt;

    if (state_ == State::awaiting_init_ack) {
        if (packet->chunk_type != chunk::kInitAck || packet->chunk_flags != 0 ||
            packet->payload.size() != kEndpointSize + kCookieSize) {
            return fail("the console's answer to the transport's opening was not an acknowledgment");
        }
        console_ = get_endpoint(packet->payload);
        if (console_.tag == 0) return fail("the console announced no tag");
        if (console_.outbound_streams == 0 || console_.inbound_streams == 0 || console_.outbound_streams > kStreams ||
            console_.inbound_streams < kStreams) {
            return fail("the console announced stream counts this client cannot work with");
        }
        std::copy_n(packet->payload.begin() + kEndpointSize, kCookieSize, cookie_.begin());
        state_ = State::awaiting_cookie_ack;
        tries_ = 1;
        deadline_ = now + kAnswerWait;
        return cookie_packet();
    }

    // Waiting for the cookie's acknowledgment: a repeated init acknowledgment is ignored.
    if (packet->chunk_type == chunk::kInitAck) return std::nullopt;
    if (packet->chunk_type != chunk::kCookieAck || packet->chunk_flags != 0 || !packet->payload.empty()) {
        return fail("the console's answer to the cookie was not an acknowledgment");
    }
    state_ = State::connected;
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> ClientHandshake::on_timeout(TimePoint now) {
    if (state_ != State::awaiting_init_ack && state_ != State::awaiting_cookie_ack) return std::nullopt;
    if (now < deadline_) return std::nullopt;
    if (tries_ >= kTries) {
        return fail(state_ == State::awaiting_init_ack ? "the console did not answer the transport's opening"
                                                         : "the console did not acknowledge the transport's cookie");
    }
    ++tries_;
    deadline_ = now + kAnswerWait;
    return state_ == State::awaiting_init_ack ? init_packet() : cookie_packet();
}

ConsoleHandshake::ConsoleHandshake(std::uint32_t tag, const std::array<std::uint8_t, kCookieSize>& cookie)
    : tag_(tag), cookie_(cookie) {}

std::optional<std::vector<std::uint8_t>> ConsoleHandshake::on_datagram(std::span<const std::uint8_t> datagram) {
    const auto packet = parse_control(datagram);
    if (!packet) return std::nullopt;

    if (packet->chunk_type == chunk::kInit && packet->payload.size() == kEndpointSize) {
        client_ = get_endpoint(packet->payload);
        init_ = *packet;
        init_->payload.clear();
        ControlPacket answer;
        answer.tag = client_->tag;
        answer.chunk_type = chunk::kInitAck;
        answer.payload = endpoint_payload(Endpoint{tag_, kReceiveWindow, kStreams, kStreams, tag_}, kCookieSize);
        std::copy(cookie_.begin(), cookie_.end(), answer.payload.begin() + kEndpointSize);
        return encode_control(answer);
    }
    if (packet->chunk_type == chunk::kCookie && client_ && packet->tag == tag_ &&
        std::ranges::equal(packet->payload, cookie_)) {
        connected_ = true;
        ControlPacket answer;
        answer.tag = client_->tag;
        answer.chunk_type = chunk::kCookieAck;
        return encode_control(answer);
    }
    return std::nullopt;
}

// ---- Messages that must arrive -------------------------------------------

namespace {

constexpr std::size_t kDataHeaderSize = 9;
constexpr std::size_t kDataAckSize = 12;

}  // namespace

std::vector<std::uint8_t> encode_data_chunk(const DataChunk& chunk) {
    std::vector<std::uint8_t> out(kDataHeaderSize + chunk.data.size());
    put32(out, 0, chunk.sequence);
    put16(out, 4, chunk.channel);
    // Bytes 6 and 7 stay zero.
    out[8] = chunk.data_type;
    std::copy(chunk.data.begin(), chunk.data.end(), out.begin() + kDataHeaderSize);
    return out;
}

std::optional<DataChunk> parse_data_chunk(std::span<const std::uint8_t> payload) {
    if (payload.size() < kDataHeaderSize) return std::nullopt;
    DataChunk chunk;
    chunk.sequence = get32(payload, 0);
    chunk.channel = get16(payload, 4);
    chunk.data_type = payload[8];
    chunk.data.assign(payload.begin() + kDataHeaderSize, payload.end());
    return chunk;
}

std::vector<std::uint8_t> encode_data_ack(const DataAck& ack) {
    std::vector<std::uint8_t> out(kDataAckSize);
    put32(out, 0, ack.cumulative);
    put32(out, 4, ack.receive_window);
    put16(out, 8, ack.gap_blocks);
    put16(out, 10, ack.duplicates);
    return out;
}

std::optional<DataAck> parse_data_ack(std::span<const std::uint8_t> payload) {
    // Gap blocks would follow the fixed part; no side sends any.
    if (payload.size() != kDataAckSize) return std::nullopt;
    DataAck ack{get32(payload, 0), get32(payload, 4), get16(payload, 8), get16(payload, 10)};
    if (ack.gap_blocks != 0) return std::nullopt;
    return ack;
}

Messenger::Messenger(std::uint32_t local_tag, std::uint32_t remote_tag, std::uint32_t first_local_sequence,
                     std::uint32_t first_remote_sequence)
    : local_tag_(local_tag), remote_tag_(remote_tag), next_local_(first_local_sequence), next_remote_(first_remote_sequence) {}

std::optional<Messenger::Sent> Messenger::send(std::uint8_t flags, std::uint16_t channel, std::span<const std::uint8_t> data,
                                               TimePoint now) {
    if (unacknowledged_.size() >= kWindow) return std::nullopt;
    ControlPacket packet;
    packet.tag = remote_tag_;
    packet.chunk_type = chunk::kData;
    packet.chunk_flags = flags;
    DataChunk chunk;
    chunk.sequence = next_local_++;
    chunk.channel = channel;
    chunk.data.assign(data.begin(), data.end());
    packet.payload = encode_data_chunk(chunk);
    Sent sent{chunk.sequence, encode_control(packet)};
    unacknowledged_.push_back(Pending{chunk.sequence, 0, now, sent.datagram});
    return sent;
}

Messenger::Received Messenger::on_packet(const ControlPacket& packet) {
    Received result;
    if (packet.tag != local_tag_) return result;

    if (packet.chunk_type == chunk::kDataAck) {
        const auto ack = parse_data_ack(packet.payload);
        if (!ack) return result;
        std::erase_if(unacknowledged_, [&](const Pending& pending) {
            const bool covered = pending.sequence == ack->cumulative || sequence_before(pending.sequence, ack->cumulative);
            if (covered) result.acknowledged.push_back(pending.sequence);
            return covered;
        });
        return result;
    }
    if (packet.chunk_type != chunk::kData) return result;
    auto chunk = parse_data_chunk(packet.payload);
    if (!chunk) return result;
    chunk->flags = packet.chunk_flags;

    const std::uint32_t ahead = chunk->sequence - next_remote_;
    bool acknowledge = false;
    if (ahead < kWindow) {
        if (!early_[ahead]) early_[ahead] = std::move(*chunk);
        // Hand over what is now in order.
        while (early_[0]) {
            result.delivered.push_back(std::move(*early_[0]));
            std::move(early_.begin() + 1, early_.end(), early_.begin());
            early_.back().reset();
            ++next_remote_;
            delivered_any_ = true;
            acknowledge = true;
        }
    } else if (sequence_before(chunk->sequence, next_remote_)) {
        // Seen before: the acknowledgment for it was lost, so it is said again.
        acknowledge = delivered_any_;
    }
    if (acknowledge) {
        ControlPacket reply;
        reply.tag = remote_tag_;
        reply.chunk_type = chunk::kDataAck;
        reply.payload = encode_data_ack(DataAck{next_remote_ - 1, kReceiveWindow, 0, 0});
        result.reply = encode_control(reply);
    }
    return result;
}

Messenger::Timer Messenger::on_timer(TimePoint now) {
    Timer timer;
    std::erase_if(unacknowledged_, [&](Pending& pending) {
        if (now < pending.last_sent + kResendAfter) return false;
        if (pending.resends >= kResends) {
            timer.abandoned.push_back(pending.sequence);
            return true;
        }
        ++pending.resends;
        pending.last_sent = now;
        timer.resend.push_back(pending.datagram);
        return false;
    });
    return timer;
}

std::optional<TimePoint> Messenger::deadline() const {
    std::optional<TimePoint> earliest;
    for (const Pending& pending : unacknowledged_) {
        const TimePoint due = pending.last_sent + kResendAfter;
        if (!earliest || due < *earliest) earliest = due;
    }
    return earliest;
}

// ---- Picture and sound packets, as the network probe uses them ------------

std::vector<std::uint8_t> encode_media(const MediaHeader& header, std::size_t datagram_size) {
    std::vector<std::uint8_t> out(std::max(datagram_size, header.size()));
    out[0] = static_cast<std::uint8_t>((header.video ? packet_kind::kVideo : packet_kind::kAudio) | (header.unit_info ? 0x10 : 0));
    put16(out, 1, header.packet_index);
    put16(out, 3, header.frame_index);
    put32(out, 5,
          (std::uint32_t{header.units_repair} & 0x3ffU) | (((std::uint32_t{header.units_total} - 1U) & 0x7ffU) << 10) |
              ((std::uint32_t{header.unit_index} & 0x7ffU) << 21));
    out[9] = header.codec;
    // Bytes 10 to 13 stay zero.
    put32(out, 14, header.key_position);
    if (header.video) {
        put16(out, 18, header.video_word);
        out[20] = static_cast<std::uint8_t>(header.adaptive_stream << 5);
    }
    return out;
}

std::optional<MediaPacket> parse_media(std::span<const std::uint8_t> datagram) {
    if (datagram.empty()) return std::nullopt;
    const std::uint8_t kind = datagram[0] & 0x0f;
    if (kind != packet_kind::kVideo && kind != packet_kind::kAudio) return std::nullopt;
    MediaPacket packet;
    MediaHeader& header = packet.header;
    header.video = kind == packet_kind::kVideo;
    header.unit_info = (datagram[0] & 0x10) != 0;
    if (datagram.size() < header.size()) return std::nullopt;
    header.packet_index = get16(datagram, 1);
    header.frame_index = get16(datagram, 3);
    const std::uint32_t units = get32(datagram, 5);
    header.unit_index = static_cast<std::uint16_t>((units >> 21) & 0x7ffU);
    header.units_total = static_cast<std::uint16_t>(((units >> 10) & 0x7ffU) + 1U);
    header.units_repair = static_cast<std::uint16_t>(units & 0x3ffU);
    header.codec = datagram[9];
    header.key_position = get32(datagram, 14);
    if (header.video) {
        header.video_word = get16(datagram, 18);
        header.adaptive_stream = static_cast<std::uint8_t>(datagram[20] >> 5);
    }
    packet.data = datagram.subspan(header.size());
    return packet;
}

}  // namespace psvp::proto::transport
