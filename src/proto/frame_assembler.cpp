#include "proto/frame_assembler.h"

#include <algorithm>

#include "proto/erasure.h"

namespace psvp::proto::transport {
namespace {

// Frame indices wrap around: `a` comes before `b` when it is less than half the number space behind it.
constexpr bool frame_before(std::uint16_t a, std::uint16_t b) noexcept {
    return a != b && static_cast<std::uint16_t>(b - a) < 0x8000U;
}

std::size_t padding_of(std::span<const std::uint8_t> unit) { return (std::size_t{unit[0]} << 8) | unit[1]; }

}  // namespace

bool FrameAssembler::begin(Slot& slot, const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, TimePoint now) {
    slot.begun = true;
    slot.first_seen = now;
    slot.adaptive_stream = packet.adaptive_stream;
    if (packet.units_total <= packet.units_repair || packet.units_total > kMaxUnits || payload.empty()) return false;
    // Every unit of a frame has one size. A repair unit shows it; a source unit says how much shorter it is.
    const unsigned source = static_cast<unsigned>(packet.units_total - packet.units_repair);
    std::size_t unit_size = payload.size();
    if (packet.unit_index < source) {
        if (payload.size() < 2) return false;
        unit_size += padding_of(payload);
    }
    if (unit_size > kMaxUnitSize) return false;
    slot.total = packet.units_total;
    slot.source = source;
    slot.repair = packet.units_repair;
    slot.unit_size = unit_size;
    slot.stride = (unit_size + 15) / 16 * 16;
    slot.sizes.assign(slot.total, 0);
    slot.buffer.assign(std::size_t{slot.total} * slot.stride, 0);
    return true;
}

void FrameAssembler::put(Slot& slot, std::uint16_t index, const StreamMediaPacket& packet, std::span<const std::uint8_t> payload,
                         TimePoint now) {
    const bool is_source = packet.unit_index < slot.source;
    // A unit must have a place in its frame and fit it; a source unit begins with its padding count.
    if (packet.unit_index >= slot.total || packet.unit_index >= packet.units_total || payload.empty() || payload.size() > slot.unit_size ||
        (is_source && payload.size() < 2)) {
        ++counters_.packets_unusable;
        return;
    }
    if (slot.sizes[packet.unit_index] != 0) {
        ++counters_.packets_duplicate;
        return;
    }
    // What a console does not send, as far as is known, and other clients take all the same: a unit
    // that counts its frame's units otherwise, or whose size is not the frame's unit size.
    const bool as_announced = packet.units_total == slot.total && packet.units_repair == slot.repair &&
                              payload.size() + (is_source ? padding_of(payload) : 0) == slot.unit_size;
    if (!as_announced) ++counters_.packets_odd;
    slot.sizes[packet.unit_index] = payload.size();
    std::copy(payload.begin(), payload.end(), slot.buffer.begin() + static_cast<std::ptrdiff_t>(packet.unit_index * slot.stride));
    ++slot.received;
    if (is_source) ++slot.source_received;
    if (packet.unit_index + 1U == slot.total && !slot.last_unit_seen) slot.last_unit_seen = now;

    // Enough for the frame: all of its source units, or as many units of any kind where the repair code reaches.
    const bool enough = slot.source_received == slot.source || (slot.received >= slot.source && slot.total <= kMaxRepairable);
    if (!enough) return;
    slot.done = finish(slot, index);
    // Enough units and still no frame: what was restored does not make sense.
    if (!slot.done) slot.unusable = true;
}

std::optional<AssembledFrame> FrameAssembler::finish(Slot& slot, std::uint16_t index) {
    AssembledFrame frame;
    frame.index = index;
    frame.adaptive_stream = slot.adaptive_stream;
    if (slot.source_received < slot.source) {
        std::vector<unsigned> missing;
        for (unsigned i = 0; i < slot.total; ++i) {
            if (slot.sizes[i] == 0) missing.push_back(i);
        }
        if (!repair_units(slot.buffer, slot.unit_size, slot.stride, slot.source, slot.repair, missing)) return std::nullopt;
        // A restored unit carries its own padding count again.
        unsigned restored = 0;
        for (unsigned i = 0; i < slot.source; ++i) {
            if (slot.sizes[i] != 0) continue;
            const std::size_t padding = padding_of(std::span<const std::uint8_t>(slot.buffer).subspan(i * slot.stride, 2));
            if (slot.unit_size < 2 || padding > slot.unit_size - 2) return std::nullopt;
            slot.sizes[i] = slot.unit_size - padding;
            ++restored;
        }
        counters_.units_repaired += restored;
        frame.repaired = true;
    }
    std::size_t size = 0;
    for (unsigned i = 0; i < slot.source; ++i) size += slot.sizes[i] - 2;
    frame.data.reserve(size);
    for (unsigned i = 0; i < slot.source; ++i) {
        const auto unit = std::span<const std::uint8_t>(slot.buffer).subspan(i * slot.stride + 2, slot.sizes[i] - 2);
        frame.data.insert(frame.data.end(), unit.begin(), unit.end());
    }
    return frame;
}

void FrameAssembler::account(std::uint16_t index, bool whole, unsigned total, unsigned received) {
    Accounted& entry = recent_[recent_at_];
    recent_at_ = (recent_at_ + 1) % recent_.size();
    if (entry.used) {
        unit_stats_.received += entry.received;
        unit_stats_.missing += entry.total - entry.received;
    }
    entry = Accounted{true, whole, index, total, received};
}

UnitStats FrameAssembler::take_unit_stats() noexcept {
    const UnitStats stats = unit_stats_;
    unit_stats_ = {};
    return stats;
}

// `count` frames from `first` on will not be handed over.
void FrameAssembler::lose(std::uint16_t first, std::uint32_t count, FrameOutput& out) {
    const auto last = static_cast<std::uint16_t>(first + count - 1);
    counters_.frames_lost += count;
    lost_since_ += count;
    // Frames next to the last range reported make it longer, as long as a range stays shorter than
    // half the number space, where first and last still tell how many frames it holds.
    const bool joins = !out.lost.empty() && static_cast<std::uint16_t>(out.lost.back().last + 1) == first &&
                       static_cast<std::uint16_t>(out.lost.back().last - out.lost.back().first) + 1U + count <= 0x8000U;
    if (joins) out.lost.back().last = last;
    else out.lost.push_back(FrameRange{first, last});
}

// Hands the oldest frame over if it is whole, and gives it up otherwise.
void FrameAssembler::pop_front(FrameOutput& out) {
    Slot& slot = slots_.front();
    account(next_, slot.done.has_value(), slot.total, slot.received);
    if (slot.done) {
        slot.done->lost_before = lost_since_;
        lost_since_ = 0;
        ++counters_.frames;
        if (slot.done->repaired) ++counters_.frames_repaired;
        out.frames.push_back(std::move(*slot.done));
    } else {
        lose(next_, 1, out);
    }
    slots_.pop_front();
    ++next_;
}

// Whole frames leave in order; a frame that cannot become whole any more does not hold the next ones up.
void FrameAssembler::drain(FrameOutput& out) {
    while (!slots_.empty() && (slots_.front().done || slots_.front().unusable)) pop_front(out);
}

std::optional<TimePoint> FrameAssembler::deadline() const {
    if (slots_.empty()) return std::nullopt;
    std::optional<TimePoint> since;
    const auto earlier = [&](TimePoint when) {
        if (!since || when < *since) since = when;
    };
    // The oldest frame is waited for from the moment a later frame shows up, or its own last unit has come.
    if (slots_.front().last_unit_seen) earlier(*slots_.front().last_unit_seen);
    for (std::size_t i = 1; i < slots_.size(); ++i) {
        if (slots_[i].begun) earlier(slots_[i].first_seen);
    }
    if (!since) return std::nullopt;
    return *since + kWait;
}

void FrameAssembler::on_timeout(TimePoint now, FrameOutput& out) {
    for (auto when = deadline(); when && *when <= now; when = deadline()) {
        pop_front(out);
        drain(out);
    }
}

void FrameAssembler::on_packet(const StreamMediaPacket& packet, std::span<const std::uint8_t> payload, TimePoint now,
                               FrameOutput& out) {
    if (!packet.video) return;
    const std::uint16_t index = packet.frame_index;
    if (!started_) {
        // A stream's frames count from 1: what comes before the first frame seen is missing.
        started_ = true;
        next_ = (index == 1 || frame_before(1, index)) ? std::uint16_t{1} : index;
    }
    if (frame_before(index, next_)) {
        // Its frame's time is over. A frame that was whole did not need it; otherwise it came too late.
        bool spare = false;
        for (Accounted& entry : recent_) {
            if (!entry.used || entry.index != index) continue;
            if (entry.received < entry.total) ++entry.received;
            spare = entry.whole;
        }
        ++(spare ? counters_.packets_spare : counters_.packets_late);
        return;
    }
    auto offset = static_cast<std::uint16_t>(index - next_);
    if (offset >= kWindow) {
        // Too far ahead of the oldest frame under way: the oldest ones are not waited for any longer.
        const auto target = static_cast<std::uint16_t>(index - (kWindow - 1));
        while (next_ != target) {
            if (slots_.empty()) {
                lose(next_, static_cast<std::uint16_t>(target - next_), out);
                next_ = target;
                break;
            }
            pop_front(out);
        }
        offset = static_cast<std::uint16_t>(index - next_);
    }
    if (slots_.size() <= offset) slots_.resize(offset + 1U);
    Slot& slot = slots_[offset];
    if (!slot.begun) slot.unusable = !begin(slot, packet, payload, now);
    if (slot.unusable) {
        ++counters_.packets_unusable;
    } else if (slot.done) {
        // Whole already and waiting for its turn: a unit it no longer needs.
        ++counters_.packets_spare;
        if (slot.received < slot.total) ++slot.received;
    } else {
        put(slot, index, packet, payload, now);
    }
    drain(out);
}

}  // namespace psvp::proto::transport
