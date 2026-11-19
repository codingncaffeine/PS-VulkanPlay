// The line-oriented text the protocol borrows from HTTP: an optional status
// line, then "name:value" lines. Discovery replies, registration responses and
// the decrypted bodies inside them are all written this way.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psvp::proto {

struct TextHeader {
    std::string name;
    std::string value;
};

struct TextMessage {
    int status = 0;  // 0 for a bare block of headers
    std::vector<TextHeader> headers;  // in the order they were written

    // The first header with exactly this name (names are case-sensitive on the wire).
    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const;
};

enum class TextError {
    no_status_line,    // does not begin with "HTTP/1.1 "
    bad_status,        // the status code is not a number above zero
    bad_header,        // a line with no colon, or with nothing before it
    too_many_headers,  // more than kMaxTextHeaders lines
};

inline constexpr std::size_t kMaxTextHeaders = 64;

[[nodiscard]] std::string_view to_string(TextError error) noexcept;

// Lines end with "\n" or "\r\n"; a last line without an ending is not read,
// since it may have been cut off. Reading stops at the first NUL byte (datagrams
// carry one after the text). One space after the colon is not part of the value.
[[nodiscard]] std::expected<TextMessage, TextError> parse_header_block(std::string_view text);

// "HTTP/1.1 <code> <words>" on the first line, then headers.
[[nodiscard]] std::expected<TextMessage, TextError> parse_status_message(std::string_view text);

}  // namespace psvp::proto
