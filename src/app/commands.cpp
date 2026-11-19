#include "app/commands.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

#include "app/account_id.h"
#include "app/chiaki_import.h"
#include "app/console_store.h"
#include "app/json.h"
#include "app/paths.h"
#include "net/socket.h"
#include "proto/discovery.h"
#include "proto/hex.h"
#include "proto/regist.h"
#include "proto/session.h"
#include "proto/transport.h"
#include "psvp/version.h"

namespace psvp::app {
namespace {

using namespace std::chrono_literals;
using net::Clock;
using proto::ConsoleKind;

constexpr int kDone = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 64;

// The local ports discovery prefers, as other clients do; any free port if all are taken.
constexpr std::uint16_t kLocalPortFirst = 9303;
constexpr std::uint16_t kLocalPortLast = 9319;

constexpr auto kRegistSearchWait = 3000ms;
constexpr auto kRegistPause = 100ms;  // between the console's answer and the connection
constexpr auto kRegistStepWait = 3000ms;
constexpr std::size_t kMaxResponseHead = 1500;

template <class... Args>
void say(std::format_string<Args...> format, Args&&... args) {
    std::fputs((std::format(format, std::forward<Args>(args)...) + "\n").c_str(), stdout);
}

template <class... Args>
int fail(std::format_string<Args...> format, Args&&... args) {
    std::fputs((std::format(format, std::forward<Args>(args)...) + "\n").c_str(), stderr);
    return kFailed;
}

template <class... Args>
int usage(std::format_string<Args...> format, Args&&... args) {
    std::fputs((std::format(format, std::forward<Args>(args)...) + "\n").c_str(), stderr);
    std::fputs(std::format("See '{} --help'.\n", PSVP_BINARY).c_str(), stderr);
    return kUsage;
}

// Options of one command: "--name value" pairs, flags, and plain words.
struct Parsed {
    std::vector<std::pair<std::string_view, std::string_view>> options;
    std::vector<std::string_view> flags;
    std::vector<std::string_view> words;
    std::string error;

