// Reads the screen of a virtual X server and counts the pixels of one color.
//
//   Xvfb ... -fbdir DIR      keeps the screen in DIR/Xvfb_screen0, in XWD format
//   psvp_fbcheck DIR/Xvfb_screen0 R G B [TOLERANCE] [MIN_PERCENT]
//
// Exit 0 when at least MIN_PERCENT (default 50) of the screen is the color
// within TOLERANCE (default 2) per channel, 1 when it is not, 2 when the file
// cannot be read. A window check uses it to see what is on the screen instead
// of trusting the program's own word that it drew.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

constexpr std::size_t kHeaderWords = 25;

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

int scaled(std::uint32_t pixel, const Channel& channel) {
    if (channel.max == 0) return 0;
    return static_cast<int>(((pixel & channel.mask) >> channel.shift) * 255U / channel.max);
}

int fail(const char* file, const char* why) {
    std::fprintf(stderr, "fbcheck: %s: %s\n", file, why);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr, "usage: %s XWD_FILE R G B [TOLERANCE] [MIN_PERCENT]\n", argv[0]);
        return 2;
    }
    const char* file = argv[1];
    const int want[3] = {std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4])};
    const int tolerance = argc > 5 ? std::atoi(argv[5]) : 2;
    const double min_percent = argc > 6 ? std::atof(argv[6]) : 50.0;

    std::ifstream stream(file, std::ios::binary);
    if (!stream) return fail(file, "cannot be opened");
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (data.size() < kHeaderWords * 4) return fail(file, "too short for an XWD header");

    // The header is 25 big-endian 32-bit words, whatever the machine.
    const auto word = [&](std::size_t index) {
        const std::size_t at = index * 4;
        return (std::uint32_t{data[at]} << 24) | (std::uint32_t{data[at + 1]} << 16) |
               (std::uint32_t{data[at + 2]} << 8) | std::uint32_t{data[at + 3]};
    };
    const std::uint32_t header_size = word(0);
    const std::uint32_t version = word(1);
    const std::uint32_t pixmap_format = word(2);
    const std::uint32_t width = word(4);
    const std::uint32_t height = word(5);
    const std::uint32_t byte_order = word(7);  // 0: least significant byte first
    const std::uint32_t bits_per_pixel = word(11);
    const std::uint32_t bytes_per_line = word(12);
    const Channel red = channel_of(word(14));
    const Channel green = channel_of(word(15));
    const Channel blue = channel_of(word(16));
    const std::uint32_t color_count = word(19);

    if (version != 7 || pixmap_format != 2) return fail(file, "not an XWD version 7 ZPixmap");
    if (bits_per_pixel != 32 && bits_per_pixel != 24) return fail(file, "not a 24 or 32 bit screen");
    if (width == 0 || height == 0) return fail(file, "an empty screen");
    const std::size_t bytes_per_pixel = bits_per_pixel / 8;
    const std::size_t pixels_at = std::size_t{header_size} + std::size_t{color_count} * 12;
    if (bytes_per_line < width * bytes_per_pixel || data.size() < pixels_at + std::size_t{bytes_per_line} * height) {
        return fail(file, "shorter than its header says");
    }

    std::uint64_t matching = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row = pixels_at + std::size_t{y} * bytes_per_line;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = row + std::size_t{x} * bytes_per_pixel;
            std::uint32_t pixel = 0;
            for (std::size_t i = 0; i < bytes_per_pixel; ++i) {
                const std::size_t shift = byte_order == 0 ? i * 8 : (bytes_per_pixel - 1 - i) * 8;
                pixel |= std::uint32_t{data[at + i]} << shift;
            }
            const int got[3] = {scaled(pixel, red), scaled(pixel, green), scaled(pixel, blue)};
            bool same = true;
            for (int channel = 0; channel < 3; ++channel) {
                if (std::abs(got[channel] - want[channel]) > tolerance) same = false;
            }
            if (same) ++matching;
        }
    }

    const std::uint64_t total = std::uint64_t{width} * height;
    const double percent = 100.0 * static_cast<double>(matching) / static_cast<double>(total);
    const bool enough = percent >= min_percent;
    std::printf("fbcheck: %ux%u screen, %.1f%% is #%02x%02x%02x (needs %.0f%%): %s\n", width, height, percent,
                static_cast<unsigned>(want[0]), static_cast<unsigned>(want[1]), static_cast<unsigned>(want[2]),
                min_percent, enough ? "yes" : "no");
    return enough ? 0 : 1;
}
