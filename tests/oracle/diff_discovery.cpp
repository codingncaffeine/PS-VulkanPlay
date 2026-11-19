// Discovery: our request datagrams and our reading of replies against the reference's.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <netinet/in.h>

#include <chiaki/discovery.h>

#include "oracle.h"
#include "proto/discovery.h"

// Defined by the reference's library; its header does not declare it.
extern "C" ChiakiErrorCode chiaki_discovery_srch_response_parse(ChiakiDiscoveryHost* response, struct sockaddr* addr,
                                                               char* addr_buf, size_t addr_buf_size, char* buf,
                                                               size_t buf_size);

namespace psvp::oracle {
namespace {

using proto::ConsoleKind;

std::string reference_packet(ChiakiDiscoveryCmd command, ConsoleKind kind, std::uint64_t credential) {
    std::string version(proto::discovery_version(kind));
    ChiakiDiscoveryPacket packet{};
    packet.cmd = command;
    packet.protocol_version = version.data();
    packet.user_credential = credential;
    char buffer[512];
    const int length = chiaki_discovery_packet_fmt(buffer, sizeof(buffer), &packet);
    return length < 0 ? std::string("?") : std::string(buffer, static_cast<std::size_t>(length));
}

struct ReferenceReply {
    bool ok = false;
    int state = 0;
    unsigned port = 0;
    std::string fields[7];
};

ReferenceReply reference_parse(std::string text) {
    ReferenceReply reply;
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    char address_text[64];
    ChiakiDiscoveryHost host{};
    // The reference writes into the buffer it is given and expects a terminator after it.
    text.push_back('\0');
    if (chiaki_discovery_srch_response_parse(&host, reinterpret_cast<struct sockaddr*>(&address), address_text,
                                             sizeof(address_text), text.data(), text.size() - 1) != CHIAKI_ERR_SUCCESS) {
        return reply;
    }
    reply.ok = true;
    reply.state = static_cast<int>(host.state);
    reply.port = host.host_request_port;
    const char* values[7] = {host.host_id,       host.host_name,           host.host_type, host.system_version,
                             host.device_discovery_protocol_version, host.running_app_titleid, host.running_app_name};
    for (int i = 0; i < 7; ++i) reply.fields[i] = values[i] != nullptr ? values[i] : "";
    return reply;
}

int state_number(proto::ConsoleState state) {
    switch (state) {
        case proto::ConsoleState::ready: return CHIAKI_DISCOVERY_HOST_STATE_READY;
        case proto::ConsoleState::standby: return CHIAKI_DISCOVERY_HOST_STATE_STANDBY;
        case proto::ConsoleState::unknown: break;
    }
    return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
}

std::string random_word(Random& random, std::size_t max_length) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_'";
    std::string word(1 + random.below(static_cast<std::uint32_t>(max_length)), 'x');
    for (char& c : word) c = alphabet[random.below(sizeof(alphabet) - 1)];
    if (word.front() == ' ') word.front() = 'x';  // one leading space is not part of a value on either side
    return word;
}

}  // namespace

Outcome diff_discovery() {
    Outcome outcome;
    Random random(0x70737670'64697321ULL);
    const auto compare = [&](bool equal, const char* what) {
        ++outcome.compared;
        if (!equal && ++outcome.mismatches <= 5) std::fprintf(stderr, "    MISMATCH discovery %s\n", what);
    };

    // The two request datagrams: the reference formats the text and sends one more byte, the terminator.
    for (int round = 0; round < 2000; ++round) {
        const ConsoleKind kind = (round & 1) != 0 ? ConsoleKind::ps5 : ConsoleKind::ps4;
        const std::uint64_t credential = round < 8 ? static_cast<std::uint64_t>(round) : random.next();
        const std::string search = proto::discovery_search_packet(kind);
        const std::string wake = proto::discovery_wake_packet(kind, credential);
        compare(search == reference_packet(CHIAKI_DISCOVERY_CMD_SRCH, kind, 0) + '\0', "search packet");
        compare(wake == reference_packet(CHIAKI_DISCOVERY_CMD_WAKEUP, kind, credential) + '\0', "wake packet");
    }

    // Replies as a console writes them, with fields present or absent at random.
    int both_read = 0;
    int both_refuse = 0;
    int only_ours = 0;
    int only_reference = 0;
    for (int round = 0; round < 20000; ++round) {
        const bool ps5 = random.below(2) != 0;
        const char* line_end = random.below(4) == 0 ? "\r\n" : "\n";
        const char* gap = random.below(2) != 0 ? " " : "";
        static const int statuses[] = {200, 620, 200, 620, 404, 500};
        std::string text = "HTTP/1.1 " + std::to_string(statuses[random.below(6)]) + " Ok" + line_end;
        const auto field = [&](const char* name, const std::string& value) {
            if (random.below(8) != 0) text += std::string(name) + ":" + gap + value + line_end;
        };
        field("host-id", "AABBCCDDEE" + std::to_string(10 + random.below(90)));
        field("host-type", ps5 ? "PS5" : "PS4");
        field("host-name", random_word(random, 20));
        field("host-request-port", std::to_string(random.below(70000)));
        field("device-discovery-protocol-version", ps5 ? "00030010" : "00020020");
        field("system-version", "0" + std::to_string(7000000 + random.below(3000000)));
        field("running-app-titleid", "CUSA" + std::to_string(10000 + random.below(89999)));
        field("running-app-name", random_word(random, 30));

        // Sometimes the text is damaged: one character replaced, or the end cut off.
        const std::uint32_t damage = random.below(6);
        if (damage == 0 && !text.empty()) {
            text[random.below(static_cast<std::uint32_t>(text.size()))] = static_cast<char>(0x20 + random.below(0x5f));
        } else if (damage == 1) {
            text.resize(random.below(static_cast<std::uint32_t>(text.size()) + 1));
        }

        const auto ours = proto::parse_discovery_reply(text);
        const ReferenceReply reference = reference_parse(text);
        ++outcome.compared;
        if (ours && reference.ok) {
            const std::string* fields[7] = {&ours->host_id,   &ours->host_name,        &ours->host_type,
                                            &ours->system_version, &ours->protocol_version, &ours->running_app_title_id,
                                            &ours->running_app_name};
            bool equal = state_number(ours->state) == reference.state;
            for (int i = 0; i < 7; ++i) equal = equal && *fields[i] == reference.fields[i];
            // The reference keeps the low sixteen bits of any number; we keep only a valid port.
            if (reference.port == ours->request_port || ours->request_port == 0) {
                // alike, or ours refused a number that is not a port
            } else {
                equal = false;
            }
            if (equal) ++both_read;
            else if (++outcome.mismatches <= 5) std::fprintf(stderr, "    MISMATCH discovery reply fields\n");
        } else if (!ours && !reference.ok) {
            ++both_refuse;
        } else if (ours) {
            ++only_ours;  // we read what the reference refuses: an empty value, or a status line with nothing after it
        } else {
            ++only_reference;
        }
    }
    std::fprintf(stderr,
                 "  discovery: %d comparisons, %d mismatches; replies: %d read alike, %d refused by both,\n"
                 "             %d read by us alone, %d read by the reference alone\n",
                 outcome.compared, outcome.mismatches, both_read, both_refuse, only_ours, only_reference);
    return outcome;
}

}  // namespace psvp::oracle
