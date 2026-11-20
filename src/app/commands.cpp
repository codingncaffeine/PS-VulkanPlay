#include "app/commands.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <thread>
#include <vector>

#include "app/account_id.h"
#include "app/chiaki_import.h"
#include "app/connection_view.h"
#include "app/console_store.h"
#include "app/controllers.h"
#include "app/decoding.h"
#include "app/json.h"
#include "app/log.h"
#include "app/paths.h"
#include "app/session_run.h"
#include "net/socket.h"
#include "proto/base64.h"
#include "proto/discovery.h"
#include "proto/hex.h"
#include "proto/probe.h"
#include "proto/regist.h"
#include "proto/key_exchange.h"
#include "proto/rp_cipher.h"
#include "proto/session.h"
#include "proto/stream_message.h"
#include "proto/stream_setup.h"
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
    const std::string text = std::format(format, std::forward<Args>(args)...);
    std::fputs((text + "\n").c_str(), stdout);
    log::record(text);
}

template <class... Args>
int fail(std::format_string<Args...> format, Args&&... args) {
    const std::string text = std::format(format, std::forward<Args>(args)...);
    std::fputs((text + "\n").c_str(), stderr);
    log::record(text);
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

// Wakes a registered console and, unless `no_wait`, repeats the wake-up until it answers as ready.
int wake_console(Context& context, StoredConsole& console, std::optional<std::string_view> host, std::chrono::milliseconds wait, bool no_wait) {
    const proto::RegisteredConsole& registration = console.registration;
    const auto credential = proto::wake_credential(registration.regist_key);
    if (!credential) return fail("wake: the registration with {} is damaged; register again", registration.nickname);

    // To its last known address when there is one, and to everyone on the network otherwise.
    std::vector<std::uint32_t> targets;
    if (host) {
        const auto address = net::resolve_ipv4(std::string(*host));
        if (!address) return fail("wake: {}", address.error());
        targets = {*address};
    } else if (const auto last = net::parse_ipv4(console.last_address)) {
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
    if (no_wait) {
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
            console.last_address = net::ip_to_string(candidate.from.ip);
            const std::string nickname = registration.nickname;
            if (const auto saved = save_store(context.store_file, context.store); !saved) return fail("wake: {}", saved.error());
            say("{} is on.", nickname);
            return kDone;
        }
        send_wake();
    }
    return fail("{} did not come on within {} seconds. It wakes only if rest mode keeps it connected to the\n"
                "network (on the console: the rest mode settings), and only for a client it is registered with.",
                registration.nickname, std::chrono::duration_cast<std::chrono::seconds>(wait).count());
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
    return wake_console(*context, *console, parsed.option("--host"), wait, parsed.flag("--no-wait"));
}

// For 'play --wake': a console that rests is woken first. One that does not answer at all is sent
// the wake-up too: a console that is off the network's reach will not start a session either, and
// the wake-up's own message says what to look at.
int wake_if_resting(const SessionPlan& plan) {
    auto context = load_context();
    if (!context) return fail("play: {}", context.error());
    StoredConsole* console = context->store.find(plan.console);
    if (console == nullptr) return kDone;  // the session says so, in its own words
    std::vector<std::uint32_t> targets = broadcast_targets();
    if (const std::string host(plan.host.value_or(console->last_address)); !host.empty()) {
        const auto address = net::resolve_ipv4(host);
        if (!address) return fail("play: {}", address.error());
        targets = {*address};
    }
    const auto found = search(targets, 1500ms, false);
    if (!found) return fail("play: {}", found.error());
    for (const Found& candidate : *found) {
        if (!same_host(console->registration, candidate.reply)) continue;
        if (candidate.reply.state == proto::ConsoleState::ready) return kDone;
        say("{} is in rest mode: waking it.", console->registration.nickname);
        return wake_console(*context, *console, plan.host, 60s, false);
    }
    say("{} did not answer: sending it a wake-up.", console->registration.nickname);
    return wake_console(*context, *console, plan.host, 60s, false);
}

// The form of a payload without its content: its length, its first byte and
// which kinds of characters the rest holds. For a report, when the content is not to be shown.
std::string describe_shape(std::span<const std::uint8_t> payload) {
    if (payload.empty()) return "empty";
    int letters = 0;
    int digits = 0;
    int zeros = 0;
    std::string others;
    for (const std::uint8_t byte : payload.subspan(1)) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')) ++letters;
        else if (byte >= '0' && byte <= '9') ++digits;
        else if (byte == 0) ++zeros;
        else if (const std::string text = std::format("{:02x}", byte); others.find(text) == std::string::npos && others.size() < 60) others += text + " ";
    }
    if (!others.empty()) others.pop_back();
    return std::format("{} bytes, first {:#04x}, then {} letters, {} digits, {} zero bytes, other byte values: {}", payload.size(),
                       payload[0], letters, digits, zeros, others.empty() ? "none" : others);
}

// What the network probe found, or why it could not be run.
struct ProbeOutcome {
    proto::transport::Endpoint console;
    std::optional<proto::transport::ProbeResult> measured;
    std::string probe_failure;
    int pongs = 0;
};

std::uint32_t random_number() {
    const proto::Key16 bytes = proto::random_key();
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
}

// While a session's datagrams are waited for, its control channel wants its
// heartbeats answered: no wait is longer than this before `service` runs again.
constexpr auto kServiceEvery = 50ms;
// Returns false once the control channel has failed; the wait in progress then ends.
using Service = std::function<bool()>;

// A connection of the stream transport that has done its opening handshake.
struct Transport {
    net::UdpSocket socket;
    net::Address console;
    std::uint32_t tag = 0;
    proto::transport::Endpoint endpoint;
};

std::expected<Transport, std::string> open_transport(const net::Address& console, const Service& service) {
    namespace transport = proto::transport;
    auto socket = net::UdpSocket::open(net::kAnyIp, 0, 0, false);
    if (!socket) return std::unexpected(socket.error());
    const std::uint32_t tag = random_number() | 1U;
    transport::ClientHandshake handshake(tag);
    auto out = handshake.start(Clock::now());
    while (true) {
        if (out) {
            if (const auto sent = socket->send_to(console, *out); !sent) return std::unexpected(sent.error());
        }
        if (handshake.state() == transport::ClientHandshake::State::connected) break;
        if (handshake.state() == transport::ClientHandshake::State::failed) return std::unexpected(handshake.failure());
        if (!service()) return std::unexpected(std::string("the control channel failed"));
        const auto datagram = socket->receive(std::min(handshake.deadline(), Clock::now() + kServiceEvery));
        if (!datagram) return std::unexpected(datagram.error());
        if (*datagram) {
            out = (*datagram)->from == console ? handshake.on_datagram((*datagram)->data, Clock::now()) : std::nullopt;
        } else {
            out = Clock::now() >= handshake.deadline() ? handshake.on_timeout(Clock::now()) : std::nullopt;
        }
    }
    return Transport{std::move(*socket), console, tag, handshake.console()};
}

// The transport's opening handshake on the console's probe port, then the network probe.
std::expected<ProbeOutcome, std::string> probe_network(const net::Address& console, const Service& service) {
    namespace transport = proto::transport;
    auto link = open_transport(console, service);
    if (!link) return std::unexpected(link.error());

    ProbeOutcome outcome;
    outcome.console = link->endpoint;
    transport::ClientProbe probe(link->tag, link->endpoint, random_number);
    transport::Datagrams sending = probe.start(Clock::now());
    while (true) {
        for (const auto& datagram : sending) {
            // A size ping that the system refuses to send did not pass; the probe finds that out by its silence.
            (void)link->socket.send_to(console, datagram);
        }
        sending.clear();
        if (probe.state() != transport::ClientProbe::State::running) break;
        if (!service()) return std::unexpected(std::string("the control channel failed"));
        const auto datagram = link->socket.receive(std::min(probe.deadline(), Clock::now() + kServiceEvery));
        if (!datagram) return std::unexpected(datagram.error());
        if (*datagram) {
            if ((*datagram)->from == console) sending = probe.on_datagram((*datagram)->data, Clock::now());
        } else if (Clock::now() >= probe.deadline()) {
            sending = probe.on_timeout(Clock::now());
        }
    }
    outcome.pongs = probe.pongs();
    if (probe.state() == transport::ClientProbe::State::done) outcome.measured = probe.result();
    else outcome.probe_failure = probe.failure();
    return outcome;
}

