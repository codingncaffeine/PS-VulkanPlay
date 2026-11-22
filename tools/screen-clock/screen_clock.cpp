// The lab's clock on the screen (PLAN I3): what a virtual screen shows, and when, read from
// outside the program that draws it; and the score of that against what the console stand-in
// sent.
//
//   psvp_screen_clock watch SCREEN --clip WIDTHxHEIGHT --seconds N --out FILE [--ready FILE]
//     SCREEN is the screen of a virtual X server kept in a file (Xvfb -fbdir DIR: DIR/Xvfb_screen0).
//     Finds the generated clip on it by its color patches, then reads the frame number the clip
//     shows and its mark (src/media/test_clip.h) every quarter of a millisecond, for N seconds or
//     until it is stopped. Writes one line per change, "seen COUNTER NS MARK DAMAGED", NS on the
//     steady clock. DAMAGED is 1 when the picture is not the frame its number says: the clip has one
//     white block that moves on by a place with every frame, and a picture built on a frame that
//     never arrived has it twice, or in the wrong place. --ready names a file made once the clip was found.
//
//   psvp_screen_clock score FILE...
//     Reads the stand-in's --timing-log, the watcher's lines and the client's log (any order) and
//     prints the score as "Lab:" lines. Exit 0 when the score is whole, 1 when something in it
//     cannot be trusted (said in a "PROBLEM" line), 2 when nothing could be scored.
//
// A reading counts only when the clip's first row and its second, which shows the same bits the
// other way round, agree, and a change only when the next reading, a quarter of a millisecond later,
// finds the same: a screen caught while it is being written is not read. A change is stamped with the
// time of its first reading.
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "media/test_clip.h"
#include "timing.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::int64_t steady_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return std::int64_t{now.tv_sec} * 1'000'000'000 + now.tv_nsec;
}

struct Channel {
    std::uint32_t mask = 0;
    unsigned shift = 0;
    std::uint32_t max = 0;
};

Channel channel_of(std::uint32_t mask) {
    Channel channel;
    channel.mask = mask;
    if (mask == 0) return channel;
    while (((mask >> channel.shift) & 1U) == 0) ++channel.shift;
    channel.max = mask >> channel.shift;
    return channel;
}

// A screen kept in a file in XWD form by the X server, mapped so that what the server writes is
// seen at once.
class Screen {
public:
    bool open(const char* path, std::string& why) {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            why = std::string("cannot be opened: ") + std::strerror(errno);
            return false;
        }
        struct stat info{};
        if (::fstat(fd, &info) != 0 || info.st_size < 100) {
            ::close(fd);
            why = "too short for an XWD header";
            return false;
        }
        size_ = static_cast<std::size_t>(info.st_size);
        void* map = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) {
            why = std::string("cannot be mapped: ") + std::strerror(errno);
            return false;
        }
        data_ = static_cast<const unsigned char*>(map);
        // The header: 25 big-endian 32-bit words, whatever the machine.
        const std::uint32_t header_size = word(0), color_count = word(19);
        if (word(1) != 7 || word(2) != 2) {
            why = "not an XWD version 7 ZPixmap";
            return false;
        }
        width_ = word(4);
        height_ = word(5);
        lsb_first_ = word(7) == 0;
        const std::uint32_t bits = word(11);
        stride_ = word(12);
        red_ = channel_of(word(14));
        green_ = channel_of(word(15));
        blue_ = channel_of(word(16));
        if (bits != 32 && bits != 24) {
            why = "not a 24 or 32 bit screen";
            return false;
        }
        bytes_ = bits / 8;
        pixels_ = std::size_t{header_size} + std::size_t{color_count} * 12;
        if (width_ == 0 || height_ == 0 || stride_ < width_ * bytes_ || size_ < pixels_ + std::size_t{stride_} * height_) {
            why = "shorter than its header says";
            return false;
        }
        return true;
    }

    [[nodiscard]] unsigned width() const noexcept { return width_; }
    [[nodiscard]] unsigned height() const noexcept { return height_; }

    struct Rgb {
        int r = 0, g = 0, b = 0;
    };
    [[nodiscard]] Rgb at(unsigned x, unsigned y) const noexcept {
        const std::size_t place = pixels_ + std::size_t{y} * stride_ + std::size_t{x} * bytes_;
        std::uint32_t pixel = 0;
        for (std::size_t i = 0; i < bytes_; ++i) {
            const std::size_t shift = lsb_first_ ? i * 8 : (bytes_ - 1 - i) * 8;
            pixel |= std::uint32_t{data_[place + i]} << shift;
        }
        return {scaled(pixel, red_), scaled(pixel, green_), scaled(pixel, blue_)};
    }

