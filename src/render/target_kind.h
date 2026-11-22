// What the window's swapchain takes, for the passes that draw into it.
#pragma once

namespace psvp::render {

enum class TargetKind {
    sdr,       // SDR, the values written are the values shown (an 8-bit UNORM format, sRGB color space)
    sdr_srgb,  // SDR, the format encodes what is written (an _SRGB format): it is given linear light
    hdr10,     // HDR10: SMPTE ST 2084 signal, BT.2020 primaries (VK_COLOR_SPACE_HDR10_ST2084_EXT, 10 bits)
};

}  // namespace psvp::render