// A session ID of the client's own making, in the form other clients fall back
// to: the seconds of a clock, then 48 random bytes in base64.
std::string invented_session_id() {
    std::array<std::uint8_t, 48> random{};
    for (std::size_t i = 0; i < random.size(); i += 16) {
        const proto::Key16 part = proto::random_key();
        std::copy(part.begin(), part.end(), random.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    return std::format("{}{}", seconds, proto::base64_encode(random));
}

struct VideoChoice {
    std::string_view name;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bitrate_kbps;
};
constexpr VideoChoice kVideoChoices[] = {{"360", 640, 360, 2000}, {"540", 960, 540, 6000}, {"720", 1280, 720, 10000}, {"1080", 1920, 1080, 15000}};

std::optional<std::uint32_t> parse_number(std::string_view text, std::uint32_t low, std::uint32_t high) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high) return std::nullopt;
    return value;
}

// Keeps what a stream delivered, for looking at afterwards: the picture as a decoder would be
// given it (each picture size's header, then its frames), the sound frames, and one line per
// frame and per packet next to them.
class StreamDump {
public:
    [[nodiscard]] static std::expected<StreamDump, std::string> open(const std::filesystem::path& folder) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error) return std::unexpected(std::format("{}: {}", folder.string(), error.message()));
        StreamDump dump;
        dump.folder_ = folder;
        const auto file = [&](std::ofstream& out, const char* name) {
            out.open(folder / name, std::ios::binary | std::ios::trunc);
            return out.is_open();
        };
        if (!file(dump.video_, "video.bin") || !file(dump.video_list_, "video.tsv") || !file(dump.sound_, "sound.bin") ||
            !file(dump.sound_list_, "sound.tsv") || !file(dump.packets_, "packets.tsv") || !file(dump.data_, "data.tsv")) {
            return std::unexpected(std::format("{}: cannot write there", folder.string()));
        }
        dump.video_list_ << "frame\tbytes\trepaired\tlost-before\tpicture-size\theader-bytes\n";
        dump.sound_list_ << "frame\tbytes\thaptics\tfrom-a-later-packet\tlost-before\n";
        dump.data_ << "ms\twhat\tchannel\tbytes\thex\n";
        dump.packets_ << "ms\tkind\tpacket\tframe\tunit\tunits\trepair-or-new\trepeated\tcodec\tpicture-size\tkey-position\tbytes\tword\tlonger-header\n";
        return dump;
    }

    // One line per picture or sound packet: its header's fields, nothing of its content.
    void packet(ConsoleKind kind, std::span<const std::uint8_t> datagram, std::chrono::microseconds since_start) {
        const auto packet = proto::transport::parse_stream_media(kind, datagram);
        if (!packet) return;
        packets_ << std::format("{:.3f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", static_cast<double>(since_start.count()) / 1000.0,
                                packet->video ? "picture" : packet->haptics ? "haptics" : "sound", packet->packet_index, packet->frame_index,
                                packet->unit_index, packet->units_total, packet->video ? packet->units_repair : packet->source_frames,
                                packet->repeated_frames, packet->codec, packet->adaptive_stream, packet->key_position, datagram.size(),
                                packet->video_word, packet->unit_info ? 1 : 0);
    }

    void take(proto::transport::MediaOutput media, const std::optional<proto::StreamInfo>& info) {
        for (const auto& frame : media.video) {
            // A decoder needs the header of a picture size before the first frame in it.
            std::size_t header_bytes = 0;
            if (frame.adaptive_stream != picture_size_ && info && frame.adaptive_stream < info->resolutions.size()) {
                const auto& header = info->resolutions[frame.adaptive_stream].video_header;
                video_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
                header_bytes = header.size();
                picture_size_ = frame.adaptive_stream;
            }
            video_.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
            video_list_ << std::format("{}\t{}\t{}\t{}\t{}\t{}\n", frame.index, frame.data.size(), frame.repaired ? 1 : 0, frame.lost_before,
                                       frame.adaptive_stream, header_bytes);
            video_bytes_ += header_bytes + frame.data.size();
        }
        for (const auto& frame : media.sound) {
            sound_.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
            sound_list_ << std::format("{}\t{}\t{}\t{}\t{}\n", frame.index, frame.data.size(), frame.haptics ? 1 : 0, frame.recovered ? 1 : 0,
                                       frame.lost_before);
            sound_bytes_ += frame.data.size();
        }
    }

    // What the console sent besides picture and sound, with its bytes.
    void take(const std::vector<proto::transport::ConsoleData>& data, std::chrono::microseconds since_start) {
        for (const auto& item : data) {
            data_ << std::format("{:.3f}\t{} {}\t{}\t{}\t{}\n", static_cast<double>(since_start.count()) / 1000.0,
                                 item.message ? "message type" : "data kind", item.kind, item.channel, item.bytes.size(),
                                 proto::hex_encode(item.bytes));
        }
    }

    // False when something could not be written.
    [[nodiscard]] bool close() {
        bool good = true;
        for (std::ofstream* file : {&video_, &video_list_, &sound_, &sound_list_, &packets_, &data_}) {
            file->flush();
            good = good && file->good();
            file->close();
        }
        return good;
    }
    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return folder_; }
    [[nodiscard]] std::uint64_t video_bytes() const noexcept { return video_bytes_; }
    [[nodiscard]] std::uint64_t sound_bytes() const noexcept { return sound_bytes_; }

private:
    std::filesystem::path folder_;
    std::ofstream video_;
    std::ofstream video_list_;
    std::ofstream sound_;
    std::ofstream sound_list_;
    std::ofstream packets_;
    std::ofstream data_;
    int picture_size_ = -1;
    std::uint64_t video_bytes_ = 0;
    std::uint64_t sound_bytes_ = 0;
};

// A button to press for a moment, so many milliseconds after the stream is set up.
struct Tap {
    proto::transport::Button button = proto::transport::Button::cross;
    std::chrono::milliseconds at{0};
};

// "right@1,left@2.5": names of buttons with the second they are pressed in. Nothing for a text that is not that.
std::optional<std::vector<Tap>> parse_taps(std::string_view text) {
    std::vector<Tap> taps;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        const std::size_t at = item.find('@');
        if (at == std::string_view::npos) return std::nullopt;
        const auto button = proto::transport::button_named(item.substr(0, at));
        // Seconds with up to three decimals.
        const std::string_view when = item.substr(at + 1);
        const std::size_t point = when.find('.');
        const std::string_view whole = when.substr(0, point);
        std::string decimals(point == std::string_view::npos ? std::string_view() : when.substr(point + 1));
        std::uint32_t seconds = 0;
        std::uint32_t thousandths = 0;
        if (!button || whole.empty() || decimals.size() > 3 || (point != std::string_view::npos && decimals.empty())) return std::nullopt;
        decimals.resize(3, '0');
        if (std::from_chars(whole.data(), whole.data() + whole.size(), seconds).ptr != whole.data() + whole.size() || seconds > 600 ||
            std::from_chars(decimals.data(), decimals.data() + 3, thousandths).ptr != decimals.data() + 3) {
            return std::nullopt;
        }
        taps.push_back(Tap{*button, std::chrono::milliseconds(seconds * 1000 + thousandths)});
    }
    if (taps.empty()) return std::nullopt;
    return taps;
}

template <typename Key> std::string counts_text(const std::map<Key, std::uint64_t>& counts, std::string_view label) {
    std::string text;
    for (const auto& [key, count] : counts) text += std::format("{}{} {} x{}", text.empty() ? "" : ", ", label, +key, count);
    return text;
}

