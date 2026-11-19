#include "proto/text_message.h"

namespace psvp::proto {
namespace {

constexpr std::string_view kStatusPrefix = "HTTP/1.1 ";

std::string_view until_nul(std::string_view text) {
    const std::size_t nul = text.find('\0');
    return nul == std::string_view::npos ? text : text.substr(0, nul);
}

std::expected<void, TextError> parse_lines(std::string_view text, TextMessage& message) {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find_first_of("\r\n", at);
        // A last line with no line ending may have been cut off in transit: it is not read.
        if (end == std::string_view::npos) break;
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return std::unexpected(TextError::bad_header);
        std::string_view value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (message.headers.size() == kMaxTextHeaders) return std::unexpected(TextError::too_many_headers);
        message.headers.push_back(TextHeader{std::string(line.substr(0, colon)), std::string(value)});
    }
    return {};
}

}  // namespace

std::optional<std::string_view> TextMessage::find(std::string_view name) const {
    for (const TextHeader& header : headers) {
        if (header.name == name) return std::string_view(header.value);
    }
    return std::nullopt;
}

std::string_view to_string(TextError error) noexcept {
    switch (error) {
        case TextError::no_status_line: return "no status line";
        case TextError::bad_status: return "status code is not a number";
        case TextError::bad_header: return "a line is not a header";
        case TextError::too_many_headers: return "too many headers";
    }
    return "unknown error";
}

std::expected<TextMessage, TextError> parse_header_block(std::string_view text) {
    TextMessage message;
    if (auto parsed = parse_lines(until_nul(text), message); !parsed) return std::unexpected(parsed.error());
    return message;
}

std::expected<TextMessage, TextError> parse_status_message(std::string_view text) {
    text = until_nul(text);
    if (!text.starts_with(kStatusPrefix)) return std::unexpected(TextError::no_status_line);
    text.remove_prefix(kStatusPrefix.size());

    int status = 0;
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' && digits < 4) {
        status = status * 10 + (text[digits] - '0');
        ++digits;
    }
    if (digits == 0 || digits > 3 || status == 0) return std::unexpected(TextError::bad_status);

    TextMessage message;
    message.status = status;
    const std::size_t line_end = text.find('\n');
    if (line_end == std::string_view::npos) return message;
    if (auto parsed = parse_lines(text.substr(line_end + 1), message); !parsed) return std::unexpected(parsed.error());
    return message;
}

}  // namespace psvp::proto