    [[nodiscard]] std::optional<std::string_view> option(std::string_view name) const {
        for (const auto& [key, value] : options) {
            if (key == name) return value;
        }
        return std::nullopt;
    }
    [[nodiscard]] bool flag(std::string_view name) const { return std::ranges::find(flags, name) != flags.end(); }
};

Parsed parse(std::span<const std::string_view> args, std::initializer_list<std::string_view> value_options,
             std::initializer_list<std::string_view> flag_options, std::size_t max_words) {
    Parsed parsed;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (std::ranges::find(value_options, arg) != value_options.end()) {
            if (i + 1 >= args.size()) {
                parsed.error = std::format("{} needs a value", arg);
                return parsed;
            }
            parsed.options.emplace_back(arg, args[++i]);
        } else if (std::ranges::find(flag_options, arg) != flag_options.end()) {
            parsed.flags.push_back(arg);
        } else if (arg.starts_with("-")) {
            parsed.error = std::format("unknown option '{}'", arg);
            return parsed;
        } else {
            parsed.words.push_back(arg);
        }
    }
    if (parsed.words.size() > max_words) parsed.error = std::format("unexpected '{}'", parsed.words[max_words]);
    return parsed;
}

struct Context {
    std::filesystem::path store_file;
    ConsoleStore store;
};

std::expected<Context, std::string> load_context() {
    const auto dirs = resolve_dirs(process_env());
    if (!dirs) return std::unexpected(dirs.error());
    Context context;
    context.store_file = store_path(dirs->config);
    auto store = load_store(context.store_file);
    if (!store) return std::unexpected(store.error());
    context.store = std::move(*store);
    return context;
}

std::string mac_text(const std::array<std::uint8_t, 6>& mac) {
    return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool same_host(const proto::RegisteredConsole& console, const proto::DiscoveryReply& reply) {
    const std::string mac = proto::hex_encode(console.mac);
    return mac.size() == reply.host_id.size() &&
           std::equal(mac.begin(), mac.end(), reply.host_id.begin(),
                      [](char a, char b) { return a == (b >= 'A' && b <= 'F' ? static_cast<char>(b - 'A' + 'a') : b); });
}

struct Found {
    net::Address from;
    proto::DiscoveryReply reply;
};

// Asks every target, for both console families, and collects the answers until
// `wait` is over. With `first_only` it returns as soon as one console answered.
std::expected<std::vector<Found>, std::string> search(const std::vector<std::uint32_t>& targets,
                                                      std::chrono::milliseconds wait, bool first_only) {
    auto socket = net::UdpSocket::open(net::kAnyIp, kLocalPortFirst, kLocalPortLast, true);
    if (!socket) socket = net::UdpSocket::open(net::kAnyIp, 0, 0, true);
    if (!socket) return std::unexpected(socket.error());

    std::vector<Found> found;
    std::string last_send_error;
    const auto send_round = [&]() -> bool {
        bool sent = false;
        for (const std::uint32_t target : targets) {
            for (const ConsoleKind kind : {ConsoleKind::ps5, ConsoleKind::ps4}) {
                const std::string packet = proto::discovery_search_packet(kind);
                const auto result = socket->send_to(net::Address{target, proto::discovery_port(kind)}, net::bytes_of(packet));
                if (result) sent = true;
                else last_send_error = result.error();
            }
        }
        return sent;
    };

    const auto start = Clock::now();
    const auto deadline = start + wait;
    const auto second_round = start + wait / 2;  // a lost datagram should not hide a console
    bool resent = false;
    if (!send_round()) return std::unexpected(last_send_error);
    while (Clock::now() < deadline) {
        const auto until = resent ? deadline : std::min(second_round, deadline);
        const auto datagram = socket->receive(until);
        if (!datagram) return std::unexpected(datagram.error());
        if (!*datagram) {
            if (!resent) {
                resent = true;
                send_round();
            }
            continue;
        }
        const std::string_view text(reinterpret_cast<const char*>((*datagram)->data.data()), (*datagram)->data.size());
        const auto reply = proto::parse_discovery_reply(text);
        if (!reply) continue;
        const auto same = std::ranges::find_if(found, [&](const Found& f) {
            return f.from.ip == (*datagram)->from.ip && f.reply.kind == reply->kind;
        });
        if (same != found.end()) same->reply = *reply;
        else found.push_back(Found{(*datagram)->from, *reply});
        if (first_only) break;
    }
    return found;
}

std::vector<std::uint32_t> broadcast_targets() {
    std::vector<std::uint32_t> targets{net::kBroadcastIp};
    for (const std::uint32_t address : net::interface_broadcasts()) targets.push_back(address);
    return targets;
}

// Every other address of the small networks this machine is in. A firewall on
// this machine drops a console's answer to a broadcast, because it comes from an
// address nothing was sent to; an answer to a direct question gets through.
constexpr std::uint32_t kLargestNetworkToAsk = 1022;  // a /22

std::vector<std::uint32_t> neighbor_addresses() {
    std::vector<std::uint32_t> targets;
    for (const net::LocalNetwork& network : net::local_networks()) {
        if (network.host_count() == 0 || network.host_count() > kLargestNetworkToAsk) continue;
        for (std::uint32_t host = network.first_host(); host <= network.last_host(); ++host) {
            if (host != network.address) targets.push_back(host);
        }
    }
    return targets;
}

std::optional<std::chrono::milliseconds> parse_seconds(std::string_view text, int max_seconds) {
    int seconds = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds < 1 || seconds > max_seconds) return std::nullopt;
    return std::chrono::seconds(seconds);
}

int command_discover(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--host", "--wait"}, {"--json"}, 0);
    if (!parsed.error.empty()) return usage("discover: {}", parsed.error);
    std::chrono::milliseconds wait = 2000ms;
    if (const auto text = parsed.option("--wait")) {
        const auto seconds = parse_seconds(*text, 30);
        if (!seconds) return usage("discover: --wait takes a number of seconds from 1 to 30");
        wait = *seconds;
    }
    std::vector<std::uint32_t> targets = broadcast_targets();
    if (const auto host = parsed.option("--host")) {
        const auto address = net::resolve_ipv4(std::string(*host));
        if (!address) return fail("discover: {}", address.error());
        targets = {*address};
    }
    auto context = load_context();
    if (!context) return fail("discover: {}", context.error());

    auto found = search(targets, wait, false);
    if (!found) return fail("discover: {}", found.error());
    if (found->empty() && !parsed.option("--host")) {
        // Nobody answered the broadcast: ask the neighbors one by one.
        if (const auto neighbors = neighbor_addresses(); !neighbors.empty()) {
            found = search(neighbors, wait, false);
            if (!found) return fail("discover: {}", found.error());
        }
    }

    bool store_changed = false;
    Json::Array list;
    for (const Found& console : *found) {
        StoredConsole* registered = nullptr;
        for (StoredConsole& stored : context->store.consoles) {
            if (same_host(stored.registration, console.reply)) registered = &stored;
        }
        const std::string address = net::ip_to_string(console.from.ip);
        if (registered != nullptr && registered->last_address != address) {
            registered->last_address = address;
            store_changed = true;
        }
        if (parsed.flag("--json")) {
            list.push_back(Json(Json::Object{
                {"name", Json(console.reply.host_name)},
                {"kind", Json(proto::to_string(console.reply.kind))},
                {"state", Json(proto::to_string(console.reply.state))},
                {"address", Json(address)},
                {"host_id", Json(console.reply.host_id)},
                {"system_version", Json(console.reply.system_version)},
                {"running_app", Json(console.reply.running_app_name)},
                {"registered", Json(registered != nullptr)},
                {"supported", Json(console.reply.supported())},
            }));
        } else {
            say("{}  {}  {}  {}  {}{}{}", console.reply.host_name.empty() ? "(no name)" : console.reply.host_name,
                proto::to_string(console.reply.kind), proto::to_string(console.reply.state), address, console.reply.host_id,
                registered != nullptr ? "  registered" : "",
                console.reply.supported() ? "" : "  (system software too old for this program)");
        }
    }
    if (store_changed) {
        if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("discover: {}", saved.error());
    }
    if (parsed.flag("--json")) std::fputs(Json(std::move(list)).dump().c_str(), stdout);
    if (found->empty()) {
        if (parsed.option("--host")) return fail("No console answered at {}.", *parsed.option("--host"));
        return fail("No console answered. It must be on or in rest mode, on the same network as this computer;\n"
                    "if it is, try its address: {} discover --host ADDRESS", PSVP_BINARY);
    }
    return kDone;
}

int command_register(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--host", "--pin", "--account-id"}, {}, 0);
    if (!parsed.error.empty()) return usage("register: {}", parsed.error);
    const auto host = parsed.option("--host");
    const auto pin_text = parsed.option("--pin");
    if (!host || !pin_text) return usage("register: --host ADDRESS and --pin NUMBER are needed");

    std::uint32_t pin = 0;
    {
        std::string digits;
        for (const char c : *pin_text) {
            if (c != ' ' && c != '-') digits.push_back(c);
        }
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), pin);
        if (digits.size() != 8 || error != std::errc{} || end != digits.data() + digits.size()) {
            return usage("register: the PIN is the eight digits the console shows");
        }
    }

    auto context = load_context();
    if (!context) return fail("register: {}", context.error());
    std::optional<proto::AccountId> account = context->store.account_id;
    if (const auto text = parsed.option("--account-id")) {
        account = parse_account_id(*text);
        if (!account) return usage("register: '{}' is not an account ID (a number, or twelve base64 characters)", *text);
    }
    if (!account) {
        return fail("register: the account ID of your PSN account is needed once: --account-id ID");
    }

    const auto address = net::resolve_ipv4(std::string(*host));
    if (!address) return fail("register: {}", address.error());

    // What is at this address, and is it awake?
    const auto found = search({*address}, 3000ms, true);
    if (!found) return fail("register: {}", found.error());
    if (found->empty()) return fail("No console answered at {}. Check the address and that the console is on.", *host);
    const proto::DiscoveryReply& reply = found->front().reply;
    const ConsoleKind kind = reply.kind;
    if (!reply.supported()) {
        return fail("{} runs system software {} that is too old for this program; update the console.",
                    reply.host_name, reply.system_version);
    }
    if (reply.state != proto::ConsoleState::ready) {
        return fail("{} is in rest mode. Turn it on and open the screen that shows the PIN, then try again.", reply.host_name);
    }

    // Is it taking registrations? It is while it shows the PIN.
    auto probe = net::UdpSocket::open(net::kAnyIp, 0, 0, false);
    if (!probe) return fail("register: {}", probe.error());
    const net::Address console_address{*address, proto::kSessionPort};
    if (const auto sent = probe->send_to(console_address, net::bytes_of(proto::regist_search_packet(kind))); !sent) {
        return fail("register: {}", sent.error());
    }
    bool answered = false;
    const auto search_deadline = Clock::now() + kRegistSearchWait;
    while (!answered) {
        const auto datagram = probe->receive(search_deadline);
        if (!datagram) return fail("register: {}", datagram.error());
        if (!*datagram) break;
        answered = proto::is_regist_search_reply(kind, (*datagram)->data);
    }
    if (!answered) {
        return fail("{} is not taking registrations. On the console, open the screen that shows the PIN\n"
                    "({}), then try again.",
                    reply.host_name,
                    kind == ConsoleKind::ps5 ? "Settings > System > Remote Play > Link Device"
                                             : "Settings > Remote Play Connection Settings > Add Device");
    }
    std::this_thread::sleep_for(kRegistPause);

    const proto::RegistRequest request = proto::build_regist_request(kind, *account, pin, proto::random_key());
    auto stream = net::TcpStream::connect(console_address, Clock::now() + kRegistStepWait);
    if (!stream) return fail("register: {}", stream.error());
    const auto step_deadline = Clock::now() + kRegistStepWait;
    if (const auto sent = stream->send_all(net::bytes_of(request.head), step_deadline); !sent) return fail("register: {}", sent.error());
    if (const auto sent = stream->send_all(request.payload, step_deadline); !sent) return fail("register: {}", sent.error());

    auto head = net::read_text_head(*stream, Clock::now() + kRegistStepWait, kMaxResponseHead);
    if (!head) return fail("register: the console's answer: {}", head.error());
    const auto body_size = proto::parse_regist_response_head(head->head);
    if (!body_size) {
        if (body_size.error().failure == proto::RegistFailure::refused) {
            return fail("{}: {}.\nCheck the PIN (the console shows a new one each time) and that the account ID is the one\n"
                        "of the account signed in on the console.",
                        reply.host_name, proto::describe(body_size.error()));
        }
        return fail("register: {}", proto::describe(body_size.error()));
    }
    std::vector<std::uint8_t> body = std::move(head->rest);
    const auto body_deadline = Clock::now() + kRegistStepWait;
    while (body.size() < *body_size) {
        std::uint8_t chunk[1024];
        const auto got = stream->receive(std::span(chunk).first(std::min(sizeof(chunk), *body_size - body.size())), body_deadline);
        if (!got) return fail("register: {}", got.error());
        if (*got == 0) return fail("register: the console closed the connection in the middle of its answer");
        body.insert(body.end(), chunk, chunk + *got);
    }
    body.resize(*body_size);
    auto registered = proto::parse_regist_response_body(request.cipher, body);
    if (!registered) return fail("register: {}", proto::describe(registered.error()));
    if (registered->nickname.empty()) registered->nickname = reply.host_name;

    context->store.account_id = account;
    const std::string nickname = registered->nickname;
    context->store.put(StoredConsole{std::move(*registered), net::ip_to_string(*address)});
    if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("register: {}", saved.error());
    say("Registered with {} ({}).", nickname, proto::to_string(kind));
    return kDone;
}

