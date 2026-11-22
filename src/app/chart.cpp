#include "app/chart.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include <SDL3/SDL.h>

#include "app/log.h"
#include "media/test_chart.h"
#include "render/vk_window.h"

namespace psvp::app {

namespace {
constexpr std::chrono::seconds kNoticeShown{10};
constexpr int kStreamedPictures = 60;
}  // namespace

int run_chart(const ChartOptions& options) {
    auto chart = std::make_shared<media::TestChart>(media::make_test_chart(options.hdr));
    auto closing = std::make_shared<std::atomic<bool>>(false);
    log::info("chart: the {} test picture, {}x{}", options.hdr ? "HDR (PQ, BT.2020)" : "SDR (BT.709)", chart->width, chart->height);
    render::WindowOptions window;
    // The picture handed over as a stream would, sixty times a second, for its first second (an HDR
    // picture's measured peak settles over about 20 pictures, media/peak_detect.h); then it stays on
    // the screen and is drawn again only when the window changes.
    window.next_picture = [chart, handed = 0, next = std::chrono::steady_clock::time_point{}]() mutable -> std::optional<media::Picture> {
        const auto now = std::chrono::steady_clock::now();
        if (handed >= kStreamedPictures || now < next) return std::nullopt;
        next = now + std::chrono::microseconds(16667);
        ++handed;
        return chart->picture();
    };
    window.ask_every = std::chrono::milliseconds(4);
    // The notice for the first seconds only: then the whole picture can be seen.
    const auto opened = std::chrono::steady_clock::now();
    window.overlay_notice = [chart, opened] {
        return std::chrono::steady_clock::now() - opened < kNoticeShown ? chart->notice() : std::string();
    };
    window.on_event = [closing](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            closing->store(true);
            render::wake_window();
        }
    };
    window.finished = [closing] { return closing->load(); };
    window.on_screen_state = [](const std::string& line) { log::info("chart: the screen: {}", line); };
    if (options.hdr_peak_nits) window.hdr_peak_nits = *options.hdr_peak_nits;
    window.hdr_output = options.hdr_output;
    window.scaler = options.scaler;
    window.deband = options.deband;
    window.sharpen = options.sharpen;
    return render::run_window(window);
}

}  // namespace psvp::app