// Opens a session and sets a stream up, as far as this version goes: the request,
// the control channel, the session ID, the network probe, then the stream
// connection with its keys and the stream information. The stream is held for a
// few seconds: its packets are checked, its picture frames and sound frames put
// together and counted, and kept in a folder on request. Nothing is decoded or shown.
// The options that 'session' and 'play' share, read into a plan. Returns what is wrong with them, if anything.
std::optional<std::string> read_plan(const Parsed& parsed, SessionPlan& plan) {
    if (parsed.words.empty()) return "which console? Give its name or hardware address";
    plan.console = std::string(parsed.words.front());
    if (const auto host = parsed.option("--host")) plan.host = std::string(*host);
    if (const auto passcode = parsed.option("--passcode")) plan.passcode = std::string(*passcode);
    plan.rest = parsed.flag("--rest");
    if (const auto text = parsed.option("--resolution")) {
        const auto choice = std::ranges::find(kVideoChoices, *text, &VideoChoice::name);
        if (choice == std::end(kVideoChoices)) return "--resolution is 360, 540, 720 or 1080";
        plan.video.width = choice->width;
        plan.video.height = choice->height;
        plan.video.bitrate_kbps = choice->bitrate_kbps;
    }
    if (const auto text = parsed.option("--fps")) {
        if (*text != "30" && *text != "60") return "--fps is 30 or 60";
        plan.video.max_fps = *text == "30" ? 30 : 60;
    }
    if (const auto text = parsed.option("--bitrate")) {
        const auto kbps = parse_number(*text, 500, 100000);
        if (!kbps) return "--bitrate is in kbit/s, from 500 to 100000";
        plan.video.bitrate_kbps = *kbps;
    }
    if (const auto text = parsed.option("--ack-copies")) {
        const auto copies = parse_number(*text, 0, 2);
        if (!copies && *text != "0") return "--ack-copies is 0, 1 or 2";
        plan.ack_copies = copies.value_or(0);
    }
    if (const auto text = parsed.option("--loss-ceiling")) {
        const auto percent = parse_number(*text, 0, 100);
        if (!percent) return "--loss-ceiling is a percent, from 0 to 100";
        plan.reported_loss_ceiling = *percent;
    }
    if (const auto text = parsed.option("--codec")) {
        if (*text == "h264") plan.video.codec = proto::Codec::h264;
        else if (*text == "hevc") plan.video.codec = proto::Codec::hevc;
        else if (*text == "hdr") plan.video.codec = proto::Codec::hevc_hdr;
        else return "--codec is h264, hevc or hdr";
    }
    // What the controller does while the stream is held: each button down, and up again 100 ms later.
    if (const auto text = parsed.option("--tap")) {
        const auto taps = parse_taps(*text);
        if (!taps) {
            return "--tap takes buttons with the second they are pressed in, like right@2,left@3.5 (cross circle square "
                   "triangle left right up down l1 r1 l3 r3 options share touchpad ps)";
        }
        for (const Tap& tap : *taps) {
            plan.actions.push_back(SessionPlan::Action{tap.at, tap.button, true});
            plan.actions.push_back(SessionPlan::Action{tap.at + 100ms, tap.button, false});
        }
        std::ranges::stable_sort(plan.actions, {}, &SessionPlan::Action::at);
    }
    return std::nullopt;
}

// With a controller of the caller's, the stream's loop looks at it this often at least.
constexpr auto kControllerEvery = 2ms;

// Tells a pause on the network from a pause in this program, by the times the system took
// the stream's packets off the network and the times they were read.
struct ArrivalWatch {
    static constexpr double kLongPauseMs = 35.0;  // two frames' time: a picture comes late
    static constexpr double kSlowReadMs = 5.0;
    std::optional<std::chrono::system_clock::time_point> last;
    std::uint64_t timed = 0, pauses = 0, slow_reads = 0;
    double longest_pause_ms = 0.0, longest_wait_ms = 0.0;

    // The pause before this packet, in milliseconds, when it was a long one.
    std::optional<double> packet(std::chrono::system_clock::time_point taken, std::chrono::system_clock::time_point read) {
        const auto ms = [](std::chrono::system_clock::duration time) { return std::chrono::duration<double, std::milli>(time).count(); };
        ++timed;
        const double waited = ms(read - taken);
        longest_wait_ms = std::max(longest_wait_ms, waited);
        if (waited > kSlowReadMs) ++slow_reads;
        std::optional<double> pause;
        if (last) {
            const double since = ms(taken - *last);
            longest_pause_ms = std::max(longest_pause_ms, since);
            if (since > kLongPauseMs) {
                ++pauses;
                pause = since;
            }
        }
        last = taken;
        return pause;
    }
};