int command_wake(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--host", "--wait"}, {"--no-wait"}, 1);
    if (!parsed.error.empty()) return usage("wake: {}", parsed.error);
    if (parsed.words.empty()) return usage("wake: which console? Give its name or hardware address");
    std::chrono::milliseconds wait = 45s;
    if (const auto text = parsed.option("--wait")) {
        const auto seconds = parse_seconds(*text, 300);
        if (!seconds) return usage("wake: --wait takes a number of seconds from 1 to 300");
        wait = *seconds;
    }
    auto context = load_context();
    if (!context) return fail("wake: {}", context.error());
    StoredConsole* console = context->store.find(parsed.words.front());
    if (console == nullptr) {
        return fail("No registered console is called '{}'. '{} consoles' lists them.", parsed.words.front(), PSVP_BINARY);
    }
    const proto::RegisteredConsole& registration = console->registration;
    const auto credential = proto::wake_credential(registration.regist_key);
    if (!credential) return fail("wake: the registration with {} is damaged; register again", registration.nickname);

    // To its last known address when there is one, and to everyone on the network otherwise.
    std::vector<std::uint32_t> targets;
    if (const auto host = parsed.option("--host")) {
        const auto address = net::resolve_ipv4(std::string(*host));
        if (!address) return fail("wake: {}", address.error());
        targets = {*address};
    } else if (const auto last = net::parse_ipv4(console->last_address)) {
        targets = {*last};
    } else {
        targets = broadcast_targets();
    }

    auto socket = net::UdpSocket::open(net::kAnyIp, kLocalPortFirst, kLocalPortLast, true);
    if (!socket) socket = net::UdpSocket::open(net::kAnyIp, 0, 0, true);
    if (!socket) return fail("wake: {}", socket.error());
    const std::string packet = proto::discovery_wake_packet(registration.kind, *credential);
    const auto send_wake = [&]() {
        bool sent = false;
        for (const std::uint32_t target : targets) {
            if (socket->send_to(net::Address{target, proto::discovery_port(registration.kind)}, net::bytes_of(packet))) sent = true;
        }
        return sent;
    };
    if (!send_wake()) return fail("wake: the wake-up could not be sent to {}", registration.nickname);
    if (parsed.flag("--no-wait")) {
        say("Wake-up sent to {}.", registration.nickname);
        return kDone;
    }

    // A console takes a while to come up; the wake-up is repeated until it answers as ready.
    const auto deadline = Clock::now() + wait;
    while (Clock::now() < deadline) {
        const auto found = search(targets, 1000ms, false);
        if (!found) return fail("wake: {}", found.error());
        for (const Found& candidate : *found) {
            if (!same_host(registration, candidate.reply) || candidate.reply.state != proto::ConsoleState::ready) continue;
            console->last_address = net::ip_to_string(candidate.from.ip);
            const std::string nickname = registration.nickname;
            if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("wake: {}", saved.error());
            say("{} is on.", nickname);
            return kDone;
        }
        send_wake();
    }
    return fail("{} did not come on within {} seconds. It wakes only if rest mode keeps it connected to the\n"
                "network (on the console: the rest mode settings), and only for a client it is registered with.",
                registration.nickname, std::chrono::duration_cast<std::chrono::seconds>(wait).count());
}

