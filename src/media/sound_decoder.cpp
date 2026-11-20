#include "media/sound_decoder.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include <opus.h>

namespace psvp::media {

namespace {
struct DecoderDeleter {
    void operator()(OpusDecoder* decoder) const noexcept { opus_decoder_destroy(decoder); }
};
struct EncoderDeleter {
    void operator()(OpusEncoder* encoder) const noexcept { opus_encoder_destroy(encoder); }
};
}  // namespace

struct SoundDecoder::State {
    std::unique_ptr<OpusDecoder, DecoderDeleter> decoder;
    unsigned rate = 0, channels = 0, frame_samples = 0;
    std::vector<std::int16_t> samples;
    SoundDecoderCounters counters;
};

SoundDecoder::SoundDecoder(std::unique_ptr<State> state) : state_(std::move(state)) {}
SoundDecoder::SoundDecoder(SoundDecoder&&) noexcept = default;
SoundDecoder& SoundDecoder::operator=(SoundDecoder&&) noexcept = default;
SoundDecoder::~SoundDecoder() = default;

std::expected<SoundDecoder, std::string> SoundDecoder::open(unsigned rate, unsigned channels, unsigned frame_samples) {
    // Opus knows five rates and one or two channels; a frame is at most 120 ms.
    const bool rate_known = rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
    if (!rate_known || channels < 1 || channels > 2 || frame_samples == 0 || frame_samples > rate / 1000 * 120) {
        return std::unexpected(std::format("sound of {} Hz with {} channels in frames of {} samples cannot be decoded", rate, channels, frame_samples));
    }
    auto state = std::make_unique<State>();
    int error = 0;
    state->decoder.reset(opus_decoder_create(static_cast<opus_int32>(rate), static_cast<int>(channels), &error));
    if (!state->decoder || error != OPUS_OK) {
        return std::unexpected(std::format("the Opus decoder did not open: {}", opus_strerror(error)));
    }
    state->rate = rate;
    state->channels = channels;
    state->frame_samples = frame_samples;
    state->samples.assign(static_cast<std::size_t>(frame_samples) * channels, 0);
    return SoundDecoder(std::move(state));
}

std::span<const std::int16_t> SoundDecoder::decode(std::span<const std::uint8_t> frame) {
    State& state = *state_;
    const int samples = frame.empty() ? -1
                                      : opus_decode(state.decoder.get(), frame.data(), static_cast<opus_int32>(frame.size()), state.samples.data(),
                                                    static_cast<int>(state.frame_samples), 0);
    if (samples != static_cast<int>(state.frame_samples)) {
        ++state.counters.refused;
        std::ranges::fill(state.samples, std::int16_t{0});
    } else {
        ++state.counters.frames;
    }
    return state.samples;
}

std::span<const std::int16_t> SoundDecoder::conceal() {
    State& state = *state_;
    const int samples = opus_decode(state.decoder.get(), nullptr, 0, state.samples.data(), static_cast<int>(state.frame_samples), 0);
    if (samples != static_cast<int>(state.frame_samples)) std::ranges::fill(state.samples, std::int16_t{0});
    ++state.counters.concealed;
    return state.samples;
}

const SoundDecoderCounters& SoundDecoder::counters() const noexcept { return state_->counters; }
unsigned SoundDecoder::channels() const noexcept { return state_->channels; }
unsigned SoundDecoder::rate() const noexcept { return state_->rate; }

struct ToneWriter::State {
    std::unique_ptr<OpusEncoder, EncoderDeleter> encoder;
    std::uint64_t sample = 0;
};

ToneWriter::ToneWriter() : state_(std::make_unique<State>()) {
    int error = 0;
    state_->encoder.reset(opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error));
    if (state_->encoder) {
        // 64 kbit/s at a constant rate: 80 bytes for every 10 ms, a console's frame size.
        opus_encoder_ctl(state_->encoder.get(), OPUS_SET_BITRATE(64000));
        opus_encoder_ctl(state_->encoder.get(), OPUS_SET_VBR(0));
    }
}
ToneWriter::ToneWriter(ToneWriter&&) noexcept = default;
ToneWriter& ToneWriter::operator=(ToneWriter&&) noexcept = default;
ToneWriter::~ToneWriter() = default;

std::vector<std::uint8_t> ToneWriter::frame() {
    std::vector<std::int16_t> samples(static_cast<std::size_t>(kFrameSamples) * kChannels);
    for (unsigned i = 0; i < kFrameSamples; ++i, ++state_->sample) {
        const double seconds = static_cast<double>(state_->sample) / kRate;
        samples[2 * i] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * kLeftHz * seconds));
        samples[2 * i + 1] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * kRightHz * seconds));
    }
    std::vector<std::uint8_t> out(400);
    const int size = state_->encoder ? opus_encode(state_->encoder.get(), samples.data(), kFrameSamples, out.data(), static_cast<opus_int32>(out.size())) : -1;
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    return out;
}

double tone_share(std::span<const std::int16_t> interleaved, unsigned channels, unsigned channel, unsigned rate, double hz) {
    if (channels == 0 || channel >= channels || rate == 0) return 0.0;
    const std::size_t count = interleaved.size() / channels;
    if (count == 0) return 0.0;
    // The sum of the samples against a sine and a cosine of the frequency gives the part of
    // the signal that is this tone; its power over the whole power is the share.
    double with_sine = 0.0, with_cosine = 0.0, power = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double sample = interleaved[i * channels + channel];
        const double angle = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
        with_sine += sample * std::sin(angle);
        with_cosine += sample * std::cos(angle);
        power += sample * sample;
    }
    if (power <= 0.0) return 0.0;
    const double tone_power = 2.0 * (with_sine * with_sine + with_cosine * with_cosine) / static_cast<double>(count);
    return std::min(1.0, tone_power / power);
}

}  // namespace psvp::media
