// The volume of the two channels a DualSense's sound device moves its actuators
// with. A desktop's sound service gives a new device less than its full volume
// (PipeWire's session manager: 40%, a sixteenth of the swing), which is right
// for a speaker and wrong for haptics: a game's haptics are meant to arrive as
// the console sent them. Only those two channels are touched; the device's
// speaker keeps the volume its user gave it.
#pragma once

#include <string>
#include <string_view>

namespace psvp::app {

struct HapticsVolume {
    bool found = false;     // a sound device of that name with the actuators' channels is there
    bool changed = false;   // their volume was another one and has been set
    int before = 0;         // what it was, in percent (the lower of the two)
    std::string device;     // the device's description
    std::string problem;    // why nothing was looked at or done, when that is so
};

// Looks for the output device whose name or description contains `needle` (letters of either
// case) and sets its third and fourth channel to `percent` of their full volume (0 to 100), when
// they are not there already. Asks the desktop's sound service (PulseAudio, or PipeWire speaking
// its protocol); waits for it for a second at most. With `only_look` nothing is changed:
// `before` says what the volume is.
[[nodiscard]] HapticsVolume set_haptics_volume(std::string_view needle, int percent, bool only_look = false);

}  // namespace psvp::app
