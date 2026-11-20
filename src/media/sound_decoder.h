// Turns the sound frames of a stream (Opus) into samples, with libopus.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace psvp::media {

struct SoundDecoderCounters {
    std::uint64_t frames = 0;     // decoded
    std::uint64_t concealed = 0;  // made up for a frame that did not arrive
    std::uint64_t refused = 0;    // frames the decoder could not read
};

class SoundDecoder {
public:
    // `frame_samples` is what one frame holds per channel (the console's sound header says it).
    [[nodiscard]] static std::expected<SoundDecoder, std::string> open(unsigned rate, unsigned channels, unsigned frame_samples);
    SoundDecoder(SoundDecoder&&) noexcept;
    SoundDecoder& operator=(SoundDecoder&&) noexcept;
    ~SoundDecoder();

    // One frame's samples, the channels interleaved; they stay valid until the next call.
    // A frame that cannot be read gives silence of a frame's length.
    [[nodiscard]] std::span<const std::int16_t> decode(std::span<const std::uint8_t> frame);
    // A frame did not arrive: samples that continue the sound before it.
    [[nodiscard]] std::span<const std::int16_t> conceal();

    [[nodiscard]] const SoundDecoderCounters& counters() const noexcept;
    [[nodiscard]] unsigned channels() const noexcept;
    [[nodiscard]] unsigned rate() const noexcept;

private:
    struct State;
    explicit SoundDecoder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// The sound of the generated clip: a steady tone of 440 Hz on the left channel and of
// 880 Hz on the right, as Opus frames of 10 ms, the size a console's are.
class ToneWriter {
public:
    static constexpr unsigned kRate = 48000, kChannels = 2, kFrameSamples = 480;
    static constexpr double kLeftHz = 440.0, kRightHz = 880.0;

    ToneWriter();
    ToneWriter(ToneWriter&&) noexcept;
    ToneWriter& operator=(ToneWriter&&) noexcept;
    ~ToneWriter();
    // The next frame; frames continue one another.
    [[nodiscard]] std::vector<std::uint8_t> frame();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// How much of one channel's power lies at one frequency, from 0 to 1.
[[nodiscard]] double tone_share(std::span<const std::int16_t> interleaved, unsigned channels, unsigned channel, unsigned rate, double hz);

}  // namespace psvp::media
