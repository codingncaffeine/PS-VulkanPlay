#include "proto/stream_message.h"

#include <initializer_list>
#include <utility>

#include "proto/wire.h"

namespace psvp::proto {
namespace {

using wire::Field;
using wire::Kind;
using wire::Reader;
using wire::Writer;

// Field numbers of the outer message.
constexpr std::uint32_t kFieldType = 1;
constexpr std::uint32_t kFieldBig = 2;
constexpr std::uint32_t kFieldBang = 3;
constexpr std::uint32_t kFieldDisconnect = 10;
constexpr std::uint32_t kFieldProbe = 14;
constexpr std::uint32_t kFieldProtocolRequest = 31;
constexpr std::uint32_t kFieldProtocolRequestAck = 32;

// The probe's commands and the field each one's body travels in.
constexpr std::uint32_t kProbeEcho = 0;
constexpr std::uint32_t kProbeMtu = 1;
constexpr std::uint32_t kProbeClientMtu = 4;
constexpr std::uint32_t kProbeFieldEcho = 2;
constexpr std::uint32_t kProbeFieldMtu = 3;
constexpr std::uint32_t kProbeFieldClientMtu = 5;

std::string text_of(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::uint8_t> copy_of(std::span<const std::uint8_t> bytes) { return {bytes.begin(), bytes.end()}; }

// Reads the fields of one message through `take`, which returns false for a
// field it knows but cannot use. `required` is the set of field numbers (as
// bits) that must all have been seen.
template <typename Take> bool read_fields(std::span<const std::uint8_t> bytes, std::uint32_t required, Take take) {
    Reader reader(bytes);
    std::uint32_t seen = 0;
    while (const auto field = reader.next()) {
        if (!take(*field)) return false;
        if (field->number < 32) seen |= 1U << field->number;
    }
    return reader.ok() && (seen & required) == required;
}

bool number(const Field& field, std::uint32_t& out) {
    if (field.kind != Kind::varint) return false;
    out = static_cast<std::uint32_t>(field.value);
    return true;
}

bool number(const Field& field, std::optional<std::uint32_t>& out) {
    if (field.kind != Kind::varint) return false;
    out = static_cast<std::uint32_t>(field.value);
    return true;
}

bool flag(const Field& field, bool& out) {
    if (field.kind != Kind::varint) return false;
    out = field.value != 0;
    return true;
}

constexpr std::uint32_t bits(std::initializer_list<std::uint32_t> numbers) {
    std::uint32_t out = 0;
    for (const std::uint32_t n : numbers) out |= 1U << n;
    return out;
}

std::optional<BigMessage> parse_big(std::span<const std::uint8_t> bytes) {
    BigMessage m;
    const bool ok = read_fields(bytes, bits({1, 2, 3, 4}), [&](const Field& f) {
        switch (f.number) {
        case 1: return number(f, m.client_version);
        case 2: if (f.kind != Kind::bytes) return false; m.session_key = text_of(f.bytes); return true;
        case 3: if (f.kind != Kind::bytes) return false; m.launch_spec = text_of(f.bytes); return true;
        case 4: if (f.kind != Kind::bytes) return false; m.encrypted_key = copy_of(f.bytes); return true;
        case 5: if (f.kind != Kind::bytes) return false; m.ecdh_public_key = copy_of(f.bytes); return true;
        case 6: if (f.kind != Kind::bytes) return false; m.ecdh_signature = copy_of(f.bytes); return true;
        default: return true;
        }
    });
    return ok ? std::optional(std::move(m)) : std::nullopt;
}

std::optional<BangMessage> parse_bang(std::span<const std::uint8_t> bytes) {
    BangMessage m;
    const bool ok = read_fields(bytes, bits({1, 2, 3, 4, 5}), [&](const Field& f) {
        switch (f.number) {
        case 1: return number(f, m.server_version);
        case 2: return number(f, m.token);
        case 3: return flag(f, m.encrypted_key_accepted);
        case 4: return flag(f, m.version_accepted);
        case 5: if (f.kind != Kind::bytes) return false; m.session_key = text_of(f.bytes); return true;
        case 8: if (f.kind != Kind::bytes) return false; m.ecdh_public_key = copy_of(f.bytes); return true;
        case 9: if (f.kind != Kind::bytes) return false; m.ecdh_signature = copy_of(f.bytes); return true;
        default: return true;
        }
    });
    return ok ? std::optional(std::move(m)) : std::nullopt;
}

std::optional<DisconnectMessage> parse_disconnect(std::span<const std::uint8_t> bytes) {
    DisconnectMessage m;
    const bool ok = read_fields(bytes, bits({1}), [&](const Field& f) {
        if (f.number != 1) return true;
        if (f.kind != Kind::bytes) return false;
        m.reason = text_of(f.bytes);
        return true;
    });
    return ok ? std::optional(std::move(m)) : std::nullopt;
}

std::optional<ProbeEcho> parse_probe_echo(std::span<const std::uint8_t> bytes) {
    ProbeEcho m;
    const bool ok = read_fields(bytes, bits({1}), [&](const Field& f) { return f.number != 1 || flag(f, m.state); });
    return ok ? std::optional(m) : std::nullopt;
}

std::optional<ProbeMtu> parse_probe_mtu(std::span<const std::uint8_t> bytes) {
    ProbeMtu m;
    const bool ok = read_fields(bytes, bits({1, 2}), [&](const Field& f) {
        switch (f.number) {
        case 1: return number(f, m.id);
        case 2: return number(f, m.mtu_request);
        case 3: return number(f, m.mtu_sent);
        case 4: return number(f, m.count);
        case 5: return number(f, m.send_delay);
        case 6: return number(f, m.delta);
        default: return true;
        }
    });
    return ok ? std::optional(m) : std::nullopt;
}

std::optional<ProbeClientMtu> parse_probe_client_mtu(std::span<const std::uint8_t> bytes) {
    ProbeClientMtu m;
    const bool ok = read_fields(bytes, bits({1, 2, 3}), [&](const Field& f) {
        switch (f.number) {
        case 1: return number(f, m.id);
        case 2: return number(f, m.mtu_request);
        case 3: return flag(f, m.state);
        case 4: return number(f, m.mtu_down);
        default: return true;
        }
    });
    return ok ? std::optional(m) : std::nullopt;
}

// The probe's body: a command number and the field that belongs to it.
bool parse_probe(std::span<const std::uint8_t> bytes, StreamMessage::Body& body) {
    std::uint32_t command = 0;
    std::span<const std::uint8_t> echo;
    std::span<const std::uint8_t> mtu;
    std::span<const std::uint8_t> client_mtu;
    bool have_echo = false;
    bool have_mtu = false;
    bool have_client_mtu = false;
    const bool ok = read_fields(bytes, bits({1}), [&](const Field& f) {
        switch (f.number) {
        case 1: return number(f, command);
        case kProbeFieldEcho: if (f.kind != Kind::bytes) return false; echo = f.bytes; have_echo = true; return true;
        case kProbeFieldMtu: if (f.kind != Kind::bytes) return false; mtu = f.bytes; have_mtu = true; return true;
        case kProbeFieldClientMtu: if (f.kind != Kind::bytes) return false; client_mtu = f.bytes; have_client_mtu = true; return true;
        default: return true;
        }
    });
    if (!ok) return false;
    if (command == kProbeEcho && have_echo) {
        const auto m = parse_probe_echo(echo);
        if (!m) return false;
        body = *m;
    } else if (command == kProbeMtu && have_mtu) {
        const auto m = parse_probe_mtu(mtu);
        if (!m) return false;
        body = *m;
    } else if (command == kProbeClientMtu && have_client_mtu) {
        const auto m = parse_probe_client_mtu(client_mtu);
        if (!m) return false;
        body = *m;
    }
    return true;
}

// Numbers of a repeated field, one per field or several packed into one.
bool parse_protocol_request(std::span<const std::uint8_t> bytes, ProtocolRequest& out) {
    constexpr std::size_t kMaxVersions = 32;
    return read_fields(bytes, 0, [&](const Field& f) {
        if (f.number != 1) return true;
        if (f.kind == Kind::varint) {
            if (out.versions.size() == kMaxVersions) return false;
            out.versions.push_back(static_cast<std::uint32_t>(f.value));
            return true;
        }
        if (f.kind != Kind::bytes) return false;
        // Packed: the numbers one after another, without field keys.
        std::span<const std::uint8_t> rest = f.bytes;
        while (!rest.empty()) {
            std::uint64_t value = 0;
            unsigned shift = 0;
            while (true) {
                if (rest.empty() || shift > 63) return false;
                const std::uint8_t byte = rest.front();
                rest = rest.subspan(1);
                value |= std::uint64_t{byte & 0x7fU} << shift;
                shift += 7;
                if ((byte & 0x80U) == 0) break;
            }
            if (out.versions.size() == kMaxVersions) return false;
            out.versions.push_back(static_cast<std::uint32_t>(value));
        }
        return true;
    });
}

void put_optional(Writer& w, std::uint32_t field, const std::optional<std::uint32_t>& value) {
    if (value) w.varint(field, *value);
}

void put_optional(Writer& w, std::uint32_t field, const std::optional<std::vector<std::uint8_t>>& value) {
    if (value) w.bytes(field, *value);
}

struct BodyWriter {
    Writer& out;

    void operator()(const std::monostate&) const {}
    void operator()(const BigMessage& m) const {
        Writer w;
        w.varint(1, m.client_version);
        w.text(2, m.session_key);
        w.text(3, m.launch_spec);
        w.bytes(4, m.encrypted_key);
        put_optional(w, 5, m.ecdh_public_key);
        put_optional(w, 6, m.ecdh_signature);
        out.message(kFieldBig, w);
    }
    void operator()(const BangMessage& m) const {
        Writer w;
        w.varint(1, m.server_version);
        w.varint(2, m.token);
        w.boolean(3, m.encrypted_key_accepted);
        w.boolean(4, m.version_accepted);
        w.text(5, m.session_key);
        put_optional(w, 8, m.ecdh_public_key);
        put_optional(w, 9, m.ecdh_signature);
        out.message(kFieldBang, w);
    }
    void operator()(const DisconnectMessage& m) const {
        Writer w;
        w.text(1, m.reason);
        out.message(kFieldDisconnect, w);
    }
    void operator()(const ProbeEcho& m) const {
        Writer inner;
        inner.boolean(1, m.state);
        probe(kProbeEcho, kProbeFieldEcho, inner);
    }
    void operator()(const ProbeMtu& m) const {
        Writer inner;
        inner.varint(1, m.id);
        inner.varint(2, m.mtu_request);
        put_optional(inner, 3, m.mtu_sent);
        put_optional(inner, 4, m.count);
        put_optional(inner, 5, m.send_delay);
        put_optional(inner, 6, m.delta);
        probe(kProbeMtu, kProbeFieldMtu, inner);
    }
    void operator()(const ProbeClientMtu& m) const {
        Writer inner;
        inner.varint(1, m.id);
        inner.varint(2, m.mtu_request);
        inner.boolean(3, m.state);
        put_optional(inner, 4, m.mtu_down);
        probe(kProbeClientMtu, kProbeFieldClientMtu, inner);
    }
    void operator()(const ProtocolRequest& m) const {
        Writer w;
        for (const std::uint32_t version : m.versions) w.varint(1, version);
        out.message(kFieldProtocolRequest, w);
    }
    void operator()(const ProtocolRequestAck& m) const {
        Writer w;
        put_optional(w, 1, m.version);
        out.message(kFieldProtocolRequestAck, w);
    }

    void probe(std::uint32_t command, std::uint32_t field, const Writer& inner) const {
        Writer w;
        w.varint(1, command);
        w.message(field, inner);
        out.message(kFieldProbe, w);
    }
};

struct TypeOf {
    std::uint32_t fallback;
    std::uint32_t operator()(const std::monostate&) const { return fallback; }
    std::uint32_t operator()(const BigMessage&) const { return message_type::kBig; }
    std::uint32_t operator()(const BangMessage&) const { return message_type::kBang; }
    std::uint32_t operator()(const DisconnectMessage&) const { return message_type::kDisconnect; }
    std::uint32_t operator()(const ProbeEcho&) const { return message_type::kProbe; }
    std::uint32_t operator()(const ProbeMtu&) const { return message_type::kProbe; }
    std::uint32_t operator()(const ProbeClientMtu&) const { return message_type::kProbe; }
    std::uint32_t operator()(const ProtocolRequest&) const { return message_type::kProtocolRequest; }
    std::uint32_t operator()(const ProtocolRequestAck&) const { return message_type::kProtocolRequestAck; }
};

}  // namespace

StreamMessage make_message(StreamMessage::Body body) {
    StreamMessage message;
    message.type = std::visit(TypeOf{0}, body);
    message.body = std::move(body);
    return message;
}

std::vector<std::uint8_t> encode_message(const StreamMessage& message) {
    Writer out;
    out.varint(kFieldType, message.type);
    std::visit(BodyWriter{out}, message.body);
    return out.take();
}

std::optional<StreamMessage> parse_message(std::span<const std::uint8_t> bytes) {
    StreamMessage message;
    bool have_type = false;
    // The body fields are kept as views until the type is known: it may come after them.
    std::span<const std::uint8_t> bodies[33];
    bool present[33] = {};
    Reader reader(bytes);
    while (const auto field = reader.next()) {
        if (field->number == kFieldType) {
            if (field->kind != Kind::varint) return std::nullopt;
            message.type = static_cast<std::uint32_t>(field->value);
            have_type = true;
        } else if (field->number <= 32 && field->kind == Kind::bytes) {
            bodies[field->number] = field->bytes;
            present[field->number] = true;
        }
    }
    if (!reader.ok() || !have_type) return std::nullopt;

    const auto take = [&](std::uint32_t field, auto parser) {
        if (!present[field]) return true;
        auto body = parser(bodies[field]);
        if (!body) return false;
        message.body = std::move(*body);
        return true;
    };
    bool ok = true;
    switch (message.type) {
    case message_type::kBig: ok = take(kFieldBig, parse_big); break;
    case message_type::kBang: ok = take(kFieldBang, parse_bang); break;
    case message_type::kDisconnect: ok = take(kFieldDisconnect, parse_disconnect); break;
    case message_type::kProbe:
        if (present[kFieldProbe]) ok = parse_probe(bodies[kFieldProbe], message.body);
        break;
    case message_type::kProtocolRequest:
        if (present[kFieldProtocolRequest]) {
            ProtocolRequest request;
            ok = parse_protocol_request(bodies[kFieldProtocolRequest], request);
            if (ok) message.body = std::move(request);
        }
        break;
    case message_type::kProtocolRequestAck:
        if (present[kFieldProtocolRequestAck]) {
            ProtocolRequestAck ack;
            ok = read_fields(bodies[kFieldProtocolRequestAck], 0, [&](const Field& f) { return f.number != 1 || number(f, ack.version); });
            if (ok) message.body = ack;
        }
        break;
    default: break;
    }
    return ok ? std::optional(std::move(message)) : std::nullopt;
}

}  // namespace psvp::proto
