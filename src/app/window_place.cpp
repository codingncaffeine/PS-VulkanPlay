#include "app/window_place.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "app/json.h"
#include "app/paths.h"

namespace psvp::app {

std::optional<std::filesystem::path> window_place_file() {
    const auto dirs = resolve_dirs(process_env());
    if (!dirs) return std::nullopt;
    return dirs->state / "window.json";
}

WindowPlace load_window_place(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto json = Json::parse(text);
    if (!json) return {};
    const auto whole = [&](const char* name, int least) -> std::optional<int> {
        const Json* field = json->find(name);
        const double* number = field != nullptr ? field->as_number() : nullptr;
        if (number == nullptr || !(*number >= least) || !(*number <= WindowPlace::kMost) || *number != std::floor(*number)) return std::nullopt;
        return static_cast<int>(*number);
    };
    const auto flag = [&](const char* name) {
        const Json* field = json->find(name);
        const bool* value = field != nullptr ? field->as_bool() : nullptr;
        return value != nullptr && *value;
    };
    const auto width = whole("width", WindowPlace::kLeastWidth), height = whole("height", WindowPlace::kLeastHeight);
    // A size that makes no sense is not half taken: the whole file counts for nothing then.
    if (!width || !height) return {};
    WindowPlace place;
    place.width = *width;
    place.height = *height;
    place.maximized = flag("maximized");
    place.fullscreen = flag("fullscreen");
    return place;
}

bool save_window_place(const std::filesystem::path& file, const WindowPlace& place) {
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    const Json json(Json::Object{
        {"width", Json(static_cast<double>(place.width))},
        {"height", Json(static_cast<double>(place.height))},
        {"maximized", Json(place.maximized)},
        {"fullscreen", Json(place.fullscreen)},
    });
    std::filesystem::path temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << json.dump();
        out.flush();
        if (!out) {
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code failed;
    std::filesystem::rename(temporary, file, failed);
    if (failed) std::filesystem::remove(temporary, ignored);
    return !failed;
}

}  // namespace psvp::app
