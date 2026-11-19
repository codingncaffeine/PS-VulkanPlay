// Reading the registrations another Remote Play client already made, so that
// nobody has to register a console twice. Chiaki and chiaki-ng keep them in
// Chiaki.conf, in the INI format of the Qt settings class. The file is only read.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/paths.h"
#include "proto/regist.h"

namespace psvp::app {

struct ImportedConfig {
    std::optional<proto::AccountId> account_id;
    std::vector<proto::RegisteredConsole> consoles;
    std::vector<std::string> skipped;  // one sentence per entry that could not be used, and why
};

[[nodiscard]] std::expected<ImportedConfig, std::string> parse_chiaki_config(std::string_view ini_text);

// Where such a file may be: the native location and the Flatpak ones.
[[nodiscard]] std::vector<std::filesystem::path> chiaki_config_candidates(const EnvLookup& env);

// One value of the INI format, with its escapes undone. Exposed for the tests.
struct IniValue {
    std::string text;                 // read as text (UTF-8)
    std::vector<std::uint8_t> bytes;  // read as bytes: the content of @ByteArray(...), else the text's bytes
    bool is_byte_array = false;
};
[[nodiscard]] IniValue decode_ini_value(std::string_view raw);

}  // namespace psvp::app