private:
    [[nodiscard]] std::uint32_t word(std::size_t index) const noexcept {
        const std::size_t at = index * 4;
        return (std::uint32_t{data_[at]} << 24) | (std::uint32_t{data_[at + 1]} << 16) | (std::uint32_t{data_[at + 2]} << 8) | std::uint32_t{data_[at + 3]};
    }
    static int scaled(std::uint32_t pixel, const Channel& channel) noexcept {
        return channel.max == 0 ? 0 : static_cast<int>(((pixel & channel.mask) >> channel.shift) * 255U / channel.max);
    }

    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0, pixels_ = 0, bytes_ = 4;
    unsigned width_ = 0, height_ = 0, stride_ = 0;
    bool lsb_first_ = true;
    Channel red_, green_, blue_;
};

// A block of the clip as BT.709 in the limited range turns it into 8-bit RGB.
Screen::Rgb rgb_of(const psvp::media::ClipBlock& block) {
    const double y = 1.164383 * (block.y - 16.0), cb = block.cb - 128.0, cr = block.cr - 128.0;
    const auto clamp = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 255.0))); };
    return {clamp(y + 1.792741 * cr), clamp(y - 0.213249 * cb - 0.532909 * cr), clamp(y + 2.112402 * cb)};
}

// Where the clip is drawn: its top left corner and how many screen pixels one of its samples takes.
struct Placement {
    double x = 0, y = 0, scale = 1;
};

// Finds the clip by the middles of two of its patches (row 2, columns 1 and 6: yellow and blue,
// colors nothing else of it has).
std::optional<Placement> find_clip(const Screen& screen) {
    const unsigned columns = 16, rows = 4;
    const Screen::Rgb want[2] = {rgb_of(psvp::media::clip_block(0, 1, 2, columns, rows)), rgb_of(psvp::media::clip_block(0, 6, 2, columns, rows))};
    double sum_x[2] = {}, sum_y[2] = {};
    std::size_t count[2] = {};
    for (unsigned y = 0; y < screen.height(); ++y) {
        for (unsigned x = 0; x < screen.width(); ++x) {
            const Screen::Rgb got = screen.at(x, y);
            for (int i = 0; i < 2; ++i) {
                if (std::abs(got.r - want[i].r) <= 8 && std::abs(got.g - want[i].g) <= 8 && std::abs(got.b - want[i].b) <= 8) {
                    sum_x[i] += x;
                    sum_y[i] += y;
                    ++count[i];
                }
            }
        }
    }
    if (count[0] < 16 || count[1] < 16) return std::nullopt;
    const double yellow_x = sum_x[0] / static_cast<double>(count[0]), yellow_y = sum_y[0] / static_cast<double>(count[0]);
    const double blue_x = sum_x[1] / static_cast<double>(count[1]), blue_y = sum_y[1] / static_cast<double>(count[1]);
    const double scale = (blue_x - yellow_x) / (5.0 * psvp::media::kClipBlock);
    if (scale < 0.25 || std::abs(blue_y - yellow_y) > 2.0) return std::nullopt;
    // Patch 1's middle is 1.5 blocks right of the clip's left edge and 2.5 blocks down.
    return Placement{yellow_x - 1.5 * psvp::media::kClipBlock * scale, yellow_y - 2.5 * psvp::media::kClipBlock * scale, scale};
}

int usage() {
    std::fputs("usage: psvp_screen_clock watch SCREEN --clip WIDTHxHEIGHT --seconds N --out FILE [--ready FILE]\n"
               "       psvp_screen_clock score FILE...\n",
               stderr);
    return 2;
}

