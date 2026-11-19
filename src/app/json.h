// A small JSON reader and writer for the application's own files.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace psvp::app {

class Json {
public:
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;  // name and value
    using Object = std::vector<Member>;           // in the order written

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(std::int64_t value) : value_(static_cast<double>(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(value_); }
    [[nodiscard]] const bool* as_bool() const { return std::get_if<bool>(&value_); }
    [[nodiscard]] const double* as_number() const { return std::get_if<double>(&value_); }
    [[nodiscard]] const std::string* as_string() const { return std::get_if<std::string>(&value_); }
    [[nodiscard]] const Array* as_array() const { return std::get_if<Array>(&value_); }
    [[nodiscard]] const Object* as_object() const { return std::get_if<Object>(&value_); }

    // A member of an object, or nothing when this is not an object or has no such member.
    [[nodiscard]] const Json* find(std::string_view name) const;
    // Shortcuts for the common "member of this type, or a fallback".
    [[nodiscard]] std::string string_or(std::string_view name, std::string_view fallback) const;
    [[nodiscard]] double number_or(std::string_view name, double fallback) const;

    // Indented with two spaces, members in the order they were added, a newline at the end.
    [[nodiscard]] std::string dump() const;

    // The error names what was wrong and where. Nesting deeper than kMaxDepth is refused.
    [[nodiscard]] static std::expected<Json, std::string> parse(std::string_view text);
    static constexpr int kMaxDepth = 32;

private:
    void write(std::string& out, int indent) const;
    std::variant<std::monostate, bool, double, std::string, Array, Object> value_;
};

}  // namespace psvp::app