int run_session_here(const SessionPlan& plan) {
    namespace transport = proto::transport;
    const transport::VideoProfile& video = plan.video;
    const std::vector<SessionPlan::Action>& actions = plan.actions;

    auto context = load_context();
    if (!context) return fail("session: {}", context.error());
    const StoredConsole* console = context->store.find(plan.console);
    if (console == nullptr) {
        return fail("No registered console is called '{}'. '{} consoles' lists them.", plan.console, PSVP_BINARY);
    }
    const proto::RegisteredConsole& registration = console->registration;
    if (registration.kind == ConsoleKind::ps4 && video.codec != proto::Codec::h264) return usage("session: a PS4 streams H.264 only");
    const std::string host(plan.host.value_or(console->last_address));
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
        if (!stream && stream.error().ends_with("Connection refused")) {
            // Seen on a PS5 that answered the search as "ready" a moment before; half a minute later it took the session.
            (void)fail("{} refused the connection: it is on, but is not taking Remote Play sessions at this moment "
                       "(it may still be starting, or closing an earlier session). Try again in a few seconds.",
                       registration.nickname);
            return kSessionNotYet;
        }
        if (!stream) return fail("session: {}", stream.error());
        const std::string request = proto::build_session_request(registration.kind, address_text, registration.regist_key);
        if (const auto sent = stream->send_all(net::bytes_of(request), Clock::now() + kStepWait); !sent) return fail("session: {}", sent.error());
        const auto head = net::read_text_head(*stream, Clock::now() + kStepWait, kMaxResponseHead);
        if (!head) return fail("session: {}", head.error());
        const auto parsed_nonce = proto::parse_session_response(head->head);
        if (!parsed_nonce) {
            (void)fail("{}: {}.", registration.nickname, proto::describe(parsed_nonce.error()));
            // A session that was just closed is still "in use" to the console for some seconds.
            return parsed_nonce.error().refusal == proto::SessionRefusal::in_use ? kSessionNotYet : kFailed;
        }
        nonce = *parsed_nonce;
    }
    const proto::RpCipher cipher = proto::make_session_cipher(registration.kind, nonce, registration.key);

    // The control channel. One more try if the console does not answer the first.
    const proto::CtrlRequest request = proto::build_ctrl_request(cipher, address_text, registration.regist_key,
                                                                 proto::make_device_id(proto::random_key()), video.codec);
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
    const auto control_opened = Clock::now();

    proto::CtrlChannel channel(cipher, request.next_local_number, accepted->next_remote_number);
    const auto send = [&](std::uint16_t type, std::span<const std::uint8_t> payload = {}) {
        const auto bytes = channel.encode(type, payload);
        return bytes && control.send_all(*bytes, Clock::now() + kStepWait).has_value();
    };

    // The control channel's own business, done whenever something waits: what the
    // console sent is read and answered. The first failure is kept in `control_failure`.
    std::optional<std::string> session_id;
    bool have_session_id = false;
    bool passcode_sent = false;
    int heartbeats = 0;
    std::string control_failure;
    const auto broke = [&](std::string why) {
        if (control_failure.empty()) control_failure = std::move(why);
        return false;
    };
    const auto handle_messages = [&]() {
        while (const auto message = channel.next()) {
            using namespace proto::ctrl_type;
            if (message->type == kSessionId) {
                session_id = proto::parse_session_id(message->payload);
                have_session_id = true;
                if (session_id) say("Session ID received ({} characters).", session_id->size());
                else say("The console sent a session ID in a form this version does not know ({}).", describe_shape(message->payload));
            } else if (message->type == kHeartbeatRequest) {
                if (!send(kHeartbeatReply)) return broke("session: the connection to the console broke");
                ++heartbeats;
            } else if (message->type == kLoginPinRequest) {
                const auto& passcode = plan.passcode;
                if (!passcode) return broke(std::format("{} asks for its login passcode: add --passcode DIGITS.", registration.nickname));
                if (!send(kLoginPinReply, net::bytes_of(*passcode))) return broke("session: the connection to the console broke");
                passcode_sent = true;
            } else if (message->type == kLoginResult) {
                if (message->payload.empty() || message->payload[0] != 0) return broke(std::format("{} did not accept the passcode.", registration.nickname));
                // A console reports the login also when it asked for no passcode.
                say("{}", passcode_sent ? "Passcode accepted." : "Logged in.");
            }
        }
        return true;
    };
    // Reads what has arrived by `until` (at once, if that is now) and handles it.
    const auto service_until = [&](Clock::time_point until) {
        if (!control_failure.empty()) return false;
        std::uint8_t chunk[1024];
        const auto got = control.receive(chunk, until);
        if (!got) {
            if (got.error().ends_with("timed out")) return true;
            return broke(std::format("session: {}", got.error()));
        }
        if (*got == 0) return broke(std::format("{} closed the control channel.", registration.nickname));
        if (!channel.feed(std::span(chunk).first(*got))) return broke("session: the console's messages could not be read");
        return handle_messages();
    };
    const Service service = [&] { return service_until(Clock::now()); };

    if (!channel.feed(head->rest)) return fail("session: the console's messages could not be read");
    if (!handle_messages()) return fail("{}", control_failure);
    // The console names the session first.
    for (const auto give_up = Clock::now() + kStepWait; !have_session_id && Clock::now() < give_up;) {
        if (!service_until(std::min(give_up, Clock::now() + 200ms))) return fail("{}", control_failure);
    }
    if (!have_session_id) return fail("{} never named the session.", registration.nickname);
    // A client with a DualSense first asks for that controller's features and names a controller.
    // Both payloads are the reference's, byte for byte: what the sixteen bytes mean is not known.
    const bool dualsense = plan.dualsense && registration.kind == ConsoleKind::ps5;
    if (dualsense) {
        constexpr std::array<std::uint8_t, 3> kFeatures{0x00, 0x40, 0x00};
        constexpr std::array<std::uint8_t, 16> kController{0xa0, 0xab, 0x51, 0xbd, 0xd1, 0x7e, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00};
        if (!send(proto::ctrl_type::kDualSenseFeatures, kFeatures) || !send(proto::ctrl_type::kControllerConnected, kController)) {
            return fail("session: the connection to the console broke");
        }
        say("Told {} that the controller is a DualSense.", registration.nickname);
    }
    // What other clients send once the session has its name: the microphone muted, twice, and no display devices.
    {
        constexpr std::array<std::uint8_t, 4> kMuted{0, 1, 1, 89};
        constexpr std::array<std::uint8_t, 4> kNoDisplays{};
        if (!send(proto::ctrl_type::kMicrophoneMute, kMuted) || !send(proto::ctrl_type::kMicrophoneMute, kMuted) ||
            !send(proto::ctrl_type::kDisplayDevices, kNoDisplays)) {
            return fail("session: the connection to the console broke");
        }
    }

    // The network probe, on the transport's probe port.
    const auto network = probe_network(net::Address{*ip, transport::kProbePort}, service);
    if (!control_failure.empty()) return fail("{}", control_failure);
    if (!network) return fail("{}: the stream transport: {}.", registration.nickname, network.error());
    say("Stream transport opened (handshake done, {} streams each way).", network->console.outbound_streams);
    transport::ProbeResult measured;
    if (network->measured) {
        measured = *network->measured;
        say("Network measured: round trip {:.2f} ms ({} of {} pings answered), largest packet in {}, out {}.",
            static_cast<double>(measured.round_trip.count()) / 1000.0, network->pongs, transport::ClientProbe::kPings, measured.mtu_in,
            measured.mtu_out);
    } else {
        say("The network could not be measured ({}); a stream would start with the defaults (largest packet {}).",
            network->probe_failure, measured.mtu_in);
    }

    // The stream connection: its handshake, the opening message, the keys, the stream information.
    const auto setup_started = Clock::now();
    auto link = open_transport(net::Address{*ip, transport::kStreamPort}, service);
    if (!control_failure.empty()) return fail("{}", control_failure);
    if (!link) return fail("{}: the stream connection: {}.", registration.nickname, link.error());
    transport::StreamRequest stream_request;
    stream_request.kind = registration.kind;
    const bool own_id = plan.invented_id || !session_id;
    stream_request.session_id = own_id ? invented_session_id() : *session_id;
    stream_request.session_cipher = cipher;
    stream_request.network = measured;
    stream_request.video = video;
    stream_request.dualsense = dualsense;
    stream_request.reported_loss_ceiling = plan.reported_loss_ceiling;
    stream_request.ack_copies = plan.ack_copies;
    transport::ClientStream stream(link->tag, link->endpoint, std::move(stream_request), proto::KeyExchange::generate(), proto::random_key());
    using State = transport::ClientStream::State;
    // A keyframe arrives as one burst of packets: room for them while the control channel is looked after.
    const std::size_t receive_buffer = link->socket.set_receive_buffer(4U << 20);
    const bool arrivals_timed = link->socket.note_arrival_times();
    ArrivalWatch arrivals;
    transport::StreamCounters minute_from;
    std::uint64_t repeats_said = 0;
    auto next_minute = Clock::now() + std::chrono::minutes(1);
    // The frames that are ready leave the stream at once; they are kept only when a dump was asked for.
    std::uint64_t video_bytes = 0;
    std::uint64_t sound_bytes = 0;
    std::set<std::uint32_t> sound_streams_seen;
    const auto take_media = [&] {
        transport::MediaOutput media = stream.take_media();
        for (const auto& frame : media.video) video_bytes += frame.data.size();
        for (const auto& frame : media.sound) sound_bytes += frame.data.size();
        if (plan.on_media && (!media.video.empty() || !media.sound.empty())) plan.on_media(media, stream.info());
        // A sound stream of a kind this version does not know is worth a line when it first shows up, and a look at its first packets.
        for (const auto& [key, count] : stream.counters().sound_streams) {
            if (sound_streams_seen.insert(key).second && stream.state() == State::ready && key >> 8 != 0) {
                log::info("stream: {:.1f} s: sound packets of stream {} with codec {} begin", std::chrono::duration<double>(Clock::now() - setup_started).count(),
                          key >> 8, key & 0xffU);
            }
        }
        for (const auto& other : stream.take_other_sound()) {
            log::info("stream: a sound packet of the unknown stream {}: codec {}, frame {}, {} new and {} repeated frames of {} bytes; its payload begins {}",
                      other.stream, other.codec, other.frame_index, other.new_frames, other.repeated_frames, other.unit_size,
                      proto::hex_encode(std::span(other.payload).first(std::min<std::size_t>(other.payload.size(), 48))));
        }
        const auto data = stream.take_console_data();
        if (plan.on_console_data && !data.empty()) plan.on_console_data(data, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - setup_started));
    };

    const auto send_all = [&](const transport::Datagrams& datagrams) {
        for (const auto& datagram : datagrams) (void)link->socket.send_to(link->console, datagram);
    };
    // Runs the stream until `stop` says so, the stream ends, or the control channel fails.
    // How the console sees the connection, as it was written into the log last.
    std::optional<proto::ConnectionQuality> view_said;
    const auto run_stream = [&](const std::function<bool()>& stop) -> std::expected<void, std::string> {
        while (!stop() && stream.state() != State::failed && !(stream.state() == State::closed && !stream.goodbye_pending())) {
            if (!service()) return {};
            const auto datagram = link->socket.receive(std::min(stream.deadline(), Clock::now() + (plan.controller ? kControllerEvery : kServiceEvery)));
            if (!datagram) return std::unexpected(datagram.error());
            if (*datagram) {
                if ((*datagram)->from != link->console) continue;
                const auto now = Clock::now();
                // Once the stream runs: a pause between two packets is the network's (or the console's), a
                // packet that waited to be read is this program's.
                if ((*datagram)->taken && stream.state() == State::ready && stream.info()) {
                    if (const auto pause = arrivals.packet(*(*datagram)->taken, std::chrono::system_clock::now())) {
                        log::debug("stream: {:.1f} s: nothing arrived for {:.0f} ms", std::chrono::duration<double>(now - setup_started).count(), *pause);
                    }
                }
                if (plan.on_packet) plan.on_packet(registration.kind, (*datagram)->data, std::chrono::duration_cast<std::chrono::microseconds>(now - setup_started));
                send_all(stream.on_datagram((*datagram)->data, now));
                // The console lowers the picture's bitrate, and with it what a person sees, when it thinks the
                // connection is poor: when its view moves, the log says so, with the time.
                if (const auto& quality = stream.quality(); quality && connection_view_changed(view_said, *quality)) {
                    view_said = *quality;
                    log::debug("stream: {:.1f} s: the console's view of the connection: target bitrate {} kbit/s, round-trip value {:.1f}, loss {}",
                               std::chrono::duration<double>(now - setup_started).count(), quality->target_bitrate.value_or(0) / 1000, quality->rtt.value_or(0.0),
                               quality->loss.value_or(0));
                }
                // A message of the console's that came again: an acknowledgment of ours did not reach it, and it
                // may count the 200 ms it waited as the round trip.
                if (stream.counters().console_repeats != repeats_said) {
                    repeats_said = stream.counters().console_repeats;
                    log::debug("stream: {:.1f} s: the console sent a message again ({} so far): an acknowledgment did not reach it",
                               std::chrono::duration<double>(now - setup_started).count(), repeats_said);
                }
                // Once a minute: what the congestion reports of that minute told the console, against what was counted.
                if (now >= next_minute) {
                    const transport::StreamCounters& told = stream.counters();
                    const std::uint64_t received = told.congestion_received - minute_from.congestion_received;
                    const std::uint64_t lost = told.congestion_lost - minute_from.congestion_lost;
                    if (stream.state() == State::ready && (received != 0 || lost != 0)) {
                        log::debug("stream: {:.1f} s: congestion reports of the last minute: {} packets arrived, {} lost ({:.2f}%); told of {} lost in {} reports, {} of them lowered to the {}% ceiling",
                                   std::chrono::duration<double>(now - setup_started).count(), received, lost,
                                   100.0 * static_cast<double>(lost) / static_cast<double>(received + lost), told.congestion_lost_told - minute_from.congestion_lost_told,
                                   told.congestion_reports - minute_from.congestion_reports, told.congestion_lowered - minute_from.congestion_lowered,
                                   plan.reported_loss_ceiling);
                    }
                    minute_from = told;
                    next_minute = now + std::chrono::minutes(1);
                }
            }
            // A deadline may pass while packets keep coming: it is looked at after every one of them.
            if (Clock::now() >= stream.deadline()) send_all(stream.on_timeout(Clock::now()));
            take_media();
        }
        return {};
    };
    send_all(stream.start(Clock::now()));
    say("Stream connection opened; opening message sent ({} bytes in {} datagram{}), session ID: {}.", stream.opening_size(),
        stream.opening_datagrams(), stream.opening_datagrams() == 1 ? "" : "s", own_id ? "an invented one" : "the console's own");
    if (const auto ran = run_stream([&] { return stream.state() == State::ready; }); !ran) return fail("session: {}", ran.error());
    if (!control_failure.empty()) return fail("{}", control_failure);
    if (const auto& bang = stream.bang()) {
        using Code = transport::BangFacts::Code;
        say("The console answered: stream protocol version {}; its key signature {}; the answer {}; its session key {}.", bang->server_version,
            bang->signature_matches ? "matches" : "DOES NOT MATCH",
            bang->code == Code::none      ? "carried no authentication code"
            : bang->code == Code::valid   ? "was sealed with the new keys"
            : bang->code == Code::invalid ? "carried a code the new keys do not give"
                                          : "came in several chunks (its code was not looked at)",
            bang->session_key_is_ours ? "repeats the one sent" : std::format("is another one, of {} characters", bang->session_key_size));
    }
    if (stream.state() == State::failed) return fail("{}: the stream: {}.", registration.nickname, stream.reason());
    if (stream.state() == State::closed) return fail("{} closed the stream: {}.", registration.nickname, stream.reason());
    if (const auto& info = stream.info()) {
        std::string sizes;
        for (const auto& resolution : info->resolutions) {
            sizes += std::format("{}{}x{} (header of {} bytes)", sizes.empty() ? "" : ", ", resolution.width, resolution.height, resolution.video_header.size());
        }
        const proto::AudioHeader audio = proto::parse_audio_header(info->audio_header).value_or(proto::AudioHeader{});
        say("Stream information: picture {}; sound {} channels, {} bits, {} Hz, frames of {}.", sizes.empty() ? "none" : sizes, audio.channels,
            audio.bits, audio.rate, audio.frame_size);
        // What else the console says it sends as sound: every channel with its type and its format, as it came.
        for (const proto::SoundChannel& listed : info->sound_channels) {
            const auto format = proto::parse_audio_header(listed.header);
            say("Sound channel of type {}: {}{}; its header: {}.", listed.type,
                format ? std::format("{} channels, {} bits, {} Hz, frames of {}", format->channels, format->bits, format->rate, format->frame_size)
                       : std::string("a format that is not read"),
                listed.raw ? (*listed.raw ? ", samples as they are" : ", coded") : "", proto::hex_encode(listed.header));
        }
        if (info->sound_channels.empty()) say("The console listed no sound channels of their own.");
    }
    say("Stream set up in {} ms.", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - setup_started).count());

    // Hold it: heartbeats go out, and whatever the console sends is counted and its codes checked.
    const auto hold_started = Clock::now();
    const auto hold_until = hold_started + plan.hold;
    transport::Buttons buttons;
    transport::InputState controller_state;
    std::size_t next_action = 0;
    const auto held = run_stream([&] {
        const auto now = Clock::now();
        for (; next_action < actions.size() && now >= hold_started + actions[next_action].at; ++next_action) {
            buttons.set(actions[next_action].button, actions[next_action].down);
            send_all(stream.set_controller(buttons, transport::InputState{}, now));
            say("{:.3f} s: {} {}.", std::chrono::duration<double>(now - hold_started).count(), transport::button_name(actions[next_action].button),
                actions[next_action].down ? "pressed" : "released");
        }
        // A controller of the caller's: what it does goes out when it changes.
        if (plan.controller && actions.empty() && plan.controller(buttons, controller_state)) {
            send_all(stream.set_controller(buttons, controller_state, now));
        }
        // No frame can be decoded from what the decoder holds: a frame that needs none is asked for.
        if (plan.wants_keyframe && plan.wants_keyframe()) send_all(stream.request_keyframe(now));
        return now >= hold_until || (plan.stop && plan.stop());
    });
    const auto held_for = Clock::now() - hold_started;
    if (!held) return fail("session: {}", held.error());
    if (!control_failure.empty()) return fail("{}", control_failure);
    const transport::StreamCounters& counters = stream.counters();
    say("Stream held for {} seconds: {} picture and {} sound packets arrived with the right authentication code, {} with a wrong one, "
        "{} before the keys; control packets refused: {}; heartbeats sent: {}.",
        std::chrono::duration_cast<std::chrono::seconds>(held_for).count(), counters.video_packets, counters.audio_packets, counters.media_rejected,
        counters.media_before_keys, stream.control_rejected(), counters.heartbeats_sent);
    {
        const transport::FrameCounters& frames = stream.video();
        const transport::SoundCounters& sound = stream.sound();
        const transport::SoundCounters& haptics = stream.haptics();
        say("Picture: {} frames put together ({} bytes), {} of them with restored packets ({} packets restored), {} frames lost; "
            "packets not needed: {}, too late: {}, twice: {}, unusable: {}, unreadable: {}.",
            frames.frames, video_bytes, frames.frames_repaired, frames.units_repaired, frames.frames_lost, frames.packets_spare,
            frames.packets_late, frames.packets_duplicate, frames.packets_unusable, counters.media_unreadable);
        if (frames.packets_odd != 0) say("Picture packets used although their sizes or counts are not their frame's: {}.", frames.packets_odd);
        say("Sound: {} frames ({} bytes with the haptics), {} of them taken from a later packet, {} lost, {} packets unusable; "
            "haptics: {} frames, {} lost.",
            sound.frames, sound_bytes, sound.frames_recovered, sound.frames_lost, sound.packets_unusable + haptics.packets_unusable,
            haptics.frames, haptics.frames_lost);
        say("Told the console: {} times of frames that did not arrive, {} congestion reports, {} input states (a resting controller's); "
            "receive buffer: {} bytes.",
            counters.loss_reports, counters.congestion_reports, counters.input_states, receive_buffer);
        if (counters.acks != 0) {
            say("Acknowledgments of the console's messages: {}, and {} copies sent 40 and 120 ms behind them; the console sent {} of its "
                "messages again (an acknowledgment had not reached it within 200 ms).",
                counters.acks, counters.ack_copies, counters.console_repeats);
        }
        if (counters.congestion_reports != 0) {
            const std::uint64_t total = counters.congestion_received + counters.congestion_lost;
            say("Congestion reports: {} packets counted as arrived and {} as lost ({:.2f}%); the console was told of {} lost ({} of {} reports "
                "lowered to the {}% ceiling).",
                counters.congestion_received, counters.congestion_lost,
                total != 0 ? 100.0 * static_cast<double>(counters.congestion_lost) / static_cast<double>(total) : 0.0,
                counters.congestion_lost_told, counters.congestion_lowered, counters.congestion_reports, plan.reported_loss_ceiling);
        }
        if (counters.keyframe_requests != 0) say("Asked the console for a frame that needs no earlier one: {} times.", counters.keyframe_requests);
        if (!actions.empty()) {
            say("Buttons: {} changes told to the console in {} input history packets (each change's packet, and the last one sent again).",
                counters.input_events, counters.history_packets);
        }
        if (arrivals.timed != 0) {
            say("Arrival: {} times nothing came from the console for over {:.0f} ms (the longest pause: {:.0f} ms); a packet waited {:.1f} ms at "
                "most before this program read it ({} waited over {:.0f} ms).",
                arrivals.pauses, ArrivalWatch::kLongPauseMs, arrivals.longest_pause_ms, arrivals.longest_wait_ms, arrivals.slow_reads,
                ArrivalWatch::kSlowReadMs);
        } else if (!arrivals_timed) {
            say("Arrival: this system does not tell when a packet came in.");
        }
    }
    if (plan.on_report) plan.on_report();
    if (!counters.messages.empty()) say("Messages from the console: {}.", counts_text(counters.messages, "type"));
    if (const auto& quality = stream.quality()) {
        say("The console's latest view of the connection: target bitrate {} kbit/s, round-trip value {:.1f}, loss {}.",
            quality->target_bitrate.value_or(0) / 1000, quality->rtt.value_or(0.0), quality->loss.value_or(0));
    }
    if (!counters.other_data.empty()) say("Other data from the console: {}.", counts_text(counters.other_data, "kind"));
    if (!counters.sound_streams.empty()) {
        // A PS5 names the stream of every sound packet: 0 the sound, 2 a controller's haptics. Another number would be a stream of its own.
        std::string streams;
        for (const auto& [key, count] : counters.sound_streams) {
            streams += std::format("{}stream {} with codec {} x{}", streams.empty() ? "" : ", ", key >> 8, key & 0xffU, count);
        }
        say("Sound packets by the stream they name: {}{}.", streams,
            counters.other_sound_packets != 0 ? std::format("; {} of a stream this version does not know were counted and not played", counters.other_sound_packets) : "");
    }
    if (counters.split_messages != 0) say("Messages that came in more than one chunk: {}.", counters.split_messages);
    if (counters.other_datagrams != 0) say("Datagrams of kinds this version does not read: {}.", counters.other_datagrams);
    if (stream.state() == State::failed) return fail("{}: the stream: {}.", registration.nickname, stream.reason());
    const bool console_closed = stream.state() == State::closed;
    if (console_closed) say("{} closed the stream: {}.", registration.nickname, stream.reason());

    if (plan.rest) {
        if (!send(proto::ctrl_type::kGoToRest)) return fail("session: the connection to the console broke");
        say("Asked {} to go to rest mode.", registration.nickname);
    }
    if (!console_closed) {
        // The goodbye, and a short wait for the console to take it.
        send_all(stream.disconnect(Clock::now()));
        const auto goodbye_until = Clock::now() + 500ms;
        (void)run_stream([&] { return Clock::now() >= goodbye_until; });
        say("Goodbye sent{}.", stream.goodbye_pending() ? " (not acknowledged within half a second)" : " and acknowledged");
    }
    say("Control channel held for {:.1f} seconds; heartbeats answered: {}.",
        std::chrono::duration<double>(Clock::now() - control_opened).count(), heartbeats);
    if (plan.verdict) {
        if (const auto failure = plan.verdict()) return fail("session: {}", *failure);
    }
    return kDone;
}