// The transport's opening handshake with one of the console's two transport ports.
std::expected<proto::transport::Endpoint, std::string> transport_handshake(const net::Address& console) {
    auto socket = net::UdpSocket::open(net::kAnyIp, 0, 0, false);
    if (!socket) return std::unexpected(socket.error());
    const proto::Key16 random = proto::random_key();
    const std::uint32_t tag = (std::uint32_t{random[0]} << 24) | (std::uint32_t{random[1]} << 16) | (std::uint32_t{random[2]} << 8) | 1U;
    proto::transport::ClientHandshake handshake(tag);
    auto out = handshake.start(Clock::now());
    while (true) {
        if (out) {
            if (const auto sent = socket->send_to(console, *out); !sent) return std::unexpected(sent.error());
        }
        if (handshake.state() == proto::transport::ClientHandshake::State::connected) return handshake.console();
        if (handshake.state() == proto::transport::ClientHandshake::State::failed) return std::unexpected(handshake.failure());
        const auto datagram = socket->receive(handshake.deadline());
        if (!datagram) return std::unexpected(datagram.error());
        if (*datagram) {
            out = (*datagram)->from == console ? handshake.on_datagram((*datagram)->data, Clock::now()) : std::nullopt;
        } else {
            out = handshake.on_timeout(Clock::now());
        }
    }
}

