// How a picture is brought to the size it is drawn at, and how banding is smoothed out of it.
//
// The stream window's shaders (src/render/shaders/convert.frag, video.frag) do these steps; the
// functions here do the same in the machine's own numbers, and are what their pixels are checked
// against (psvp_render_check --scaling).
//
// Scaling: every drawn pixel is a weighted mean of the picture's samples around the place it falls
// on. The weight of a sample is a function of its distance (the kernel): separable kernels take the
// distances across and down apart and multiply, polar ("EWA", elliptical weighted average) kernels
// take the straight distance. When the picture is drawn smaller, the kernel is stretched by as
// much, so that it still averages every sample it passes over (else fine detail flickers).
// The weights come from a table of each kernel, filled here once, and read with linear steps
// between its entries, by the shader and by the reference alike.
//
// Debanding: a sample is compared with the mean of four others at a random distance around it (the
// same distance turned four quarter turns); where it is that close to them, the place is a smooth
// area that the encoder cut into flat bands, and it takes their mean. Then a faint noise of the
// same random numbers is added (grain), which hides what is left of the steps on an 8-bit screen.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace psvp::media {

enum class Scaler : std::uint8_t {
    bilinear,           // a tent, 1 sample out: soft, the cheapest
    catmull_rom,        // the cubic with B = 0, C = 0.5 (Mitchell and Netravali), 2 out
    lanczos,            // sinc windowed by sinc, 3 lobes, separable
    ewa_lanczos,        // jinc windowed by jinc, 3 lobes, polar: what chiaki-ng uses by default
    ewa_lanczos_sharp,  // the same, made narrower by 0.98125 (sharper, a little more ringing)
};
inline constexpr std::array kScalers{Scaler::bilinear, Scaler::catmull_rom, Scaler::lanczos, Scaler::ewa_lanczos, Scaler::ewa_lanczos_sharp};
inline constexpr Scaler kDefaultScaler = Scaler::ewa_lanczos;

// The name on the command line and in the log ("bilinear", "catmull-rom", "lanczos", "ewa-lanczos",
// "ewa-lanczos-sharp"), and back.
[[nodiscard]] std::string_view scaler_name(Scaler scaler);
[[nodiscard]] std::optional<Scaler> scaler_from_name(std::string_view name);

// The first zeros of jinc(x) = 2 J1(pi x) / (pi x): the polar kernels end at the third.
inline constexpr double kJincZero1 = 1.2196698912665045;
inline constexpr double kJincZero3 = 3.2383154841662362;
inline constexpr double kEwaSharpBlur = 0.9812505644269356;

// J1, Bessel's function of the first kind and order one (its series, in doubles: good to about
// 1e-12 as far as the kernels reach), and jinc as above (jinc(0) = 1).
[[nodiscard]] double bessel_j1(double x);
[[nodiscard]] double jinc(double x);

// Where a kernel ends (its weight is 0 from there on), in samples, before stretching; and whether it
// takes the straight distance.
[[nodiscard]] double kernel_radius(Scaler scaler);
[[nodiscard]] bool kernel_polar(Scaler scaler);
// The kernel's weight at a distance (exact, in doubles).
[[nodiscard]] double kernel_weight(Scaler scaler, double distance);

// The table: kTableEntries weights from distance 0 to the radius, evenly apart, one kernel after
// the other in the order of kScalers (the shader takes it as one buffer).
inline constexpr std::uint32_t kTableEntries = 1024;
[[nodiscard]] std::vector<float> weight_tables();
// A weight read from the tables as the shader reads it (float, linear between entries).
[[nodiscard]] float table_weight(const std::vector<float>& tables, Scaler scaler, float distance);

// How much the kernel is stretched along one axis for a picture `from` samples long drawn `to`
// pixels long: 1 when drawn as large or larger, the ratio when smaller, at most kMostStretch.
inline constexpr float kMostStretch = 8.0F;
[[nodiscard]] float stretch_for(std::uint32_t from, std::uint32_t to);

// One channel of a picture (`width` x `height` values, row by row) drawn `to_width` x `to_height`,
// as the shader draws it: a place's value from the samples inside the stretched radius, edges
// repeated, each weight from the table, the sum divided by the sum of the weights. Drawn as large
// as the picture, the samples themselves.
[[nodiscard]] std::vector<float> scale_plane(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, std::uint32_t to_width,
                                             std::uint32_t to_height, Scaler scaler, const std::vector<float>& tables);

// Debanding's settings, in shares of a sample's largest value (a picture of 8 bits: 1/255 is a step).
struct Deband {
    float threshold = 64.0F / 16384.0F;  // closer than this to the mean of the four: smoothed
    int radius = 16;                     // the four are at most this many samples away in each direction
    float grain = 32.0F / 8192.0F;       // the noise added spans this much (half of it each way)
};

// The random numbers both sides draw: a 32-bit integer hash (lowbias32), and a place's own number
// from its column, row and the picture's seed.
[[nodiscard]] constexpr std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
[[nodiscard]] constexpr std::uint32_t place_hash(std::uint32_t x, std::uint32_t y, std::uint32_t seed) { return hash32(x + hash32(y + hash32(seed))); }

// Where the four samples compared with a place lie: the first of them, the others are it turned
// a quarter, half and three quarters around the place.
struct DebandOffset {
    int x = 0, y = 0;
};
[[nodiscard]] DebandOffset deband_offset(std::uint32_t place, int radius);
// The grain at a place: from -grain/2 to grain/2.
[[nodiscard]] float deband_grain(std::uint32_t place, float grain);

// Debanding of one channel as the shader does it for a picture whose color planes are even (a gray
// one: the shader compares all three together, and the color differences then never differ), the
// grain left out (it is added after the conversion to red, green and blue). Values as shares.
[[nodiscard]] std::vector<float> deband_plane(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, const Deband& deband, std::uint32_t seed);

// Sharpening, of the brightness only, at the picture's own size (before it is scaled): contrast
// adaptive, after AMD's published description of CAS. A sample and its four neighbors across and
// down: the less room the brightest and darkest of them leave to the ends of the range, the less it
// is sharpened (no ringing into black or white, little in already sharp detail); the sample moves
// away from its neighbors' mean by a weight of at most 1/8 (strength 0) to 1/5 (strength 1) of the
// difference. `strength` from 0 to 1; 0 is still a little sharper than nothing, so "off" is no pass.
[[nodiscard]] float sharpen_sample(float c, float north, float south, float east, float west, float strength);
// One plane through sharpen_sample, the edges repeated. Values as shares.
[[nodiscard]] std::vector<float> sharpen_plane(const std::vector<float>& plane, std::uint32_t width, std::uint32_t height, float strength);
// The user's percent (1 to 100) as the strength above; 0 percent is no sharpening at all.
[[nodiscard]] constexpr float sharpen_strength(int percent) { return static_cast<float>(percent - 1) / 99.0F; }

}  // namespace psvp::media