int command_session(std::span<const std::string_view> args) {
    const Parsed parsed = parse(
        args, {"--host", "--seconds", "--passcode", "--resolution", "--fps", "--bitrate", "--codec", "--session-id", "--dump", "--tap", "--loss-ceiling", "--ack-copies"},
        {"--rest", "--decode", "--expect-clip"}, 1);
    if (!parsed.error.empty()) return usage("session: {}", parsed.error);
    SessionPlan plan;
    if (const auto wrong = read_plan(parsed, plan)) return usage("session: {}", *wrong);
    if (const auto text = parsed.option("--seconds")) {
        const auto seconds = parse_seconds(*text, 600);
        if (!seconds) return usage("session: --seconds takes a number from 1 to 600");
        plan.hold = *seconds;
    }
    const std::string_view id_choice = parsed.option("--session-id").value_or("console");
    if (id_choice != "console" && id_choice != "invented") return usage("session: --session-id is console or invented");
    plan.invented_id = id_choice == "invented";

    std::optional<StreamDump> dump;
    if (const auto folder = parsed.option("--dump")) {
        if (folder->empty()) return usage("session: --dump takes a folder");
        auto opened = StreamDump::open(std::filesystem::path(*folder));
        if (!opened) return fail("session: --dump: {}", opened.error());
        dump.emplace(std::move(*opened));
    }
    const bool expect_clip = parsed.flag("--expect-clip");
    std::optional<Decoding> decoding;
    if (parsed.flag("--decode") || expect_clip) decoding.emplace(Decoding::Options{.expect_clip = expect_clip});

    // The frames that are ready leave the stream at once; they are decoded and kept only when that was asked for.
    if (dump) {
        plan.on_packet = [&](ConsoleKind kind, std::span<const std::uint8_t> datagram, std::chrono::microseconds since) { dump->packet(kind, datagram, since); };
        plan.on_console_data = [&](const std::vector<proto::transport::ConsoleData>& data, std::chrono::microseconds since) { dump->take(data, since); };
    }
    plan.on_media = [&](proto::transport::MediaOutput& media, const std::optional<proto::StreamInfo>& stream_info) {
        if (decoding && stream_info) {
            const proto::StreamInfo& info = *stream_info;
            for (const auto& frame : media.video) {
                const std::span<const std::uint8_t> header =
                    frame.adaptive_stream < info.resolutions.size() ? std::span<const std::uint8_t>(info.resolutions[frame.adaptive_stream].video_header)
                                                                    : std::span<const std::uint8_t>();
                decoding->video(header, frame.adaptive_stream, frame.index, frame.data, frame.lost_before);
            }
            const proto::AudioHeader audio = proto::parse_audio_header(info.audio_header).value_or(proto::AudioHeader{});
            for (const auto& frame : media.sound) {
                if (!frame.haptics) decoding->sound(audio.rate, audio.channels, audio.frame_size, frame.data, frame.lost_before);
            }
        }
        if (dump) dump->take(std::move(media), stream_info);
    };
    plan.on_report = [&] {
        if (dump) {
            const bool written = dump->close();
            say("Kept in {}: video.bin ({} bytes), sound.bin ({} bytes), and a line per frame, per packet and per other message{}.", dump->folder().string(),
                dump->video_bytes(), dump->sound_bytes(), written ? "" : "; SOMETHING COULD NOT BE WRITTEN");
        }
        if (decoding) {
            for (const std::string& line : decoding->report()) say("{}", line);
        }
    };
    plan.verdict = [&]() -> std::optional<std::string> {
        if (!decoding || decoding->good()) return std::nullopt;
        return expect_clip ? "what was decoded is not the test clip" : "nothing of the stream could be decoded";
    };
    const int code = run_session_here(plan);
    return code == kSessionNotYet ? kFailed : code;
}

