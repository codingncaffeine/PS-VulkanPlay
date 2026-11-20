#include "app/haptics_volume.h"

#ifdef PSVP_HAVE_PULSE

#include <algorithm>
#include <cctype>
#include <chrono>

#include <pulse/pulseaudio.h>

namespace psvp::app {

namespace {

bool contains(std::string_view text, std::string_view needle) {
    const auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(), same) != text.end();
}

struct Search {
    std::string_view needle;
    bool listed = false;
    bool found = false;
    std::uint32_t index = 0;
    pa_cvolume volume{};
    std::string description;
    bool set_done = false, set_ok = false;
};

void on_sink(pa_context*, const pa_sink_info* info, int end, void* userdata) {
    auto& search = *static_cast<Search*>(userdata);
    if (end != 0 || info == nullptr) {
        search.listed = true;
        return;
    }
    if (search.found || info->volume.channels < 4) return;
    const std::string_view name = info->name != nullptr ? info->name : "";
    const std::string_view description = info->description != nullptr ? info->description : "";
    if (!contains(name, search.needle) && !contains(description, search.needle)) return;
    search.found = true;
    search.index = info->index;
    search.volume = info->volume;
    search.description = description.empty() ? std::string(name) : std::string(description);
}

void on_set(pa_context*, int success, void* userdata) {
    auto& search = *static_cast<Search*>(userdata);
    search.set_done = true;
    search.set_ok = success != 0;
}

}  // namespace

HapticsVolume set_haptics_volume(std::string_view needle, int percent, bool only_look) {
    HapticsVolume result;
    pa_mainloop* loop = pa_mainloop_new();
    if (loop == nullptr) {
        result.problem = "the sound service could not be asked";
        return result;
    }
    pa_context* context = pa_context_new(pa_mainloop_get_api(loop), "PS VulkanPlay");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    // Runs the loop until `done` says so; false when the second is over or the connection broke.
    const auto run_until = [&](const auto& done) {
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (context != nullptr && !PA_CONTEXT_IS_GOOD(pa_context_get_state(context))) return false;
            if (pa_mainloop_prepare(loop, 50000) < 0 || pa_mainloop_poll(loop) < 0 || pa_mainloop_dispatch(loop) < 0) return false;
        }
        return true;
    };
    Search search;
    search.needle = needle;
    if (context == nullptr || pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0 ||
        !run_until([&] { return pa_context_get_state(context) == PA_CONTEXT_READY; })) {
        result.problem = "the desktop's sound service does not answer";
    } else {
        pa_operation* list = pa_context_get_sink_info_list(context, &on_sink, &search);
        const bool listed = list != nullptr && run_until([&] { return search.listed; });
        if (list != nullptr) pa_operation_unref(list);
        if (!listed) {
            result.problem = "the desktop's sound service did not list its devices";
        } else if (search.found) {
            result.found = true;
            result.device = search.description;
            // The channels of the device: its speaker's two, then the two actuators.
            const pa_volume_t lower = std::min(search.volume.values[2], search.volume.values[3]);
            result.before = static_cast<int>((static_cast<std::uint64_t>(lower) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
            const auto level = static_cast<pa_volume_t>(static_cast<std::uint64_t>(PA_VOLUME_NORM) * static_cast<unsigned>(std::clamp(percent, 0, 100)) / 100);
            if ((search.volume.values[2] != level || search.volume.values[3] != level) && !only_look) {
                pa_cvolume wanted = search.volume;
                wanted.values[2] = level;
                wanted.values[3] = level;
                pa_operation* set = pa_context_set_sink_volume_by_index(context, search.index, &wanted, &on_set, &search);
                const bool done = set != nullptr && run_until([&] { return search.set_done; });
                if (set != nullptr) pa_operation_unref(set);
                result.changed = done && search.set_ok;
                if (!result.changed) result.problem = "the desktop's sound service did not take the new volume";
            }
        }
    }
    if (context != nullptr) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    pa_mainloop_free(loop);
    return result;
}

}  // namespace psvp::app

#else

namespace psvp::app {

HapticsVolume set_haptics_volume(std::string_view, int, bool) {
    HapticsVolume result;
    result.problem = "this build cannot ask the desktop's sound service (it was made without libpulse)";
    return result;
}

}  // namespace psvp::app

#endif