// Opens a session as far as this version goes: the request, the control channel,
// the session ID and the heartbeats, then the transport's handshake. Nothing is streamed.
int command_session(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--host", "--seconds", "--passcode"}, {"--rest"}, 1);
    if (!parsed.error.empty()) return usage("session: {}", parsed.error);
    if (parsed.words.empty()) return usage("session: which console? Give its name or hardware address");
    std::chrono::milliseconds hold = 3s;
    if (const auto text = parsed.option("--seconds")) {
        const auto seconds = parse_seconds(*text, 600);
        if (!seconds) return usage("session: --seconds takes a number from 1 to 600");
        hold = *seconds;
    }
    auto context = load_context();
    if (!context) return fail("session: {}", context.error());
    const StoredConsole* console = context->store.find(parsed.words.front());
    if (console == nullptr) {
        return fail("No registered console is called '{}'. '{} consoles' lists them.", parsed.words.front(), PSVP_BINARY);
    }
    const proto::RegisteredConsole& registration = console->registration;
    const std::string host(parsed.option("--host").value_or(console->last_address));
    if (host.empty()) return fail("The address of {} is not known yet: '{} discover' finds it, or give --host ADDRESS.", registration.nickname, PSVP_BINARY);
    const auto ip = net::resolve_ipv4(host);
    if (!ip) return fail("session: {}", ip.error());
    const net::Address address{*ip, proto::kSessionPort};
    const std::string address_text = net::ip_to_string(*ip);
    constexpr auto kStepWait = 5000ms;

    // The request that yields the console's nonce.
    proto::Key16 nonce{};
    {
        auto stream = net::TcpStream::connect(address, Clock::now() + kStepWait);
        if (!stream) return fail("session: {}", stream.error());
        const std::string request = proto::build_session_request(registration.kind, address_text, registration.regist_key);
        if (const auto sent = stream->send_all(net::bytes_of(request), Clock::now() + kStepWait); !sent) return fail("session: {}", sent.error());
        const auto head = net::read_text_head(*stream, Clock::now() + kStepWait, kMaxResponseHead);
        if (!head) return fail("session: {}", head.error());
        const auto parsed_nonce = proto::parse_session_response(head->head);
        if (!parsed_nonce) return fail("{}: {}.", registration.nickname, proto::describe(parsed_nonce.error()));
        nonce = *parsed_nonce;
    }
    const proto::RpCipher cipher = proto::make_session_cipher(registration.kind, nonce, registration.key);

    // The control channel. One more try if the console does not answer the first.
    const proto::CtrlRequest request = proto::build_ctrl_request(cipher, address_text, registration.regist_key,
                                                                 proto::make_device_id(proto::random_key()), proto::Codec::h264);
    net::TcpStream control;
    std::optional<net::TextHead> head;
    std::string last_error;
    for (int attempt = 0; attempt < 2 && !head; ++attempt) {
        auto stream = net::TcpStream::connect(address, Clock::now() + kStepWait);
        if (!stream) return fail("session: {}", stream.error());
        if (const auto sent = stream->send_all(net::bytes_of(request.text), Clock::now() + kStepWait); !sent) return fail("session: {}", sent.error());
        auto answer = net::read_text_head(*stream, Clock::now() + kStepWait, kMaxResponseHead);
        if (answer) {
            head = std::move(*answer);
            control = std::move(*stream);
        } else {
            last_error = answer.error();
        }
    }
    if (!head) return fail("session: the control channel: {}", last_error);
    const auto accepted = proto::parse_ctrl_response(cipher, head->head);
    if (!accepted) return fail("{}: {}.", registration.nickname, proto::describe(accepted.error()));
    say("{} accepted the session ({}).", registration.nickname, proto::to_string(accepted->model));

    proto::CtrlChannel channel(cipher, request.next_local_number, accepted->next_remote_number);
    const auto send = [&](std::uint16_t type, std::span<const std::uint8_t> payload = {}) {
        const auto bytes = channel.encode(type, payload);
        return bytes && control.send_all(*bytes, Clock::now() + kStepWait).has_value();
    };
    if (!channel.feed(head->rest)) return fail("session: the console's messages could not be read");

    bool have_session_id = false;
    int heartbeats = 0;
    const auto deadline = Clock::now() + hold;
    while (Clock::now() < deadline) {
        while (const auto message = channel.next()) {
            using namespace proto::ctrl_type;
            if (message->type == kSessionId) {
                const auto id = proto::parse_session_id(message->payload);
                have_session_id = true;
                if (id) say("Session ID received ({} characters).", id->size());
                else say("The console sent a session ID in a form this version does not know.");
            } else if (message->type == kHeartbeatRequest) {
                if (!send(kHeartbeatReply)) return fail("session: the connection to the console broke");
                ++heartbeats;
            } else if (message->type == kLoginPinRequest) {
                const auto passcode = parsed.option("--passcode");
                if (!passcode) return fail("{} asks for its login passcode: add --passcode DIGITS.", registration.nickname);
                if (!send(kLoginPinReply, net::bytes_of(*passcode))) return fail("session: the connection to the console broke");
            } else if (message->type == kLoginResult) {
                if (message->payload.empty() || message->payload[0] != 0) return fail("{} did not accept the passcode.", registration.nickname);
                say("Passcode accepted.");
            }
        }
        std::uint8_t chunk[1024];
        const auto got = control.receive(chunk, std::min(deadline, Clock::now() + 200ms));
        if (!got) {
            if (got.error().ends_with("timed out")) continue;
            return fail("session: {}", got.error());
        }
        if (*got == 0) return fail("{} closed the control channel.", registration.nickname);
        if (!channel.feed(std::span(chunk).first(*got))) return fail("session: the console's messages could not be read");
    }
    if (!have_session_id) return fail("{} never named the session.", registration.nickname);

    // The transport the stream will use, as far as its opening handshake.
    const auto transport = transport_handshake(net::Address{*ip, proto::transport::kProbePort});
    if (!transport) return fail("{}: the stream transport: {}.", registration.nickname, transport.error());
    say("Stream transport opened (handshake done, {} streams each way).", transport->outbound_streams);

    if (parsed.flag("--rest")) {
        if (!send(proto::ctrl_type::kGoToRest)) return fail("session: the connection to the console broke");
        say("Asked {} to go to rest mode.", registration.nickname);
    }
    say("Control channel held for {} seconds; heartbeats answered: {}.",
        std::chrono::duration_cast<std::chrono::seconds>(hold).count(), heartbeats);
    return kDone;
}