PlayRunner play_runner = nullptr;

// The stream in a window, with its sound and a controller: what the program is for.
int command_play(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--host", "--seconds", "--passcode", "--resolution", "--fps", "--bitrate", "--codec", "--tap", "--frames", "--controller", "--pacing", "--cushion", "--haptics-volume", "--back-left", "--back-right", "--loss-ceiling", "--ack-copies"},
                                {"--rest", "--wake"}, 1);
    if (!parsed.error.empty()) return usage("play: {}", parsed.error);
    SessionPlan plan;
    // What a console sends when nothing else is asked for: 1080p, 60 pictures a second, HEVC from a PS5.
    plan.video.width = 1920;
    plan.video.height = 1080;
    plan.video.bitrate_kbps = 15000;
    plan.video.codec = proto::Codec::hevc;
    if (const auto wrong = read_plan(parsed, plan)) return usage("play: {}", *wrong);
    plan.hold = std::chrono::hours(24);
    if (const auto text = parsed.option("--seconds")) {
        const auto seconds = parse_seconds(*text, 600);
        if (!seconds) return usage("play: --seconds takes a number from 1 to 600");
        plan.hold = *seconds;
    }
    PlayOptions options;
    if (const auto text = parsed.option("--frames")) {
        const auto frames = parse_number(*text, 1, 100000000);
        if (!frames) return usage("play: --frames takes a whole number above zero");
        options.frames = *frames;
    }
    if (const auto text = parsed.option("--controller")) {
        if (*text == "dualsense") options.controller = PlayOptions::Controller::dualsense;
        else if (*text == "dualshock4") options.controller = PlayOptions::Controller::dualshock4;
        else if (*text != "auto") return usage("play: --controller is auto, dualsense or dualshock4");
    }
    if (const auto text = parsed.option("--haptics-volume")) {
        const auto percent = parse_number(*text, 0, 100);
        if (!percent && *text != "0") return usage("play: --haptics-volume takes a number from 0 to 100");
        options.haptics_volume = percent ? static_cast<int>(*percent) : 0;
    }
    for (const auto& [name, target] : {std::pair{"--back-left", &options.back_left}, std::pair{"--back-right", &options.back_right}}) {
        const auto text = parsed.option(name);
        if (!text) continue;
        *target = proto::transport::button_named(*text);
        if (!*target) return usage("play: {} takes a button's name, as --tap does (cross, circle, l1, r3, options ...)", name);
    }
    if (const auto text = parsed.option("--pacing")) {
        if (*text != "on" && *text != "off") return usage("play: --pacing is on or off");
        options.pacing = *text == "on";
    }
    if (const auto text = parsed.option("--cushion")) {
        const auto milliseconds = parse_number(*text, 0, 50);
        if (!milliseconds && *text != "0") return usage("play: --cushion takes a number of milliseconds from 0 to 50");
        options.cushion_ms = milliseconds ? static_cast<int>(*milliseconds) : 0;
        if (options.cushion_ms > 0 && !options.pacing) return usage("play: --cushion needs the pacing it is part of (not --pacing off)");
    }
    // A PS4 streams H.264 only: for one, the codec nobody chose is that.
    if (!parsed.option("--codec")) {
        if (auto context = load_context()) {
            const StoredConsole* console = context->store.find(plan.console);
            if (console != nullptr && console->registration.kind == ConsoleKind::ps4) plan.video.codec = proto::Codec::h264;
        }
    }
    if (play_runner == nullptr) return fail("play: this build has no window to play in");
    // Everything a session says and logs is kept: the file of the last session, and the one before beside it.
    if (const auto dirs = resolve_dirs(process_env())) {
        const auto file = dirs->state / "logs" / "play.log";
        if (!log::open_file(file)) log::warn("log: cannot write {}", file.string());
    }
    // Builds between two releases carry one version: when this one was made tells them apart.
    std::string built;
    {
        std::error_code failed;
        const auto written = std::filesystem::last_write_time("/proc/self/exe", failed);
        if (!failed) built = std::format(", built {:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(std::chrono::file_clock::to_sys(written)));
    }
    log::info("{} {}{}: play {}", PSVP_BINARY, PSVP_VERSION, built, plan.console);
    // With --wake a resting console is woken first, and one that takes no session yet (it is still
    // starting, or closing the session before) is asked again for a minute and a half.
    const bool wake = parsed.flag("--wake");
    int code = wake ? wake_if_resting(plan) : kDone;
    if (code == kDone) {
        const auto give_up = Clock::now() + 95s;
        for (;;) {
            code = play_runner(plan, options);
            if (code != kSessionNotYet || !wake || Clock::now() >= give_up) break;
            say("{} is not taking a session yet: asking again in 5 seconds.", plan.console);
            std::this_thread::sleep_for(5s);
        }
        if (code == kSessionNotYet) code = kFailed;
    }
    log::info("play ended with code {}", code);
    log::close_file();
    return code;
}

