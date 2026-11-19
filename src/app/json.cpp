#include "app/json.h"

#include <cerrno>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>

#include <locale.h>

namespace psvp::app {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::expected<Json, std::string> parse_document() {
        auto value = parse_value(0);
        if (!value) return value;
        skip_space();
        if (at_ != text_.size()) return fail("text after the value");
        return value;
    }

private:
    std::unexpected<std::string> fail(std::string_view what) const {
        return std::unexpected(std::format("{} at byte {}", what, at_));
    }

    void skip_space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
    }

    bool take(char c) {
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    bool take_word(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    std::expected<Json, std::string> parse_value(int depth) {
        if (depth > Json::kMaxDepth) return fail("nested too deeply");
        skip_space();
        if (at_ >= text_.size()) return fail("a value is missing");
        const char c = text_[at_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') {
            auto text = parse_string();
            if (!text) return std::unexpected(text.error());
            return Json(std::move(*text));
        }
        if (take_word("true")) return Json(true);
        if (take_word("false")) return Json(false);
        if (take_word("null")) return Json(nullptr);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return fail("not a value");
    }

    std::expected<Json, std::string> parse_object(int depth) {
        ++at_;
        Json::Object members;
        skip_space();
        if (take('}')) return Json(std::move(members));
        while (true) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') return fail("a member name is missing");
            auto name = parse_string();
            if (!name) return std::unexpected(name.error());
            skip_space();
            if (!take(':')) return fail("':' is missing after a member name");
            auto value = parse_value(depth + 1);
            if (!value) return value;
            members.emplace_back(std::move(*name), std::move(*value));
            skip_space();
            if (take('}')) return Json(std::move(members));
            if (!take(',')) return fail("',' or '}' is missing in an object");
        }
    }

    std::expected<Json, std::string> parse_array(int depth) {
        ++at_;
        Json::Array items;
        skip_space();
        if (take(']')) return Json(std::move(items));
        while (true) {
            auto value = parse_value(depth + 1);
            if (!value) return value;
            items.push_back(std::move(*value));
            skip_space();
            if (take(']')) return Json(std::move(items));
            if (!take(',')) return fail("',' or ']' is missing in an array");
        }
    }

    std::optional<unsigned> hex4() {
        if (at_ + 4 > text_.size()) return std::nullopt;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_ + static_cast<std::size_t>(i)];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
            value = value * 16 + digit;
        }
        at_ += 4;
        return value;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    std::expected<std::string, std::string> parse_string() {
        ++at_;  // the opening quote
        std::string out;
        while (true) {
            if (at_ >= text_.size()) return fail("a string does not end");
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return fail("a control character in a string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) return fail("a string does not end");
            const char escape = text_[at_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto code = hex4();
                    if (!code) return fail("a bad \\u escape");
                    if (*code >= 0xd800 && *code < 0xdc00) {
                        // The first half of a pair: the second must follow.
                        if (!take_word("\\u")) return fail("half of a surrogate pair");
                        const auto low = hex4();
                        if (!low || *low < 0xdc00 || *low > 0xdfff) return fail("half of a surrogate pair");
                        code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
                    } else if (*code >= 0xdc00 && *code <= 0xdfff) {
                        return fail("half of a surrogate pair");
                    }
                    append_utf8(out, *code);
                    break;
                }
                default: return fail("an unknown escape");
            }
        }
    }

    // The token is checked against the JSON grammar here; its value is then
    // converted exactly (to the nearest double), so that a number written out
    // again reads back as the same number.
    std::expected<Json, std::string> parse_number() {
        const std::size_t start = at_;
        const auto digits = [&]() {
            const std::size_t before = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            return at_ - before;
        };
        take('-');
        if (at_ < text_.size() && text_[at_] == '0' && at_ + 1 < text_.size() && text_[at_ + 1] >= '0' && text_[at_ + 1] <= '9') {
            return fail("a number with a leading zero");
        }
        if (digits() == 0) return fail("a number without digits");
        if (take('.') && digits() == 0) return fail("a number ending in '.'");
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) ++at_;
            if (digits() == 0) return fail("a number with an empty exponent");
        }
        const auto value = exact_double(text_.substr(start, at_ - start));
        if (!value) {
            at_ = start;
            return fail("a number out of range");
        }
        return Json(*value);
    }

    static std::optional<double> exact_double(std::string_view token) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        double value = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size()) return std::nullopt;
        return value;
#else
        // A standard library without the floating-point form of from_chars: the C
        // function, pinned to the "C" locale so that the decimal point is a point.
        static const locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
        const std::string copy(token);
        char* end = nullptr;
        errno = 0;
        const double value = ::strtod_l(copy.c_str(), &end, c_locale);
        if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return std::nullopt;
        return value;
#endif
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

void write_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else out.push_back(c);
        }
    }
    out.push_back('"');
}

}  // namespace

const Json* Json::find(std::string_view name) const {
    const Object* object = as_object();
    if (object == nullptr) return nullptr;
    for (const Member& member : *object) {
        if (member.first == name) return &member.second;
    }
    return nullptr;
}

std::string Json::string_or(std::string_view name, std::string_view fallback) const {
    const Json* member = find(name);
    const std::string* text = member != nullptr ? member->as_string() : nullptr;
    return text != nullptr ? *text : std::string(fallback);
}

double Json::number_or(std::string_view name, double fallback) const {
    const Json* member = find(name);
    const double* number = member != nullptr ? member->as_number() : nullptr;
    return number != nullptr ? *number : fallback;
}

void Json::write(std::string& out, int indent) const {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    if (is_null()) {
        out += "null";
    } else if (const bool* flag = as_bool()) {
        out += *flag ? "true" : "false";
    } else if (const double* number = as_number()) {
        const bool whole = std::isfinite(*number) && std::floor(*number) == *number && std::fabs(*number) < 9.0e15;
        if (whole) out += std::format("{}", static_cast<std::int64_t>(*number));
        else if (std::isfinite(*number)) out += std::format("{}", *number);
        else out += "null";
    } else if (const std::string* text = as_string()) {
        write_string(out, *text);
    } else if (const Array* items = as_array()) {
        if (items->empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < items->size(); ++i) {
            out += pad + "  ";
            (*items)[i].write(out, indent + 1);
            out += i + 1 < items->size() ? ",\n" : "\n";
        }
        out += pad + "]";
    } else if (const Object* members = as_object()) {
        if (members->empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        for (std::size_t i = 0; i < members->size(); ++i) {
            out += pad + "  ";
            write_string(out, (*members)[i].first);
            out += ": ";
            (*members)[i].second.write(out, indent + 1);
            out += i + 1 < members->size() ? ",\n" : "\n";
        }
        out += pad + "}";
    }
}

std::string Json::dump() const {
    std::string out;
    write(out, 0);
    out.push_back('\n');
    return out;
}

std::expected<Json, std::string> Json::parse(std::string_view text) { return Parser(text).parse_document(); }

}  // namespace psvp::app
