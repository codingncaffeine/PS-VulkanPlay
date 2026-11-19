#include <map>
#include <string>

#include "app/paths.h"
#include "check.h"

namespace {

psvp::app::EnvLookup env_of(std::map<std::string, std::string, std::less<>> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    };
}

}  // namespace

PSVP_TEST(paths_follow_the_xdg_variables) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", "/home/someone"},
                                                      {"XDG_CONFIG_HOME", "/a/config"},
                                                      {"XDG_DATA_HOME", "/a/data"},
                                                      {"XDG_STATE_HOME", "/a/state"},
                                                      {"XDG_CACHE_HOME", "/a/cache"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/a/config/ps-vulkanplay");
    CHECK(dirs->data == "/a/data/ps-vulkanplay");
    CHECK(dirs->state == "/a/state/ps-vulkanplay");
    CHECK(dirs->cache == "/a/cache/ps-vulkanplay");
}

PSVP_TEST(paths_fall_back_to_the_home_folder) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", "/home/someone"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/home/someone/.config/ps-vulkanplay");
    CHECK(dirs->data == "/home/someone/.local/share/ps-vulkanplay");
    CHECK(dirs->state == "/home/someone/.local/state/ps-vulkanplay");
    CHECK(dirs->cache == "/home/someone/.cache/ps-vulkanplay");
}

// The specification: a relative or empty value is invalid and is ignored.
PSVP_TEST(paths_ignore_relative_and_empty_variables) {
    const auto dirs = psvp::app::resolve_dirs(
        env_of({{"HOME", "/home/someone"}, {"XDG_CONFIG_HOME", "relative/config"}, {"XDG_STATE_HOME", ""}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->config == "/home/someone/.config/ps-vulkanplay");
    CHECK(dirs->state == "/home/someone/.local/state/ps-vulkanplay");
}

// With no usable HOME the account's own home folder is used, or the call fails:
// in neither case does a relative path come back.
PSVP_TEST(paths_are_never_relative) {
    for (const char* home : {"", "relative/home", "."}) {
        const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", home}}));
        if (!dirs) {
            CHECK(!dirs.error().empty());
            continue;
        }
        CHECK(dirs->config.is_absolute());
        CHECK(dirs->data.is_absolute());
        CHECK(dirs->state.is_absolute());
        CHECK(dirs->cache.is_absolute());
        CHECK(dirs->config.filename() == "ps-vulkanplay");
    }
}

PSVP_TEST(paths_use_set_variables_even_without_a_home) {
    const auto dirs = psvp::app::resolve_dirs(env_of({{"HOME", ""},
                                                      {"XDG_CONFIG_HOME", "/b/config"},
                                                      {"XDG_DATA_HOME", "/b/data"},
                                                      {"XDG_STATE_HOME", "/b/state"},
                                                      {"XDG_CACHE_HOME", "/b/cache"}}));
    CHECK(dirs.has_value());
    if (!dirs) return;
    CHECK(dirs->state == "/b/state/ps-vulkanplay");
}
