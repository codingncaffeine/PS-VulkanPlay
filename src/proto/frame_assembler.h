// Puts picture frames together from their units, restores lost units from the
// repair units, hands the frames over in order and tells which ones will not
// come. docs/protocol/video.md describes a frame's layout and the rules here.
// No sockets and no clock of its own: the caller passes the time in.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "proto/media_packet.h"
#include "proto/transport.h"

namespace psvp::proto::transport {

struct AssembledFrame {
    std::uint16_t index = 0;
    std::uint8_t adaptive_stream = 0;
    std::vector<std::uint8_t> data;
    bool repaired = false;            // some of its units were restored from repair units
    std::uint32_t lost_before = 0;    // frames given up since the frame handed over before this one
};

// Frames first to last, both included; indices wrap around.
struct FrameRange {
    std::uint16_t first = 0;
    std::uint16_t last = 0;
    friend bool operator==(const FrameRange&, const FrameRange&) = default;
};

struct FrameOutput {
    std::vector<AssembledFrame> frames;  // in order
    std::vector<FrameRange> lost;        // frames that will not be handed over: the console is told
};

struct FrameCounters {
    std::uint64_t frames = 0;
    std::uint64_t frames_repaired = 0;
    std::uint64_t frames_lost = 0;
    std::uint64_t units_repaired = 0;
    std::uint64_t packets_spare = 0;      // of a frame that was whole without them
    std::uint64_t packets_late = 0;       // of a frame that had been given up
    std::uint64_t packets_duplicate = 0;
    std::uint64_t packets_unusable = 0;   // that contradict themselves or their frame
    std::uint64_t packets_odd = 0;        // used, although their sizes or counts are not what their frame's first packet said
};

// Units that arrived and units that did not, of the frames whose time is over.
struct UnitStats {
    std::uint64_t received = 0;
    std::uint64_t missing = 0;
};

class FrameAssembler {
public:
    static constexpr std::size_t kMaxUnits = 512;
    static constexpr std::size_t kMaxRepairable = 256;  // the repair code has no more distinct factors
    static constexpr std::size_t kMaxUnitSize = 1500;   // no unit is larger than a datagram
    static constexpr std::uint16_t kWindow = 4;         // frames that may be under way at once
    static constexpr std::chrono::milliseconds kWait{16};
    // How long a frame is still waited for once a later frame is whole: what is missing of
    // it then was lost, unless it was overtaken on the way by a moment.
    static constexpr std::chrono::milliseconds kWaitBehindWhole{2};

    // `payload` is the packet's payload, decrypted.
    void on_packet(const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, TimePoint now, FrameOutput& out);
    // To be called when deadline() has passed.
    void on_timeout(TimePoint now, FrameOutput& out);
    // When the oldest frame under way is given up; nothing while no frame waits for another.
    [[nodiscard]] std::optional<TimePoint> deadline() const;

    [[nodiscard]] const FrameCounters& counters() const noexcept { return counters_; }
    // What was counted since the last call.
    [[nodiscard]] UnitStats take_unit_stats() noexcept;

private:
    struct Slot {
        bool begun = false;
        bool unusable = false;
        std::optional<AssembledFrame> done;
        TimePoint first_seen{};
        std::optional<TimePoint> last_unit_seen;  // when the unit with the highest index arrived
        TimePoint whole_at{};                     // when it became whole, for a frame that is
        std::uint8_t adaptive_stream = 0;
        unsigned total = 0;
        unsigned source = 0;
        unsigned repair = 0;
        std::size_t unit_size = 0;
        std::size_t stride = 0;
        unsigned received = 0;
        unsigned source_received = 0;
        std::vector<std::size_t> sizes;  // per unit: the bytes that arrived, 0 for none
        std::vector<std::uint8_t> buffer;
    };
    struct Accounted {
        bool used = false;
        bool whole = false;
        std::uint16_t index = 0;
        unsigned total = 0;
        unsigned received = 0;
    };

    static bool begin(Slot& slot, const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, TimePoint now);
    void put(Slot& slot, std::uint16_t index, const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, TimePoint now);
    std::optional<AssembledFrame> finish(Slot& slot, std::uint16_t index);
    void drain(FrameOutput& out);
    void pop_front(FrameOutput& out);
    void lose(std::uint16_t first, std::uint32_t count, FrameOutput& out);
    void account(std::uint16_t index, bool whole, unsigned total, unsigned received);

    bool started_ = false;
    std::uint16_t next_ = 0;       // the frame to hand over next; slots_[i] holds frame next_ + i
    std::deque<Slot> slots_;
    std::uint32_t lost_since_ = 0;
    std::array<Accounted, 4> recent_{};  // frames whose time is over, for units that still arrive
    std::size_t recent_at_ = 0;
    UnitStats unit_stats_;
    FrameCounters counters_;
};

}  // namespace psvp::proto::transport
