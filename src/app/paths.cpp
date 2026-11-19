#include "app/paths.h"

#include <cstdlib>
#include <vector>

#include <pwd.h>
#include <unistd.h>

#include "psvp/version.h"

namespace psvp::app {
namespace {

std::optional<std::filesystem::path> absolute_from(const EnvLookup& env, std::string_view name) {
    auto value = env(name);
    if (!value || value->empty()) return std::nullopt;
    std::filesystem::path path(*value);
    if (!path.is_absolute()) return std::nullopt;
    return path;
}

std::optional<std::filesystem::path> home_from_account() {
    long hint = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(hint > 0 ? static_cast<std::size_t>(hint) : std::size_t{4096});
    struct passwd entry {};
    struct passwd* found = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr) {
        return std::nullopt;
    }
    if (found->pw_dir == nullptr) return std::nullopt;
    std::filesystem::path path(found->pw_dir);
    if (!path.is_absolute()) return std::nullopt;
    return path;
}

}  // namespace

EnvLookup process_env() {
    return [](std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        const char* value = std::getenv(key.c_str());
        if (value == nullptr) return std::nullopt;
        return std::string(value);
    };
}

std::expected<Dirs, std::string> resolve_dirs(const EnvLookup& env) {
    std::optional<std::filesystem::path> home = absolute_from(env, "HOME");
    if (!home) home = home_from_account();

    const auto pick = [&](std::string_view variable,
                          std::string_view fallback) -> std::optional<std::filesystem::path> {
        if (auto set = absolute_from(env, variable)) return *set / PSVP_BINARY;
        if (home) return *home / fallback / PSVP_BINARY;
        return std::nullopt;
    };

    const auto config = pick("XDG_CONFIG_HOME", ".config");
    const auto data = pick("XDG_DATA_HOME", ".local/share");
    const auto state = pick("XDG_STATE_HOME", ".local/state");
    const auto cache = pick("XDG_CACHE_HOME", ".cache");
    if (!config || !data || !state || !cache) {
        return std::unexpected(std::string("no home folder: HOME is not an absolute path and the account has none"));
    }
    return Dirs{*config, *data, *state, *cache};
}

}  // namespace psvp::app
