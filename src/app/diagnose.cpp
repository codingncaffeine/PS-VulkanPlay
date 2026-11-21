#include "app/diagnose.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "app/json.h"
#include "app/paths.h"
#include "media/gpu_device.h"
#include "media/gpu_share.h"
#include "media/video_decoder.h"
#include "psvp/version.h"
#include "render/video_device.h"

namespace psvp::app {
namespace {

std::string version_text(std::uint32_t version) {
    return std::format("{}.{}.{}", (version >> 22) & 0x7f, (version >> 12) & 0x3ff, version & 0xfff);
}

const char* kind_text(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
    default: return "other";
    }
}

int rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

struct Device {
    std::string name, kind, vulkan;
};

struct Facts {
    std::string loader;
    std::vector<Device> devices;
    std::string decoding_device;  // the device the stream window would use, as it decodes
    std::string decoding_why_not;
    std::string vulkan_error;
    std::vector<std::string> last_session;
    std::string last_session_started;
    std::string logs;
};

// The graphics devices, and whether the one the window picks (the best kind that draws) decodes.
void look_at_vulkan(Facts& facts) {
    std::uint32_t loader = (1U << 22);
    vkEnumerateInstanceVersion(&loader);
    facts.loader = version_text(loader);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = PSVP_NAME;
    const std::uint32_t api = loader >= render::kVideoApi ? render::kVideoApi : (1U << 22) | (2U << 12);
    app.apiVersion = api;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        facts.vulkan_error = "Vulkan could not be started: no Vulkan driver is installed for a graphics card here";
        return;
    }
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    std::optional<std::pair<VkPhysicalDevice, std::uint32_t>> best;
    int best_rank = -1;
    VkPhysicalDeviceProperties best_properties{};
    for (const VkPhysicalDevice physical : physicals) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        facts.devices.push_back({properties.deviceName, kind_text(properties.deviceType), version_text(properties.apiVersion)});
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            if (rank(properties.deviceType) > best_rank) {
                best_rank = rank(properties.deviceType);
                best = {physical, i};
                best_properties = properties;
            }
            break;
        }
    }
    if (!best) {
        facts.decoding_why_not = "no graphics device draws";
    } else {
        // The device made as the window makes it, then lent to FFmpeg as the window lends it.
        auto made = render::make_device(render::DeviceRequest{instance, api, {}, best->first, best_properties.apiVersion, best->second,
                                                              best_properties.deviceName, true, {}});
        if (!made) {
            facts.decoding_why_not = made.error();
        } else {
            if (made->why_not.empty()) {
                auto shared = media::share_device(made->share);
                if (shared) facts.decoding_device = shared->description();
                else facts.decoding_why_not = shared.error();
            } else {
                facts.decoding_why_not = made->why_not;
            }
            vkDestroyDevice(made->device, nullptr);
        }
    }
    vkDestroyInstance(instance, nullptr);
}

// The report of the last session, from the log `play` writes: what it said at its end.
void look_at_last_session(Facts& facts) {
    const auto dirs = resolve_dirs(process_env());
    if (!dirs) return;
    facts.logs = (dirs->state / "logs").string();
    std::ifstream log(dirs->state / "logs" / "play.log");
    if (!log) return;
    std::string line, started;
    std::vector<std::string> said;
    // A line: the time, a space, the level padded to five, a space, the text.
    while (std::getline(log, line)) {
        if (line.size() < 32 || line[4] != '-' || line[10] != 'T') continue;
        const std::string level = line.substr(25, 5), text = line.substr(31);
        if (level == "INFO " && text.starts_with(PSVP_BINARY " ") && text.find(": play ") != std::string::npos) {
            started = line.substr(0, 19);
            said.clear();
        } else if (level == "SAID ") {
            said.push_back(text);
        }
    }
    facts.last_session_started = started;
    facts.last_session = std::move(said);
}

std::string one_line(const Json& json) {
    const std::string spread = json.dump();
    std::string out;
    for (std::size_t i = 0; i < spread.size(); ++i) {
        if (spread[i] != '\n') {
            out += spread[i];
            continue;
        }
        while (i + 1 < spread.size() && spread[i + 1] == ' ') ++i;
    }
    return out;
}

}  // namespace

int run_diagnose(bool json) {
    Facts facts;
    look_at_vulkan(facts);
    look_at_last_session(facts);
    const int sdl = SDL_GetVersion();
    const std::string sdl_text = std::format("{}.{}.{}", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl));
    if (json) {
        Json::Array devices, session;
        for (const Device& device : facts.devices) {
            devices.push_back(Json(Json::Object{{"name", Json(device.name)}, {"kind", Json(device.kind)}, {"vulkan", Json(device.vulkan)}}));
        }
        for (const std::string& line : facts.last_session) session.push_back(Json(line));
        const Json out(Json::Object{
            {"version", Json(PSVP_VERSION)},
            {"vulkan_loader", Json(facts.loader)},
            {"vulkan_error", Json(facts.vulkan_error)},
            {"devices", Json(std::move(devices))},
            {"gpu_decoding", Json(facts.decoding_device)},
            {"gpu_decoding_why_not", Json(facts.decoding_why_not)},
            {"ffmpeg", Json(media::ffmpeg_version())},
            {"sdl", Json(sdl_text)},
            {"last_session_started", Json(facts.last_session_started)},
            {"last_session", Json(std::move(session))},
            {"logs_folder", Json(facts.logs)},
        });
        std::printf("%s\n", one_line(out).c_str());
    } else {
        std::printf("%s %s; FFmpeg %s; SDL %s; Vulkan loader %s\n", PSVP_BINARY, PSVP_VERSION, media::ffmpeg_version().c_str(), sdl_text.c_str(), facts.loader.c_str());
        if (!facts.vulkan_error.empty()) std::printf("%s.\n", facts.vulkan_error.c_str());
        for (const Device& device : facts.devices) std::printf("Graphics device: %s (%s), Vulkan %s\n", device.name.c_str(), device.kind.c_str(), device.vulkan.c_str());
        if (!facts.decoding_device.empty()) std::printf("Pictures are decoded on %s, checked against software at the start of each session.\n", facts.decoding_device.c_str());
        else std::printf("Pictures are decoded in software: %s.\n", facts.decoding_why_not.c_str());
        if (facts.last_session_started.empty()) {
            std::printf("No session was played yet.\n");
        } else {
            std::printf("The last session (started %s UTC) ended with:\n", facts.last_session_started.c_str());
            for (const std::string& line : facts.last_session) std::printf("  %s\n", line.c_str());
        }
    }
    return facts.vulkan_error.empty() ? 0 : 1;
}

}  // namespace psvp::app
