// The consoles this client is registered with, and the account they were
// registered for. Kept in one file that only its owner can read: it holds the
// secrets shared with each console.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "proto/regist.h"

namespace psvp::app {

struct StoredConsole {
    proto::RegisteredConsole registration;
    std::string last_address;  // where it last answered, as dotted IPv4; may be empty
};

struct ConsoleStore {
    std::optional<proto::AccountId> account_id;
    std::vector<StoredConsole> consoles;

    // By nickname (ignoring case) or by hardware address (twelve hex digits, with or without colons).
    [[nodiscard]] StoredConsole* find(std::string_view name_or_mac);
    [[nodiscard]] StoredConsole* find_mac(const std::array<std::uint8_t, 6>& mac);
    // Adds the console, or replaces the entry with the same hardware address.
    void put(StoredConsole console);
};

[[nodiscard]] std::filesystem::path store_path(const std::filesystem::path& config_dir);

// A file that does not exist is an empty store, not an error.
[[nodiscard]] std::expected<ConsoleStore, std::string> load_store(const std::filesystem::path& file);

// Written beside the file and moved over it, so a crash never leaves half a file.
[[nodiscard]] std::expected<void, std::string> save_store(const std::filesystem::path& file, const ConsoleStore& store);

}  // namespace psvp::app