int command_consoles(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {}, {"--json"}, 0);
    if (!parsed.error.empty()) return usage("consoles: {}", parsed.error);
    const auto context = load_context();
    if (!context) return fail("consoles: {}", context.error());
    if (parsed.flag("--json")) {
        Json::Array list;
        for (const StoredConsole& console : context->store.consoles) {
            list.push_back(Json(Json::Object{
                {"name", Json(console.registration.nickname)},
                {"kind", Json(proto::to_string(console.registration.kind))},
                {"mac", Json(mac_text(console.registration.mac))},
                {"last_address", Json(console.last_address)},
            }));
        }
        std::fputs(Json(std::move(list)).dump().c_str(), stdout);
        return kDone;
    }
    if (context->store.consoles.empty()) {
        say("No console is registered yet. '{} register --host ADDRESS --pin NUMBER' registers with one.", PSVP_BINARY);
        return kDone;
    }
    for (const StoredConsole& console : context->store.consoles) {
        say("{}  {}  {}  {}", console.registration.nickname, proto::to_string(console.registration.kind),
            mac_text(console.registration.mac), console.last_address.empty() ? "(address not known)" : console.last_address);
    }
    return kDone;
}

int command_forget(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {}, {}, 1);
    if (!parsed.error.empty()) return usage("forget: {}", parsed.error);
    if (parsed.words.empty()) return usage("forget: which console? Give its name or hardware address");
    auto context = load_context();
    if (!context) return fail("forget: {}", context.error());
    const StoredConsole* console = context->store.find(parsed.words.front());
    if (console == nullptr) return fail("No registered console is called '{}'.", parsed.words.front());
    const std::string nickname = console->registration.nickname;
    const auto mac = console->registration.mac;
    std::erase_if(context->store.consoles, [&](const StoredConsole& c) { return c.registration.mac == mac; });
    if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("forget: {}", saved.error());
    say("Forgot {}. The console still lists this client until it is removed there too.", nickname);
    return kDone;
}