ControllersRunner controllers_runner = nullptr;

// The controllers that are connected: what a session would find.
int command_controllers(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--pretend", "--pretend-taps"}, {"--json", "--watch", "--presses"}, 0);
    if (!parsed.error.empty()) return usage("controllers: {}", parsed.error);
    if (controllers_runner == nullptr) return fail("controllers: this build does not read controllers");
    ControllersOptions options;
    options.json = parsed.flag("--json");
    options.watch = parsed.flag("--watch");
    options.presses = parsed.flag("--presses");
    if (const auto text = parsed.option("--pretend")) options.pretend = std::string(*text);
    if (const auto text = parsed.option("--pretend-taps")) options.pretend_taps = std::string(*text);
    if (options.presses && !options.watch) return usage("controllers: --presses goes with --watch");
    if (options.pretend_taps && !(options.pretend && options.watch)) return usage("controllers: --pretend-taps goes with --pretend and --watch");
    return controllers_runner(options);
}

// Decodes what 'session --dump' kept: the same decoders, without a console.
int command_decode(std::span<const std::string_view> args) {
    const Parsed parsed = parse(args, {"--lose"}, {"--expect-clip"}, 1);
    if (!parsed.error.empty()) return usage("decode: {}", parsed.error);
    // Frames to leave out, as if they had not arrived: what a player then shows is compared with the whole stream's pictures.
    std::vector<std::uint64_t> lose;
    if (const auto text = parsed.option("--lose")) {
        for (const auto part : std::views::split(*text, ',')) {
            std::uint64_t first = 0, last = 0;
            const std::string_view range(part.begin(), part.end());
            const auto dash = range.find('-');
            const std::string_view from = range.substr(0, dash), to = dash == std::string_view::npos ? from : range.substr(dash + 1);
            const auto a = std::from_chars(from.data(), from.data() + from.size(), first);
            const auto b = std::from_chars(to.data(), to.data() + to.size(), last);
            if (a.ec != std::errc() || b.ec != std::errc() || a.ptr != from.data() + from.size() || b.ptr != to.data() + to.size() || last < first || last - first > 1000) {
                return usage("decode: --lose takes frame numbers and ranges, as in 100,250-252");
            }
            for (std::uint64_t frame = first; frame <= last; ++frame) lose.push_back(frame);
        }
    }
    if (parsed.words.empty()) return usage("decode: which folder? Give one that 'session --dump' wrote");
    const std::filesystem::path folder(parsed.words.front());
    const auto read = [&](const char* name) -> std::optional<std::string> {
        std::ifstream file(folder / name, std::ios::binary);
        if (!file) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    // The lists: a heading, then a line of numbers per frame.
    const auto rows = [](std::string_view text) {
        std::vector<std::vector<std::uint64_t>> out;
        bool heading = true;
        for (const auto line : std::views::split(text, '\n')) {
            if (std::exchange(heading, false) || line.empty()) continue;
            std::vector<std::uint64_t> row;
            for (const auto field : std::views::split(line, '\t')) {
                std::uint64_t value = 0;
                std::from_chars(field.data(), field.data() + field.size(), value);
                row.push_back(value);
            }
            out.push_back(std::move(row));
        }
        return out;
    };
    const auto video = read("video.bin"), video_list = read("video.tsv"), sound = read("sound.bin"), sound_list = read("sound.tsv");
    if (!video || !video_list || !sound || !sound_list) return fail("decode: {} is not a folder that 'session --dump' wrote", folder.string());

    Decoding decoding(Decoding::Options{.expect_clip = parsed.flag("--expect-clip")});
    const auto bytes = [](const std::string& text, std::size_t at, std::size_t size) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()) + at, size);
    };
    // video.tsv: frame, bytes, repaired, lost-before, picture-size, header-bytes (the header stands before the frame).
    std::size_t at = 0;
    std::map<std::uint64_t, std::span<const std::uint8_t>> headers;
    std::uint32_t left_out = 0;
    for (const auto& row : rows(*video_list)) {
        if (row.size() < 6 || row[5] > video->size() - at || row[1] > video->size() - at - row[5]) return fail("decode: video.tsv does not fit video.bin");
        if (row[5] != 0) headers[row[4]] = bytes(*video, at, row[5]);
        at += row[5];
        const auto index = static_cast<std::uint16_t>(row[0]);
        const auto frame = bytes(*video, at, row[1]);
        at += row[1];
        if (!lose.empty()) decoding.reference(headers[row[4]], static_cast<unsigned>(row[4]), index, frame);
        if (std::ranges::find(lose, row[0]) != lose.end()) {
            ++left_out;
            continue;
        }
        decoding.video(headers[row[4]], static_cast<unsigned>(row[4]), index, frame, static_cast<std::uint32_t>(row[3]) + std::exchange(left_out, 0U));
    }
    // sound.tsv: frame, bytes, haptics, from-a-later-packet, lost-before. The format is the one every console was seen to send.
    const proto::AudioHeader format;
    at = 0;
    for (const auto& row : rows(*sound_list)) {
        if (row.size() < 5 || row[1] > sound->size() - at) return fail("decode: sound.tsv does not fit sound.bin");
        if (row[2] == 0) decoding.sound(format.rate, format.channels, format.frame_size, bytes(*sound, at, row[1]), static_cast<std::uint32_t>(row[4]));
        at += row[1];
    }
    for (const std::string& line : decoding.report()) say("{}", line);
    if (!decoding.good()) return fail("decode: {}", parsed.flag("--expect-clip") ? "what was decoded is not the test clip" : "nothing could be decoded");
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
    const Parsed parsed = parse(args, {}, {"--json"}, 1);
    if (!parsed.error.empty()) return usage("account: {}", parsed.error);
    auto context = load_context();
    if (!context) return fail("account: {}", context.error());
    if (!parsed.words.empty()) {
        const auto id = parse_account_id(parsed.words.front());
        if (!id) return usage("account: '{}' is not an account ID (a number, or twelve base64 characters)", parsed.words.front());
        context->store.account_id = id;
        if (const auto saved = save_store(context->store_file, context->store); !saved) return fail("account: {}", saved.error());
    }
    if (parsed.flag("--json")) {
        std::fputs(Json(Json::Object{{"account_id", Json(context->store.account_id ? account_id_base64(*context->store.account_id) : std::string())}}).dump().c_str(), stdout);
        return kDone;
    }
    if (!context->store.account_id) {
        say("No account ID is set. '{} account ID' sets it.", PSVP_BINARY);
        return kDone;
    }
    say("{}  ({})", account_id_base64(*context->store.account_id), account_id_decimal(*context->store.account_id));
    return kDone;
}

}  // namespace