int watch(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* path = argv[2];
    unsigned clip_width = 0, clip_height = 0;
    double seconds = 0;
    std::string out_path, ready_path;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string_view arg = argv[i], value = argv[i + 1];
        if (arg == "--clip") {
            const std::size_t by = value.find('x');
            if (by == std::string_view::npos || std::from_chars(value.data(), value.data() + by, clip_width).ec != std::errc{} ||
                std::from_chars(value.data() + by + 1, value.data() + value.size(), clip_height).ec != std::errc{}) {
                return usage();
            }
        } else if (arg == "--seconds") {
            seconds = std::atof(argv[i + 1]);
        } else if (arg == "--out") {
            out_path = value;
        } else if (arg == "--ready") {
            ready_path = value;
        } else {
            return usage();
        }
    }
    if (clip_width < psvp::media::kClipMinWidth || clip_height < psvp::media::kClipMinHeight || seconds <= 0 || out_path.empty()) return usage();

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    const std::int64_t end = steady_ns() + static_cast<std::int64_t>(seconds * 1e9);

    // The screen file appears when the X server starts, the clip when the first picture is drawn.
    Screen screen;
    std::optional<Placement> placement;
    std::string why;
    while (g_stop == 0 && steady_ns() < end) {
        if (screen.width() == 0 && !screen.open(path, why)) screen = Screen{};
        if (screen.width() != 0) placement = find_clip(screen);
        if (placement) break;
        ::usleep(100'000);
    }
    if (!placement) {
        std::fprintf(stderr, "screen clock: the clip was never found on %s%s%s\n", path, why.empty() ? "" : ": ", why.c_str());
        return 2;
    }
    const unsigned columns = (clip_width + psvp::media::kClipBlock - 1) / psvp::media::kClipBlock;
    const auto screen_x = [&](double clip_x) { return static_cast<unsigned>(std::lround(placement->x + clip_x * placement->scale)); };
    const auto screen_y = [&](double clip_y) { return static_cast<unsigned>(std::lround(placement->y + clip_y * placement->scale)); };
    // Where each of the counter's bits is read in both rows, and the mark: the middle of what is
    // shown of the last block of row 2 (the clip may end inside it).
    std::vector<unsigned> bit_x;
    for (unsigned column = 0; column < psvp::media::kClipCounterBits; ++column) bit_x.push_back(screen_x(column * 16.0 + 8.0));
    const unsigned row0_y = screen_y(8.0), row1_y = screen_y(24.0);
    const unsigned mark_x = screen_x(((columns - 1) * 16.0 + clip_width) / 2.0), mark_y = screen_y(psvp::media::kClipMarkRow * 16.0 + 8.0);
    // The places of the moving white block (rows 3 on), as test_clip.cpp counts them, each read in its middle.
    const unsigned rows = (clip_height + psvp::media::kClipBlock - 1) / psvp::media::kClipBlock;
    std::vector<std::pair<unsigned, unsigned>> places;
    for (unsigned row = 3; row < rows; ++row) {
        for (unsigned column = 0; column < columns; ++column) {
            places.emplace_back(screen_x(column * 16.0 + 8.0), screen_y(row * 16.0 + 8.0));
        }
    }
    // A place whose middle the clip cuts off is never read: it counts as gray.
    const auto inside = [&](std::size_t i) {
        return (i % columns) * 16 + 8 < clip_width && (3 + i / columns) * 16 + 8 < clip_height;
    };
    if (mark_x >= screen.width() || mark_y >= screen.height() || bit_x.back() >= screen.width()) {
        std::fprintf(stderr, "screen clock: the clip found does not fit the screen\n");
        return 2;
    }
    std::fprintf(stderr, "screen clock: the clip is at %.1f,%.1f, %.3f screen pixels to a sample\n", placement->x, placement->y, placement->scale);
    if (!ready_path.empty()) std::ofstream(ready_path) << "found\n";

    const auto light = [&](unsigned x, unsigned y) {
        const Screen::Rgb p = screen.at(x, y);
        return p.r + p.g + p.b;
    };
    std::vector<psvp::lab::Seen> seen;
    seen.reserve(1 << 16);
    struct Key {
        std::uint16_t counter = 0;
        bool mark = false;
        std::size_t white = 0, white_at = 0;
        bool operator==(const Key&) const = default;
    };
    std::optional<Key> last;
    std::optional<std::pair<Key, std::int64_t>> pending;  // a change read once, and when
    std::uint64_t readings = 0, torn = 0, half_drawn = 0;
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_stop == 0) {
        next.tv_nsec += 250'000;
        if (next.tv_nsec >= 1'000'000'000) {
            next.tv_nsec -= 1'000'000'000;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        const std::int64_t now = steady_ns();
        if (now >= end) break;
        ++readings;
        std::uint16_t counter = 0;
        bool whole = true;
        for (const unsigned x : bit_x) {
            const bool first = light(x, row0_y) >= 3 * 128, second = light(x, row1_y) >= 3 * 128;
            if (first == second) {
                whole = false;
                break;
            }
            counter = static_cast<std::uint16_t>((static_cast<unsigned>(counter) << 1U) | (first ? 1U : 0U));
        }
        if (!whole) {
            ++torn;
            continue;
        }
        // White against the clip's gray (64: about 56 in RGB): halfway is about 155.
        const bool mark = light(mark_x, mark_y) >= 3 * 155;
        Key key{counter, mark};
        if (!places.empty()) {
            for (std::size_t i = 0; i < places.size(); ++i) {
                if (!inside(i) || light(places[i].first, places[i].second) < 3 * 155) continue;
                if (key.white++ == 0) key.white_at = i;
            }
        }
        if (last && *last == key) {
            pending.reset();
            continue;
        }
        if (!pending || !(pending->first == key)) {
            pending = std::pair(key, now);
            continue;
        }
        const std::int64_t first_read = pending->second;
        pending.reset();
        last = key;
        const std::size_t expected = places.empty() ? 0 : static_cast<std::size_t>(counter) % places.size();
        const bool damaged = !places.empty() && (inside(expected) ? key.white != 1 || key.white_at != expected : key.white != 0);
        // A damaged reading that then settles, whole, under the same number: the virtual screen was read
        // while it was still being written (its rows are copied top to bottom; the number's rows lie
        // above the patches), seen 2026-11-22 under load as "damaged" pictures of 0.5 to 0.8 ms followed
        // by the same frame whole. It is the one picture: its first reading stays, what it shows is what
        // it settled on. (Not the other way round: a picture built on a lost frame can keep the number
        // of the frame before it, whole first and damaged after.)
        if (!seen.empty() && seen.back().counter == counter && seen.back().damaged && !damaged) {
            seen.back().mark = mark;
            seen.back().damaged = damaged;
            ++half_drawn;
            continue;
        }
        seen.push_back({counter, first_read, mark, damaged});
    }

    std::ofstream out(out_path, std::ios::trunc);
    for (const auto& one : seen) out << "seen " << one.counter << ' ' << one.ns << ' ' << (one.mark ? 1 : 0) << ' ' << (one.damaged ? 1 : 0) << '\n';
    out.flush();
    std::fprintf(stderr, "screen clock: %zu changes in %llu readings (%llu not whole, %llu pictures read before they were whole)\n", seen.size(),
                 static_cast<unsigned long long>(readings), static_cast<unsigned long long>(torn), static_cast<unsigned long long>(half_drawn));
    return out ? 0 : 2;
}

int score(int argc, char** argv) {
    if (argc < 3) return usage();
    psvp::lab::Timing timing;
    for (int i = 2; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "screen clock: %s cannot be read\n", argv[i]);
            return 2;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        psvp::lab::read_lines(text, timing);
    }
    const auto result = psvp::lab::score(timing);
    std::fputs(psvp::lab::describe(result).c_str(), stdout);
    if (result.reached == 0) return 2;
    return result.problems.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "watch") return watch(argc, argv);
    if (argc >= 2 && std::string_view(argv[1]) == "score") return score(argc, argv);
    return usage();
}
