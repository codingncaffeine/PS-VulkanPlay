#include "app/decoding.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "media/test_clip.h"

namespace psvp::app {

namespace {
// Sound kept for the tone check: a minute of two channels at 48 kHz.
constexpr std::size_t kMaxSamples = 60U * 48000U * 2U;
// Frames at the start that the tone check leaves out: a codec's first frames fade in.
constexpr std::size_t kToneSkipFrames = 10;
// Frames filled in for lost ones at most, per gap: a long gap is silence, not a held note.
constexpr std::uint32_t kMaxConcealed = 5;

// How far two pictures are apart: the mean difference of their brightness samples, in steps of an
// 8-bit sample; nothing when they are not the same size and depth.
std::optional<double> difference(const media::Picture& a, const media::Picture& b) {
    if (a.width != b.width || a.height != b.height || a.bits != b.bits || a.width == 0 || a.height == 0) return std::nullopt;
    std::uint64_t sum = 0;
    for (unsigned y = 0; y < a.height; ++y) {
        const std::uint8_t* row_a = a.planes[0] + y * a.strides[0];
        const std::uint8_t* row_b = b.planes[0] + y * b.strides[0];
        if (a.bits == 8) {
            for (unsigned x = 0; x < a.width; ++x) sum += static_cast<std::uint64_t>(row_a[x] > row_b[x] ? row_a[x] - row_b[x] : row_b[x] - row_a[x]);
        } else {
            for (unsigned x = 0; x < a.width; ++x) {
                std::uint16_t sample_a = 0, sample_b = 0;
                std::memcpy(&sample_a, row_a + 2 * x, 2);
                std::memcpy(&sample_b, row_b + 2 * x, 2);
                sum += static_cast<std::uint64_t>(sample_a > sample_b ? sample_a - sample_b : sample_b - sample_a);
            }
        }
    }
    const double mean = static_cast<double>(sum) / (static_cast<double>(a.width) * a.height);
    return a.bits == 8 ? mean : mean / static_cast<double>(1U << (a.bits - 8));
}
}  // namespace

void Decoding::reference(std::span<const std::uint8_t> header, unsigned size_index, std::uint16_t index, std::span<const std::uint8_t> frame) {
    if (reference_size_index_ != size_index) {
        reference_size_index_ = size_index;
        reference_.reset();
        if (const auto codec = media::codec_of_header(header)) {
            if (auto opened = media::VideoDecoder::open(*codec, header)) reference_.emplace(std::move(*opened));
        }
    }
    reference_picture_.reset();
    if (!reference_) return;
    reference_picture_ = reference_->decode(frame);
    reference_index_ = index;
}

void Decoding::video(std::span<const std::uint8_t> header, unsigned size_index, std::uint16_t index, std::span<const std::uint8_t> frame,
                     std::uint32_t lost_before) {
    ++frames_;
    if (size_index_ != size_index) {
        // A new picture size is a new stream to the decoder: its header first, then a keyframe.
        size_index_ = size_index;
        if (video_) {
            const auto& counters = video_->counters();
            microseconds_ += counters.microseconds;
            slowest_microseconds_ = std::max(slowest_microseconds_, counters.slowest_microseconds);
            changed_ += counters.changed;
            approximate_ += counters.approximate;
            no_reference_ += counters.undecodable;
        }
        video_.reset();
        const auto codec = media::codec_of_header(header);
        if (!codec) {
            video_error_ = std::format("the picture header of {} bytes is neither H.264's nor HEVC's", header.size());
        } else if (auto opened = media::VideoDecoder::open(*codec, header)) {
            video_.emplace(std::move(*opened));
            decoder_text_ = video_->description();
        } else {
            video_error_ = opened.error();
        }
    }
    if (!video_) {
        ++undecodable_;
        return;
    }
    if (lost_before != 0) {
        after_loss_ = true;
        repair_run_ = 0;
    }
    const auto decoded = video_->decode(index, frame);
    unfollowed_ = video_->unfollowed_reason();
    const std::optional<media::Picture>& picture = decoded.picture;
    const bool compare = reference_picture_ && reference_index_ == index;
    if (!picture) {
        if (!decoded.needs_keyframe) ++refused_;
        if (compare) ++missing_;
        return;
    }
    ++pictures_;
    if (picture->damaged) ++damaged_;
    if (compare) {
        if (picture->damaged) {
            ++missing_;  // a player does not show it
        } else if (const auto apart = difference(*picture, *reference_picture_)) {
            ++compared_;
            if (*apart == 0.0) ++same_;
            difference_sum_ += *apart;
            if (*apart > worst_difference_) {
                worst_difference_ = *apart;
                worst_index_ = index;
            }
        }
    }
    if (picture_text_.empty()) {
        picture_text_ = std::format("{}x{}, {} bits, {} range, {}{}", picture->width, picture->height, picture->bits,
                                    picture->full_range ? "full" : "limited",
                                    picture->matrix == media::ColorMatrix::bt2020 ? "BT.2020" : picture->matrix == media::ColorMatrix::bt601 ? "BT.601" : "BT.709",
                                    picture->transfer == media::ColorTransfer::pq ? ", HDR (PQ)" : picture->transfer == media::ColorTransfer::hlg ? ", HDR (HLG)" : "");
    }
    if (!options_.expect_clip) return;
    const bool right = picture->bits == 8 && media::read_clip_counter(picture->view()) == index && media::clip_differences(picture->view(), index) == 0;
    if (right) {
        ++clip_right_;
        if (after_loss_) longest_repair_ = std::max(longest_repair_, repair_run_);
        after_loss_ = false;
    } else if (after_loss_) {
        // Between a lost frame and the console's repair the decoder shows what it kept.
        ++clip_wrong_after_loss_;
        ++repair_run_;
    } else {
        ++clip_wrong_;
        if (!clip_first_wrong_) clip_first_wrong_ = index;
    }
}

void Decoding::sound(unsigned rate, unsigned channels, unsigned frame_samples, std::span<const std::uint8_t> frame, std::uint32_t lost_before) {
    ++sound_frames_;
    if (!sound_ && sound_error_.empty()) {
        if (auto opened = media::SoundDecoder::open(rate, channels, frame_samples)) {
            sound_.emplace(std::move(*opened));
            sound_text_ = std::format("{} samples a frame, {} Hz, {} channels", frame_samples, rate, channels);
        } else {
            sound_error_ = opened.error();
        }
    }
    if (!sound_) return;
    const auto keep = [&](std::span<const std::int16_t> samples) {
        if (options_.expect_clip && samples_.size() < kMaxSamples) samples_.insert(samples_.end(), samples.begin(), samples.end());
    };
    for (std::uint32_t i = 0; i < std::min(lost_before, kMaxConcealed); ++i) keep(sound_->conceal());
    keep(sound_->decode(frame));
}

std::vector<std::string> Decoding::report() const {
    std::vector<std::string> lines;
    if (frames_ != 0) {
        std::uint64_t microseconds = microseconds_, slowest = slowest_microseconds_;
        std::uint64_t changed = changed_, approximate = approximate_, no_reference = no_reference_;
        if (video_) {
            microseconds += video_->counters().microseconds;
            slowest = std::max(slowest, video_->counters().slowest_microseconds);
            changed += video_->counters().changed;
            approximate += video_->counters().approximate;
            no_reference += video_->counters().undecodable;
        }
        if (!video_error_.empty()) lines.push_back(std::format("Pictures could not be decoded: {}.", video_error_));
        if (pictures_ + refused_ != 0) {
            lines.push_back(std::format("Decoded ({}): {} pictures from {} frames ({}); marked as damaged: {}; frames that gave no picture: {}; "
                                        "{:.2f} ms a frame, {:.2f} ms the slowest.",
                                        decoder_text_, pictures_, frames_, picture_text_.empty() ? "no picture" : picture_text_, damaged_,
                                        refused_ + undecodable_, static_cast<double>(microseconds) / 1000.0 / static_cast<double>(pictures_ + refused_),
                                        static_cast<double>(slowest) / 1000.0));
        }
        if (pictures_ + refused_ != 0) {
            if (unfollowed_.empty()) {
                lines.push_back(std::format("Behind lost frames: {} frames were decoded from the nearest frame held in place of a lost one, {} pictures "
                                            "were built on those until the console's repair, {} frames could not be decoded from anything held.",
                                            changed, approximate, no_reference));
            } else {
                lines.push_back(std::format("Behind lost frames the picture stands until the console's repair: {}.", unfollowed_));
            }
        }
        if (compared_ + missing_ != 0) {
            lines.push_back(std::format("Against the stream with nothing lost: {} pictures are the same sample for sample, {} are not (off by {:.2f} of "
                                        "255 on average, {:.2f} at most, in frame {}), {} are not there to be shown.",
                                        same_, compared_ - same_, compared_ == same_ ? 0.0 : difference_sum_ / static_cast<double>(compared_ - same_),
                                        worst_difference_, worst_index_, missing_));
        }
        if (options_.expect_clip) {
            lines.push_back(std::format("Test clip: {} pictures are their frame's, sample for sample; {} are not{}; {} more are not between a lost "
                                        "frame and its repair (the longest wait: {} pictures).",
                                        clip_right_, clip_wrong_, clip_first_wrong_ ? std::format(" (the first: frame {})", *clip_first_wrong_) : "",
                                        clip_wrong_after_loss_, longest_repair_));
        }
    }
    if (sound_frames_ != 0) {
        if (!sound_error_.empty()) lines.push_back(std::format("Sound could not be decoded: {}.", sound_error_));
        if (sound_) {
            const auto& counters = sound_->counters();
            lines.push_back(std::format("Sound decoded (Opus): {} frames ({}); filled in for frames that did not arrive: {}; unreadable: {}.",
                                        counters.frames, sound_text_, counters.concealed, counters.refused));
            if (options_.expect_clip) {
                const std::size_t skip = std::min(samples_.size(), kToneSkipFrames * media::ToneWriter::kFrameSamples * sound_->channels());
                const std::span<const std::int16_t> heard(samples_.data() + skip, samples_.size() - skip);
                lines.push_back(std::format("Test tone: {:.0f} Hz is {:.0f}% of the left channel, {:.0f} Hz is {:.0f}% of the right.",
                                            media::ToneWriter::kLeftHz, 100.0 * media::tone_share(heard, sound_->channels(), 0, sound_->rate(), media::ToneWriter::kLeftHz),
                                            media::ToneWriter::kRightHz,
                                            100.0 * media::tone_share(heard, sound_->channels(), sound_->channels() - 1, sound_->rate(), media::ToneWriter::kRightHz)));
            }
        }
    }
    if (lines.empty()) lines.emplace_back("Nothing arrived to decode.");
    return lines;
}

bool Decoding::good() const {
    if (!video_error_.empty() || !sound_error_.empty()) return false;
    if (!options_.expect_clip) return pictures_ != 0 || (sound_ && sound_->counters().frames != 0);
    if (clip_right_ == 0 || clip_wrong_ != 0 || refused_ + undecodable_ != 0) return false;
    if (!sound_ || sound_->counters().refused != 0) return false;
    const std::size_t skip = std::min(samples_.size(), kToneSkipFrames * media::ToneWriter::kFrameSamples * sound_->channels());
    const std::span<const std::int16_t> heard(samples_.data() + skip, samples_.size() - skip);
    return media::tone_share(heard, sound_->channels(), 0, sound_->rate(), media::ToneWriter::kLeftHz) > 0.8 &&
           media::tone_share(heard, sound_->channels(), sound_->channels() - 1, sound_->rate(), media::ToneWriter::kRightHz) > 0.8;
}

}  // namespace psvp::app