int run_session(const SessionPlan& plan) { return run_session_here(plan); }
void set_play_runner(PlayRunner runner) { play_runner = runner; }
void set_controllers_runner(ControllersRunner runner) { controllers_runner = runner; }

bool is_command(std::span<const std::string_view> args) { return !args.empty() && !args.front().starts_with("-"); }

int run_command(std::span<const std::string_view> args) {
    const std::string_view name = args.front();
    const auto rest = args.subspan(1);
    if (name == "discover") return command_discover(rest);
    if (name == "register") return command_register(rest);
    if (name == "wake") return command_wake(rest);
    if (name == "session") return command_session(rest);
    if (name == "decode") return command_decode(rest);
    if (name == "play") return command_play(rest);
    if (name == "consoles") return command_consoles(rest);
    if (name == "forget") return command_forget(rest);
    if (name == "import") return command_import(rest);
    if (name == "account") return command_account(rest);
    if (name == "controllers") return command_controllers(rest);
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
        "          [--resolution 360|540|720|1080] [--fps 30|60] [--bitrate KBPS] [--codec h264|hevc|hdr]\n"
        "          [--session-id console|invented] [--dump FOLDER] [--decode] [--tap BUTTON@SECOND,...]\n"
        "          [--loss-ceiling PERCENT]\n"
        "      open a session with a console, set a stream up, hold it for N seconds and close it\n"
        "      (a connection check: the picture's frames and the sound's are put together and counted,\n"
        "      nothing is shown yet); --dump keeps them in FOLDER; --decode decodes them and says what came\n"
        "      out; --tap presses buttons for a moment while the stream is held; --rest then sends the\n"
        "      console to rest mode\n"
        "  {0} play CONSOLE [--host ADDRESS] [--passcode DIGITS] [--resolution 360|540|720|1080] [--fps 30|60]\n"
        "          [--bitrate KBPS] [--codec h264|hevc|hdr] [--controller auto|dualsense|dualshock4]\n"
        "          [--pacing on|off] [--cushion 0-50] [--haptics-volume 0-100] [--back-left BUTTON]\n"
        "          [--back-right BUTTON] [--loss-ceiling PERCENT] [--ack-copies 0-2] [--wake] [--rest]\n"
        "      play: the console's picture in a window, its sound, and a controller that is plugged in\n"
        "      or paired; closing the window ends the session (--rest then sends the console to rest mode).\n"
        "      --wake wakes a console that is in rest mode first, and asks one that takes no session yet\n"
        "      again until it does;\n"
        "      --controller says what the console is told the controller is (auto: a DualSense when one\n"
        "      is connected at the start, which gets its haptics and trigger effects); --pacing off shows\n"
        "      every picture the moment it is there instead of evenly, a frame's time apart; --cushion keeps\n"
        "      that many milliseconds of delay, so that frames which come that much late (a wireless\n"
        "      network's short pauses) are still shown in their place (none unless said);\n"
        "      --haptics-volume says how strong a DualSense's haptics are (100 unless said otherwise);\n"
        "      --back-left and --back-right say which of the console's buttons a DualSense Edge's back\n"
        "      buttons are (cross, l3, options ...; nothing unless said);\n"
        "      --loss-ceiling is the most loss the console is told of, in percent of the packets of each\n"
        "      report (5 unless said): a console answers loss with a lower bitrate and fewer pictures,\n"
        "      for a burst on a wireless network as for a slow line; 100 tells it all; --ack-copies says\n"
        "      how often each acknowledgment of the console's messages is sent again, 40 and 120 ms later\n"
        "      (2 unless said): one that is lost makes the console wait 200 ms, which it takes as the\n"
        "      round trip and answers with a lower bitrate\n"
        "  {0} decode FOLDER [--lose FRAMES]\n"
        "      decode the picture and the sound that 'session --dump FOLDER' kept, and say what came out;\n"
        "      --lose 100,250-255 leaves those frames out, as if they had not arrived, and compares the\n"
        "      pictures behind them with the whole stream's\n"
        "  {0} consoles [--json]\n"
        "      list the registered consoles\n"
        "  {0} forget CONSOLE\n"
        "      remove a console from the list\n"
        "  {0} import [FILE]\n"
        "      take over the consoles registered in Chiaki or chiaki-ng\n"
        "  {0} account [ID] [--json]\n"
        "      show or set the PSN account ID used for registering\n"
        "  {0} controllers [--json] [--watch [--presses]]\n"
        "      list the controllers that are connected, with battery and what each can do; --watch goes\n"
        "      on and says it again whenever that changes; --presses also says what is pressed on one\n"
        "      (a direction, accept, back, the page before or after)\n",
        PSVP_BINARY);
}

}  // namespace psvp::app
