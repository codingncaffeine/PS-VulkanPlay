#include "app/chiaki_import.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

#include "app/account_id.h"

namespace psvp::app {
namespace {

// The console types the other client writes in its "target" key.
constexpr long kTargetPs4Current = 1000;
constexpr long kTargetPs5Current = 1000100;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

// One unit of a decoded value: a byte taken from the file as it is, or the
// number an escape stood for.
struct Unit {
    unsigned value = 0;
    bool escaped = false;
};

std::vector<Unit> unescape(std::string_view raw) {
    std::vector<Unit> units;
    bool quoted = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c != '\\' || i + 1 >= raw.size()) {
            units.push_back(Unit{static_cast<unsigned char>(c), false});
            continue;
        }
        const char e = raw[++i];
        unsigned value = 0;
        switch (e) {
            case 'a': value = '\a'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'v': value = '\v'; break;
            case 'x': {
                // As many hex digits as follow; the writer never lets a literal digit follow one.
                while (i + 1 < raw.size() && hex_digit(raw[i + 1]) >= 0) {
                    value = (value << 4) | static_cast<unsigned>(hex_digit(raw[++i]));
                    value &= 0xffff;
                }
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    value = static_cast<unsigned>(e - '0');
                    while (i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7') {
                        value = ((value << 3) | static_cast<unsigned>(raw[++i] - '0')) & 0xffff;
                    }
                } else {
                    value = static_cast<unsigned char>(e);  // \\ \" \' \? and anything else: the character itself
                }
        }
        units.push_back(Unit{value, true});
    }
    return units;
}

struct Section {
    std::map<std::string, std::string, std::less<>> values;  // key -> raw value
};

std::map<std::string, Section, std::less<>> read_sections(std::string_view text) {
    std::map<std::string, Section, std::less<>> sections;
    std::string current = "General";
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            current = std::string(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        sections[current].values[std::string(trim(line.substr(0, equals)))] = std::string(trim(line.substr(equals + 1)));
    }
    return sections;
}

std::string string_until_nul(const std::vector<std::uint8_t>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    return std::string(bytes.begin(), end);
}

}  // namespace

IniValue decode_ini_value(std::string_view raw) {
    const std::vector<Unit> units = unescape(raw);
    IniValue value;
    for (const Unit& unit : units) {
        if (unit.escaped && unit.value >= 0x80) append_utf8(value.text, unit.value);
        else value.text.push_back(static_cast<char>(unit.value));
        value.bytes.push_back(static_cast<std::uint8_t>(unit.value & 0xff));
    }
    constexpr std::string_view marker = "@ByteArray(";
    if (value.text.starts_with(marker) && value.text.ends_with(")")) {
        value.is_byte_array = true;
        value.bytes.erase(value.bytes.begin(), value.bytes.begin() + static_cast<std::ptrdiff_t>(marker.size()));
        value.bytes.pop_back();
    } else if (value.text.starts_with("@@")) {
        value.text.erase(0, 1);
        value.bytes.erase(value.bytes.begin());
    }
    return value;
}

std::expected<ImportedConfig, std::string> parse_chiaki_config(std::string_view ini_text) {
    const auto sections = read_sections(ini_text);
    ImportedConfig config;

    if (const auto settings = sections.find("settings"); settings != sections.end()) {
        if (const auto id = settings->second.values.find("psn_account_id"); id != settings->second.values.end()) {
            config.account_id = parse_account_id(decode_ini_value(id->second).text);
        }
    }

    const auto hosts = sections.find("registered_hosts");
    if (hosts == sections.end()) return config;
    const auto& values = hosts->second.values;
    long count = 0;
    if (const auto size = values.find("size"); size != values.end()) {
        std::from_chars(size->second.data(), size->second.data() + size->second.size(), count);
    }
    if (count < 0 || count > 64) return std::unexpected(std::string("the list of registered consoles is damaged"));

    for (long index = 1; index <= count; ++index) {
        const auto field = [&](std::string_view name) -> std::optional<IniValue> {
            const auto found = values.find(std::format("{}\\{}", index, name));
            if (found == values.end()) return std::nullopt;
            return decode_ini_value(found->second);
        };
        const std::string label = field("server_nickname").value_or(IniValue{}).text;
        const auto skip = [&](std::string_view why) {
            config.skipped.push_back(std::format("{}: {}", label.empty() ? std::format("console {}", index) : label, why));
        };

        long target = 0;
        const std::string target_text = field("target").value_or(IniValue{}).text;
        std::from_chars(target_text.data(), target_text.data() + target_text.size(), target);
        proto::RegisteredConsole console;
        if (target == kTargetPs5Current) console.kind = proto::ConsoleKind::ps5;
        else if (target == kTargetPs4Current) console.kind = proto::ConsoleKind::ps4;
        else if (target > 0 && target < kTargetPs4Current) {
            skip("registered with PS4 system software older than 8.00, which is not supported");
            continue;
        } else {
            skip("a console type this program does not know");
            continue;
        }

        const auto mac = field("server_mac");
        const auto regist_key = field("rp_regist_key");
        const auto key = field("rp_key");
        if (!mac || mac->bytes.size() != console.mac.size() || !key || key->bytes.size() != console.key.size() || !regist_key) {
            skip("the registration is incomplete");
            continue;
        }
        std::copy(mac->bytes.begin(), mac->bytes.end(), console.mac.begin());
        std::copy(key->bytes.begin(), key->bytes.end(), console.key.begin());
        console.regist_key = string_until_nul(regist_key->bytes);
        if (console.regist_key.empty() || console.regist_key.size() > 16) {
            skip("the registration is incomplete");
            continue;
        }
        console.nickname = label;
        const std::string key_type = field("rp_key_type").value_or(IniValue{}).text;
        std::from_chars(key_type.data(), key_type.data() + key_type.size(), console.key_type);
        console.ap_ssid = field("ap_ssid").value_or(IniValue{}).text;
        console.ap_bssid = field("ap_bssid").value_or(IniValue{}).text;
        console.ap_key = field("ap_key").value_or(IniValue{}).text;
        console.ap_name = field("ap_name").value_or(IniValue{}).text;
        config.consoles.push_back(std::move(console));
    }
    return config;
}

std::vector<std::filesystem::path> chiaki_config_candidates(const EnvLookup& env) {
    std::vector<std::filesystem::path> candidates;
    const auto absolute = [&](std::string_view name) -> std::optional<std::filesystem::path> {
        const auto value = env(name);
        if (!value || value->empty() || !std::filesystem::path(*value).is_absolute()) return std::nullopt;
        return std::filesystem::path(*value);
    };
    const auto home = absolute("HOME");
    const auto config = absolute("XDG_CONFIG_HOME");
    if (config) candidates.push_back(*config / "Chiaki" / "Chiaki.conf");
    if (home) {
        if (!config || *config != *home / ".config") candidates.push_back(*home / ".config" / "Chiaki" / "Chiaki.conf");
        for (const char* app : {"io.github.streetpea.Chiaki4deck", "re.chiaki.Chiaki"}) {
            candidates.push_back(*home / ".var" / "app" / app / "config" / "Chiaki" / "Chiaki.conf");
        }
    }
    return candidates;
}

}  // namespace psvp::app