int command_import(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {}, {}, 1);
    if (!parsed.error.empty()) return usage("import: {}", parsed.error);
    std::vector<std::filesystem::path> files;
    if (!parsed.words.empty()) {
        files.emplace_back(parsed.words.front());
    } else {
        std::error_code ignored;
        for (const auto& candidate : chiaki_config_candidates(process_env())) {
            if (std::filesystem::exists(candidate, ignored)) files.push_back(candidate);
        }
        if (files.empty()) return fail("No Chiaki or chiaki-ng settings were found on this computer.");
    }
    auto context = load_context();
    if (!context) return fail("import: {}", context.error());

    int added = 0;
    for (const auto& file : files) {
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return fail("import: cannot read {}", file.string());
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto config = parse_chiaki_config(text);
        if (!config) return fail("import: {}: {}", file.string(), config.error());
        say("{}:", file.string());
        if (config->account_id && !context->store.account_id) {
            context->store.account_id = config->account_id;
            say("  account ID taken over");
        }
        for (const proto::RegisteredConsole& console : config->consoles) {
            if (context->store.find_mac(console.mac) != nullptr) {
                say("  {} ({}): already registered here", console.nickname, proto::to_string(console.kind));
                continue;
            }
            say("  {} ({}): imported", console.nickname, proto::to_string(console.kind));
            context->store.put(StoredConsole{console, std::string()});
            ++added;
        }
        for (const std::string& reason : config->skipped) say("  {}: not imported", reason);
        if (config->consoles.empty() && config->skipped.empty()) say("  no registered console in it");
    }
    if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("import: {}", saved.error());
    say("{} console{} imported.", added, added == 1 ? "" : "s");
    return kDone;
}

