// A still test picture for judging the picture path on the user's own screen (PLAN phase 7): drawn
// exactly as a console's picture is (the same conversion, scaling, tone mapping or HDR10), so what is
// wrong on it is wrong in a stream too. 1920x1080, 10-bit, limited range, 4:2:0.
//
// SDR (BT.709): a gradient from black to white (it must be smooth), eleven grays from 0 to 100%,
// black and white level patches (below black and above white must not be seen, the ones just inside
// must), 75% color bars, full primaries and secondaries, and a one-pixel checkerboard (scaling).
// HDR (BT.2020, PQ): a PQ gradient to 10000 cd/m², grays at set brightnesses from 0.1 to 10000
// cd/m² (where the screen or the tone mapping stops telling them apart is its peak), near-black
// patches, and colors at 203 cd/m² (SDR white): BT.709's (they must look as in the SDR picture)
// and BT.2020's (more saturated, where the screen can).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/video_decoder.h"

namespace psvp::media {

struct TestChart {
    unsigned width = 0, height = 0;
    bool hdr = false;
    std::vector<std::uint16_t> y, cb, cr;  // 10-bit codes
    // A picture over the planes above (they must outlive it).
    [[nodiscard]] Picture picture() const;
    // What to look for, in one short line, for the notice over it (the app's page says the rest).
    [[nodiscard]] std::string notice() const;
};

[[nodiscard]] TestChart make_test_chart(bool hdr);

// The places of the parts, for checks: the brightness a 10-bit code stands for, and where each
// band starts (rows).
inline constexpr unsigned kChartWidth = 1920, kChartHeight = 1080;
inline constexpr unsigned kChartGradientRows = 180, kChartGraysAt = 180, kChartLevelsAt = 360, kChartBarsAt = 660, kChartPrimariesAt = 900;
// The HDR grays, in cd/m².
inline constexpr double kChartHdrGrays[] = {0.1, 1.0, 5.0, 10.0, 50.0, 100.0, 203.0, 400.0, 600.0, 1000.0, 2000.0, 4000.0, 10000.0};

}  // namespace psvp::media
