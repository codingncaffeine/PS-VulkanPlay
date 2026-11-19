#include "app/console_store.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/account_id.h"
#include "app/json.h"
#include "proto/hex.h"

namespace psvp::app {
namespace {

constexpr int kFormatVersion = 1;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

Json to_json(const StoredConsole& console) {
    const proto::RegisteredConsole& r = console.registration;
    return Json(Json::Object{
        {"kind", Json(proto::to_string(r.kind))},
        {"nickname", Json(r.nickname)},
        {"mac", Json(proto::hex_encode(r.mac))},
        {"regist_key", Json(r.regist_key)},
        {"key", Json(proto::hex_encode(r.key))},
        {"key_type", Json(static_cast<std::int64_t>(r.key_type))},
        {"ap_ssid", Json(r.ap_ssid)},
        {"ap_bssid", Json(r.ap_bssid)},
        {"ap_key", Json(r.ap_key)},
        {"ap_name", Json(r.ap_name)},
        {"last_address", Json(console.last_address)},
    });
}

std::expected<StoredConsole, std::string> from_json(const Json& json) {
    StoredConsole console;
    proto::RegisteredConsole& r = console.registration;
    const std::string kind = json.string_or("kind", "");
    if (kind == "PS5") r.kind = proto::ConsoleKind::ps5;
    else if (kind == "PS4") r.kind = proto::ConsoleKind::ps4;
    else return std::unexpected(std::format("a console of unknown kind '{}'", kind));

    const auto mac = proto::hex_decode(json.string_or("mac", ""));
    const auto key = proto::hex_decode(json.string_or("key", ""));
    r.regist_key = json.string_or("regist_key", "");
    if (!mac || mac->size() != r.mac.size()) return std::unexpected(std::string("a console without a hardware address"));
    if (!key || key->size() != r.key.size()) return std::unexpected(std::string("a console without its secret"));
    if (r.regist_key.empty() || r.regist_key.size() > 16) return std::unexpected(std::string("a console without its registration key"));
    std::copy(mac->begin(), mac->end(), r.mac.begin());
    std::copy(key->begin(), key->end(), r.key.begin());

    r.nickname = json.string_or("nickname", "");
    r.key_type = static_cast<std::uint32_t>(json.number_or("key_type", 0));
    r.ap_ssid = json.string_or("ap_ssid", "");
    r.ap_bssid = json.string_or("ap_bssid", "");
    r.ap_key = json.string_or("ap_key", "");
    r.ap_name = json.string_or("ap_name", "");
    console.last_address = json.string_or("last_address", "");
    return console;
}

}  // namespace

StoredConsole* ConsoleStore::find_mac(const std::array<std::uint8_t, 6>& mac) {
    for (StoredConsole& console : consoles) {
        if (console.registration.mac == mac) return &console;
    }
    return nullptr;
}

StoredConsole* ConsoleStore::find(std::string_view name_or_mac) {
    std::string digits;
    for (const char c : name_or_mac) {
        if (c != ':' && c != '-') digits.push_back(c);
    }
    if (const auto mac = proto::hex_decode(digits); mac && mac->size() == 6) {
        std::array<std::uint8_t, 6> wanted{};
        std::copy(mac->begin(), mac->end(), wanted.begin());
        if (StoredConsole* found = find_mac(wanted)) return found;
    }
    for (StoredConsole& console : consoles) {
        if (same_ignoring_case(console.registration.nickname, name_or_mac)) return &console;
    }
    return nullptr;
}

void ConsoleStore::put(StoredConsole console) {
    if (StoredConsole* existing = find_mac(console.registration.mac)) {
        if (console.last_address.empty()) console.last_address = existing->last_address;
        *existing = std::move(console);
    } else {
        consoles.push_back(std::move(console));
    }
}

std::filesystem::path store_path(const std::filesystem::path& config_dir) { return config_dir / "consoles.json"; }

std::expected<ConsoleStore, std::string> load_store(const std::filesystem::path& file) {
    ConsoleStore store;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return store;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::unexpected(std::format("cannot read {}", file.string()));
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    const auto json = Json::parse(text);
    if (!json) return std::unexpected(std::format("{} is damaged: {}", file.string(), json.error()));
    if (json->as_object() == nullptr) return std::unexpected(std::format("{} is damaged: not an object", file.string()));
    const int version = static_cast<int>(json->number_or("version", 0));
    if (version != kFormatVersion) {
        return std::unexpected(std::format("{} was written by another version of this program (format {})", file.string(), version));
    }
    if (const std::string account = json->string_or("account_id", ""); !account.empty()) {
        store.account_id = parse_account_id(account);
        if (!store.account_id) return std::unexpected(std::format("{} is damaged: bad account ID", file.string()));
    }
    if (const Json* consoles = json->find("consoles"); consoles != nullptr && consoles->as_array() != nullptr) {
        for (const Json& entry : *consoles->as_array()) {
            auto console = from_json(entry);
            if (!console) return std::unexpected(std::format("{} is damaged: {}", file.string(), console.error()));
            store.consoles.push_back(std::move(*console));
        }
    }
    return store;
}

std::expected<void, std::string> save_store(const std::filesystem::path& file, const ConsoleStore& store) {
    Json::Array consoles;
    for (const StoredConsole& console : store.consoles) consoles.push_back(to_json(console));
    const Json document(Json::Object{
        {"version", Json(static_cast<std::int64_t>(kFormatVersion))},
        {"account_id", Json(store.account_id ? account_id_base64(*store.account_id) : std::string())},
        {"consoles", Json(std::move(consoles))},
    });
    const std::string text = document.dump();

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) return std::unexpected(std::format("cannot create {}: {}", file.parent_path().string(), error.message()));

    // Created readable by its owner alone, written and flushed, then moved into place.
    std::filesystem::path temporary = file;
    temporary += ".new";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return std::unexpected(std::format("cannot write {}: {}", temporary.string(), std::strerror(errno)));
    std::size_t written = 0;
    bool ok = ::fchmod(fd, 0600) == 0;
    while (ok && written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else written += static_cast<std::size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    const int saved_errno = errno;
    ::close(fd);
    if (!ok) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(std::format("cannot write {}: {}", temporary.string(), std::strerror(saved_errno)));
    }
    std::filesystem::rename(temporary, file, error);
    if (error) return std::unexpected(std::format("cannot replace {}: {}", file.string(), error.message()));
    return {};
}

}  // namespace psvp::app