int command_account(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {}, {}, 1);
    if (!parsed.error.empty()) return usage("account: {}", parsed.error);
    auto context = load_context();
    if (!context) return fail("account: {}", context.error());
    if (!parsed.words.empty()) {
        const auto id = parse_account_id(parsed.words.front());
        if (!id) return usage("account: '{}' is not an account ID (a number, or twelve base64 characters)", parsed.words.front());
        context->store.account_id = id;
        if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("account: {}", saved.error());
    }
    if (!context->store.account_id) {
        say("No account ID is set. '{} account ID' sets it.", PSVP_BINARY);
        return kDone;
    }
    say("{}  ({})", account_id_base64(*context->store.account_id), account_id_decimal(*context->store.account_id));
    return kDone;
}

}  // namespace

bool is_command(std::span<const std::string_view> args) { return !args.empty() && !args.front().starts_with("-"); }

int run_command(std::span<const std::string_view> args) {
    const std::string_view name = args.front();
    const auto rest = args.subspan(1);
    if (name == "discover") return command_discover(rest);
    if (name == "register") return command_register(rest);
    if (name == "wake") return command_wake(rest);
    if (name == "session") return command_session(rest);
    if (name == "consoles") return command_consoles(rest);
    if (name == "forget") return command_forget(rest);
    if (name == "import") return command_import(rest);
    if (name == "account") return command_account(rest);
    return usage("unknown command '{}'", name);
}

std::string commands_help() {
    return std::format(
        "Commands:\n"
        "  {0} discover [--host ADDRESS] [--wait SECONDS] [--json]\n"
        "      list the consoles on the network, or ask one address\n"
        "  {0} register --host ADDRESS --pin NUMBER [--account-id ID]\n"
        "      register with a console that is showing its eight-digit PIN\n"
        "  {0} wake CONSOLE [--host ADDRESS] [--wait SECONDS] [--no-wait]\n"
        "      wake a registered console from rest mode\n"
        "  {0} session CONSOLE [--host ADDRESS] [--seconds N] [--passcode DIGITS] [--rest]\n"
        "      open a session's control channel with a console and close it again\n"
        "      (a connection check: nothing is streamed yet); --rest then sends it to rest mode\n"
        "  {0} consoles [--json]\n"
        "      list the registered consoles\n"
        "  {0} forget CONSOLE\n"
        "      remove a console from the list\n"
        "  {0} import [FILE]\n"
        "      take over the consoles registered in Chiaki or chiaki-ng\n"
        "  {0} account [ID]\n"
        "      show or set the PSN account ID used for registering\n",
        PSVP_BINARY);
}

}  // namespace psvp::app
