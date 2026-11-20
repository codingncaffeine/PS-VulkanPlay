#include "app/player.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/haptics_volume.h"
#include "app/log.h"
#include "app/window_place.h"
#include "media/sound_decoder.h"
#include "media/frame_pacer.h"
#include "media/haptics.h"
#include "media/sound_buffer.h"
#include "proto/controller_feedback.h"
#include "media/video_decoder.h"
#include "psvp/version.h"
#include "render/vk_window.h"

namespace psvp::app {

namespace {

namespace transport = proto::transport;

using Clock = std::chrono::steady_clock;
// A picture that follows the one before it by more than this came late enough to be seen: the
// picture stood. Frames are 16.7 ms apart.
constexpr double kStoodMs = 30.0;
// What counts as a cause of that: a pause in arriving, and a frame that waited for the decoder.
constexpr double kArrivedLateMs = 25.0, kDecoderBehindMs = 12.0;
// A frame this close behind a late one was held up with it.
constexpr double kRightBehindMs = 10.0;
// A console that sends no frame that can be decoded is asked for a keyframe this often at most.
constexpr auto kKeyframeAskEvery = std::chrono::milliseconds(500);
// A rumble the console does not take back lasts this long at most: a lost "stop" must not leave a
// controller shaking.
constexpr std::uint32_t kRumbleLifeMs = 5000;
// Haptics on a controller that can only rumble: its motors are set from every third frame's
// time, to the largest swing in it, and stop by themselves when no more frames come.
constexpr unsigned kHapticsFramesPerRumble = 3;
constexpr std::uint32_t kHapticsRumbleLifeMs = 100;
// Haptics that wait to be played on the controller's sound device, at most: more is old.
constexpr int kMaxHapticsQueuedMs = 80;
// The speed of the sound is set anew when it should differ by this much.
constexpr double kSpeedStep = 0.002;
// The touchpad as a console counts it (the reference's numbers).
constexpr float kTouchWidth = 1919.0F, kTouchHeight = 1079.0F;

// How far apart things came that should come every 16.7 ms, counted in bands.
struct Spacing {
    static constexpr std::array<double, 7> kUpTo{8.0, 13.0, 21.0, 30.0, 50.0, 100.0, 1e12};
    std::array<std::uint64_t, 7> counts{};
    void add(double ms) {
        for (std::size_t i = 0; i < kUpTo.size(); ++i) {
            if (ms < kUpTo[i]) {
                ++counts[i];
                return;
            }
        }
    }
    [[nodiscard]] std::string text() const {
        return std::format("under 8 ms: {}, 8 to 13: {}, 13 to 21: {}, 21 to 30: {}, 30 to 50: {}, 50 to 100: {}, over 100: {}", counts[0], counts[1],
                           counts[2], counts[3], counts[4], counts[5], counts[6]);
    }
};

class Player {
public:
    // `frames_per_second` is what the console was asked for; with `pacing`, pictures are shown that evenly.
    Player(double frames_per_second, bool pacing, int haptics_volume, std::optional<transport::Button> back_left,
           std::optional<transport::Button> back_right)
        : haptics_volume_(haptics_volume), back_left_(back_left), back_right_(back_right), decode_thread_([this] { decode_loop(); }) {
        const std::lock_guard lock(jobs_mutex_);
        if (pacing) pacer_.emplace(frames_per_second);
    }
    // Ends the decoding: frames still waiting are not decoded any more. After it the counts stand still.
    void finish() {
        {
            const std::lock_guard lock(jobs_mutex_);
            closing_ = true;
        }
        jobs_wake_.notify_all();
        if (decode_thread_.joinable()) decode_thread_.join();
    }
    ~Player() {
        finish();
        if (audio_ != nullptr) SDL_DestroyAudioStream(audio_);
        if (haptics_audio_ != nullptr) SDL_DestroyAudioStream(haptics_audio_);
        if (pad_ != nullptr) {
            release_triggers();
            SDL_RumbleGamepad(pad_, 0, 0, 0);  // whatever the console said last
            SDL_CloseGamepad(pad_);
        }
    }

    // ---- on the session's thread ----
    void on_media(transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) {
        if (!info) return;
        {
            const auto now = Clock::now();
            const std::lock_guard lock(jobs_mutex_);
            for (auto& frame : media.video) {
                Job job;
                if (frame.adaptive_stream < info->resolutions.size()) job.header = info->resolutions[frame.adaptive_stream].video_header;
                job.size_index = frame.adaptive_stream;
                job.index = frame.index;
                job.lost_before = frame.lost_before;
                job.arrived = now;
                if (last_arrival_) {
                    job.since_arrival_ms = std::chrono::duration<double, std::milli>(now - *last_arrival_).count();
                    arrival_spacing_.add(job.since_arrival_ms);
                    // A frame that came late, with none lost before it: held up on the way when the next one
                    // comes right behind it, sent late by the console when the next one keeps its distance
                    // (a game that does not make sixty pictures a second at that moment).
                    if (std::exchange(late_before_, false)) ++(job.since_arrival_ms < kRightBehindMs ? late_on_the_way_ : late_sent_);
                    late_before_ = job.since_arrival_ms > kArrivedLateMs && frame.lost_before == 0;
                }
                last_arrival_ = now;
                job.data = std::move(frame.data);  // nobody else is handed these frames
                jobs_.push_back(std::move(job));
            }
            longest_queue_ = std::max(longest_queue_, jobs_.size());
        }
        jobs_wake_.notify_one();
        if (!media.sound.empty()) {
            const proto::AudioHeader format = proto::parse_audio_header(info->audio_header).value_or(proto::AudioHeader{});
            for (const auto& frame : media.sound) {
                if (!frame.haptics) play_sound(format, frame);
                else play_haptics(frame);
            }
        }
        started_.store(true);
        started_.notify_all();
    }
    // What the console sends for the controller: rumble and the light bar's color go to the
    // controller at once; the rest is counted until this program passes it on.
    void on_console_data(const std::vector<transport::ConsoleData>& data) {
        for (const transport::ConsoleData& item : data) {
            if (item.message) continue;
            const std::lock_guard lock(pad_mutex_);
            if (item.kind == transport::data_type::kRumble) {
                const auto rumble = transport::parse_rumble(item.bytes);
                if (!rumble) continue;
                ++feedback_.rumbles;
                feedback_.strongest_left = std::max(feedback_.strongest_left, rumble->left);
                feedback_.strongest_right = std::max(feedback_.strongest_right, rumble->right);
                log::debug("play: the console sets the rumble to {} and {} of 255", rumble->left, rumble->right);
                if (pad_ != nullptr && SDL_RumbleGamepad(pad_, static_cast<Uint16>(rumble->left * 257), static_cast<Uint16>(rumble->right * 257),
                                                         rumble->left != 0 || rumble->right != 0 ? kRumbleLifeMs : 0)) {
                    ++feedback_.rumbles_passed;
                }
            } else if (item.kind == transport::data_type::kPadInfo) {
                const auto info = transport::parse_controller_info(item.bytes);
                if (!info) continue;
                ++feedback_.infos;
                if (info->reset_motion) ++feedback_.motion_resets;
                feedback_.player = info->player;
                feedback_.haptics_strength = info->haptics;
                feedback_.triggers_strength = info->triggers;
                if (feedback_.light != info->light) {
                    feedback_.light = info->light;
                    ++feedback_.lights;
                    log::debug("play: the console sets the light bar to {}, {}, {} (player {})", info->light[0], info->light[1], info->light[2], info->player);
                    if (pad_ != nullptr && SDL_SetGamepadLED(pad_, info->light[0], info->light[1], info->light[2])) ++feedback_.lights_passed;
                }
            } else if (item.kind == transport::data_type::kTriggerEffects) {
                const auto effects = transport::parse_trigger_effects(item.bytes);
                if (!effects) continue;
                ++feedback_.trigger_effects;
                log::debug("play: the console sets the triggers' effects: left type {:#04x}, right type {:#04x}", effects->left_type, effects->right_type);
                // Only a DualSense has triggers that take them.
                if (pad_ != nullptr && pad_is_dualsense_) {
                    const auto report = media::dualsense_trigger_effects(effects->left_type, effects->left, effects->right_type, effects->right);
                    if (SDL_SendGamepadEffect(pad_, report.data(), static_cast<int>(report.size()))) {
                        ++feedback_.trigger_effects_passed;
                        triggers_set_ = true;
                    }
                }
            }
        }
    }
    // True when the console is to be asked for a keyframe now.
    bool wants_keyframe() {
        if (!keyframe_needed_.exchange(false)) return false;
        const auto now = Clock::now();
        if (last_keyframe_ask_ && now - *last_keyframe_ask_ < kKeyframeAskEvery) return false;
        last_keyframe_ask_ = now;
        return true;
    }
    bool controller(transport::Buttons& buttons, transport::InputState& state) {
        const std::lock_guard lock(input_mutex_);
        if (!std::exchange(input_changed_, false)) return false;
        buttons = buttons_;
        state = state_;
        return true;
    }

    // ---- on the window's thread ----
    std::optional<media::Picture> next_picture() {
        const std::lock_guard lock(picture_mutex_);
        if (!std::exchange(fresh_, false)) return std::nullopt;
        ++shown_;
        return latest_;
    }
    void on_event(const SDL_Event& event);

    // ---- from any thread ----
    void wait_for_stream_or(const std::atomic<bool>& done) const {
        while (!started_.load() && !done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Only after finish(), and when the session has ended.
    [[nodiscard]] std::vector<std::string> report();

private:
    struct Job {
        std::vector<std::uint8_t> header;
        unsigned size_index = 0;
        std::uint16_t index = 0;
        std::uint32_t lost_before = 0;
        Clock::time_point arrived{};
        double since_arrival_ms = 0.0;  // since the frame before it arrived
        std::vector<std::uint8_t> data;
    };
    void decode_loop();
    void note_handed(Clock::time_point now);
    void play_sound(const proto::AudioHeader& format, const transport::SoundFrame& frame);
    void set_button(transport::Button button, bool down) {
        const std::lock_guard lock(input_mutex_);
        if (buttons_.is_pressed(button) == down) return;
        buttons_.set(button, down);
        input_changed_ = true;
    }

    // Frames waiting for the decoder.
    std::mutex jobs_mutex_;
    std::condition_variable jobs_wake_;
    std::deque<Job> jobs_;
    bool closing_ = false;
    std::size_t longest_queue_ = 0;
    std::optional<Clock::time_point> last_arrival_;  // on the session's thread
    Spacing arrival_spacing_;                        // on the session's thread, until it has ended
    bool late_before_ = false;
    std::uint64_t late_on_the_way_ = 0, late_sent_ = 0;
    Spacing handed_spacing_;                         // on the decode thread, until it has ended
    std::optional<Clock::time_point> last_keyframe_ask_;
    std::atomic<bool> keyframe_needed_{false};

    // The decoder's side: only the decode thread touches these until it has ended.
    std::optional<media::VideoDecoder> decoder_;
    std::optional<unsigned> size_index_;
    std::string decoder_text_, decoder_error_;
    std::uint64_t frames_ = 0, pictures_ = 0, held_back_ = 0, refused_ = 0, microseconds_ = 0, slowest_ = 0;
    std::uint64_t changed_ = 0, approximate_ = 0, undecodable_ = 0;
    std::string unfollowed_;
    void take_counters() {
        const media::VideoDecoderCounters& counters = decoder_->counters();
        microseconds_ += counters.microseconds;
        slowest_ = std::max(slowest_, counters.slowest_microseconds);
        changed_ += counters.changed;
        approximate_ += counters.approximate;
        undecodable_ += counters.undecodable;
        unfollowed_ = decoder_->unfollowed_reason();
    }
    // When to show each picture. Set before the first frame is there; the decode thread's from then on.
    std::optional<media::FramePacer> pacer_;
    std::uint64_t paced_ = 0, paced_late_ = 0, paced_microseconds_ = 0, paced_most_microseconds_ = 0;
    // When the picture stood, and what was seen to go with it.
    std::optional<Clock::time_point> first_handed_, last_handed_;
    std::uint64_t stood_ = 0, stood_lost_ = 0, stood_arrival_ = 0, stood_decoder_ = 0, stood_other_ = 0;
    double longest_stood_ms_ = 0.0;
    std::uint64_t not_shown_since_ = 0, lost_since_ = 0;
    double longest_arrival_since_ms_ = 0.0, longest_behind_since_ms_ = 0.0;

    // The newest picture, for the window.
    std::mutex picture_mutex_;
    std::optional<media::Picture> latest_;
    bool fresh_ = false;
    std::uint64_t shown_ = 0, replaced_ = 0;

    // Sound: only the session's thread touches these.
    std::optional<media::SoundDecoder> sound_;
    // The device's own thread takes the sound from the line as it needs it and decodes it there;
    // the session's thread puts the frames in. The lock is for the line; the decoder is the device thread's.
    SDL_AudioStream* audio_ = nullptr;
    bool sound_failed_ = false;
    std::mutex sound_mutex_;
    media::SoundBuffer sound_line_;
    double sound_speed_ = 1.0;        // on the device's thread
    std::vector<std::int16_t> silence_;
    static void SDLCALL sound_wanted(void* self, SDL_AudioStream* stream, int additional, int total);
    void fill_sound(SDL_AudioStream* stream, int additional);

    // What the console sent for the controller. With `pad_`, under the pad's lock: the session's
    // thread uses the controller for this while the window's thread opens and closes it.
    struct Feedback {
        std::uint64_t rumbles = 0, rumbles_passed = 0, lights = 0, lights_passed = 0, infos = 0, motion_resets = 0, trigger_effects = 0;
        std::uint64_t trigger_effects_passed = 0;
        std::uint8_t strongest_left = 0, strongest_right = 0;
        std::optional<std::array<std::uint8_t, 3>> light;
        std::uint8_t player = 0, haptics_strength = 0, triggers_strength = 0;
    };
    std::mutex pad_mutex_;
    Feedback feedback_;
    bool pad_is_dualsense_ = false;
    bool triggers_set_ = false;
    // To be called with the pad's lock held, before the controller is closed: its triggers as they were.
    void release_triggers() {
        if (pad_ == nullptr || !std::exchange(triggers_set_, false)) return;
        const auto off = media::dualsense_triggers_off();
        SDL_SendGamepadEffect(pad_, off.data(), static_cast<int>(off.size()));
    }

    // Haptics, on the session's thread: to the controller's own sound device where it has one
    // (a DualSense on USB), and as rumble otherwise.
    void play_haptics(const transport::SoundFrame& frame);
    void open_haptics_device();
    int haptics_volume_ = 100;  // percent, for the controller's actuator channels
    // What a controller's two back buttons are to the console; set before the window runs.
    std::optional<transport::Button> back_left_, back_right_;
    std::uint64_t back_presses_ = 0;
    // The controller's battery, as it was last heard of; on the window's thread.
    std::string battery_text_;
    bool battery_warned_ = false;
    void note_battery();
    bool haptics_looked_ = false;
    SDL_AudioStream* haptics_audio_ = nullptr;
    std::string haptics_device_;
    media::HapticsUpsampler haptics_upsampler_;
    media::HapticsLevel haptics_peak_, haptics_rumble_;
    unsigned haptics_since_rumble_ = 0;
    std::uint64_t haptics_frames_ = 0, haptics_played_ = 0, haptics_rumbled_ = 0, haptics_unusable_ = 0, haptics_let_go_ = 0;

    // The controller.
    std::mutex input_mutex_;
    transport::Buttons buttons_;
    transport::InputState state_;
    bool input_changed_ = false;
    SDL_Gamepad* pad_ = nullptr;
    SDL_JoystickID pad_id_ = 0;
    std::uint8_t next_touch_ = 0;
    std::string pad_name_;

    std::atomic<bool> started_{false};
    std::thread decode_thread_;  // last: it starts with everything above in place
};

void Player::decode_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(jobs_mutex_);
            jobs_wake_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
            if (closing_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        ++frames_;
        if (size_index_ != job.size_index) {
            // A new picture size is a new stream to the decoder: its header first, then a keyframe.
            size_index_ = job.size_index;
            if (decoder_) take_counters();
            decoder_.reset();
            const auto codec = media::codec_of_header(job.header);
            if (!codec) {
                decoder_error_ = std::format("the picture header of {} bytes is neither H.264's nor HEVC's", job.header.size());
            } else if (auto opened = media::VideoDecoder::open(*codec, job.header)) {
                decoder_.emplace(std::move(*opened));
                decoder_text_ = decoder_->description();
                log::info("play: decoding with {}", decoder_text_);
            } else {
                decoder_error_ = opened.error();
            }
            if (!decoder_) log::error("play: pictures cannot be decoded: {}", decoder_error_);
        }
        if (!decoder_) {
            ++refused_;
            continue;
        }
        lost_since_ += job.lost_before;
        longest_arrival_since_ms_ = std::max(longest_arrival_since_ms_, job.since_arrival_ms);
        auto decoded = decoder_->decode(job.index, job.data);
        // No frame the decoder holds leads to this one: only a keyframe gets the picture going again.
        if (decoded.needs_keyframe) keyframe_needed_.store(true);
        std::optional<media::Picture>& picture = decoded.picture;
        if (!picture) {
            if (!decoded.needs_keyframe) ++refused_;
            ++not_shown_since_;
            continue;
        }
        ++pictures_;
        if (picture->damaged) {
            // Built on a frame that did not arrive, in a stream whose frames are not followed: the last
            // whole picture stays up until the console's repair, instead of a picture with holes in it.
            ++held_back_;
            ++not_shown_since_;
            continue;
        }
        const auto ready = Clock::now();
        longest_behind_since_ms_ = std::max(longest_behind_since_ms_, std::chrono::duration<double, std::milli>(ready - job.arrived).count());
        if (pacer_) {
            std::unique_lock lock(jobs_mutex_);
            // With frames already waiting behind this one the stream is behind (a bunch came after a pause):
            // the picture is shown at once, so that the pause does not stay as delay.
            const media::FramePacer::Slot slot = pacer_->schedule(ready, !jobs_.empty());
            if (slot.late) ++paced_late_;
            if (slot.at > ready) {
                ++paced_;
                const auto wait = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(slot.at - ready).count());
                paced_microseconds_ += wait;
                paced_most_microseconds_ = std::max(paced_most_microseconds_, wait);
                if (jobs_wake_.wait_until(lock, slot.at, [&] { return closing_; })) return;
            }
        }
        const auto now = Clock::now();
        note_handed(now);
        {
            const std::lock_guard lock(picture_mutex_);
            if (fresh_) ++replaced_;
            latest_ = std::move(*picture);
            fresh_ = true;
        }
        render::wake_window();
    }
}

// A picture goes to the window: was the one before it up for longer than it should have been, and why.
void Player::note_handed(Clock::time_point now) {
    if (!first_handed_) first_handed_ = now;
    if (last_handed_) {
        const double stood_ms = std::chrono::duration<double, std::milli>(now - *last_handed_).count();
        handed_spacing_.add(stood_ms);
        if (stood_ms > kStoodMs) {
            ++stood_;
            longest_stood_ms_ = std::max(longest_stood_ms_, stood_ms);
            std::string causes;
            const auto cause = [&](std::string text) { causes += (causes.empty() ? "" : "; ") + std::move(text); };
            if (lost_since_ != 0) cause(std::format("{} frame{} lost", lost_since_, lost_since_ == 1 ? "" : "s"));
            if (not_shown_since_ != 0) cause(std::format("{} frame{} could not be shown", not_shown_since_, not_shown_since_ == 1 ? "" : "s"));
            if (longest_arrival_since_ms_ > kArrivedLateMs) cause(std::format("no frame arrived for {:.0f} ms", longest_arrival_since_ms_));
            if (longest_behind_since_ms_ > kDecoderBehindMs) cause(std::format("a frame waited {:.0f} ms for the decoder", longest_behind_since_ms_));
            // One cause is counted, the first that applies: a loss comes with a pause more often than not.
            if (lost_since_ != 0 || not_shown_since_ != 0) ++stood_lost_;
            else if (longest_arrival_since_ms_ > kArrivedLateMs) ++stood_arrival_;
            else if (longest_behind_since_ms_ > kDecoderBehindMs) ++stood_decoder_;
            else ++stood_other_;
            log::debug("play: {:.1f} s: the picture stood for {:.0f} ms ({})", std::chrono::duration<double>(now - *first_handed_).count(), stood_ms,
                       causes.empty() ? "nothing seen that explains it" : causes);
        }
    }
    last_handed_ = now;
    lost_since_ = not_shown_since_ = 0;
    longest_arrival_since_ms_ = longest_behind_since_ms_ = 0.0;
}

void Player::play_sound(const proto::AudioHeader& format, const transport::SoundFrame& frame) {
    if (sound_failed_) return;
    if (!sound_) {
        auto opened = media::SoundDecoder::open(format.rate, format.channels, format.frame_size);
        if (!opened) {
            log::error("play: sound cannot be decoded: {}", opened.error());
            sound_failed_ = true;
            return;
        }
        sound_.emplace(std::move(*opened));
        const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(format.channels), static_cast<int>(format.rate)};
        silence_.assign(std::size_t{format.frame_size} * format.channels, 0);
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Player::sound_wanted, this);
        if (audio_ == nullptr || !SDL_ResumeAudioStreamDevice(audio_)) {
            log::warn("play: no sound: the sound device could not be opened: {}", SDL_GetError());
            sound_failed_ = true;
            return;
        }
        log::info("play: sound through SDL's {} driver, {} Hz, {} channels", SDL_GetCurrentAudioDriver(), format.rate, format.channels);
    }
    const std::lock_guard lock(sound_mutex_);
    sound_line_.push(std::vector<std::uint8_t>(frame.data.begin(), frame.data.end()), frame.lost_before, Clock::now());
    // Every ten seconds of sound, how the line is doing: for the log's file.
    const media::SoundBuffer::Counters& line = sound_line_.counters();
    if (line.arrived % 1000 == 0) {
        log::debug("play: sound: {} frames in line (aimed at {}, the device takes {} at a time); so far it ran empty {} times, {} frames' time was "
                   "continued, {} silent, {} frames came too late",
                   sound_line_.depth(), sound_line_.aim(), sound_line_.period(), line.ran_empty, line.concealed, line.silent, line.let_go);
    }
}

void SDLCALL Player::sound_wanted(void* self, SDL_AudioStream* stream, int additional, int /*total*/) {
    static_cast<Player*>(self)->fill_sound(stream, additional);
}

// On the sound device's thread, whenever it needs more than the stream holds: a frame's worth at a time.
void Player::fill_sound(SDL_AudioStream* stream, int additional) {
    if (additional <= 0 || silence_.empty()) return;
    {
        const int frame_bytes = static_cast<int>(silence_.size() * sizeof(std::int16_t));
        const std::lock_guard lock(sound_mutex_);
        sound_line_.begin_period(static_cast<std::size_t>((additional + frame_bytes - 1) / frame_bytes));
    }
    while (additional > 0) {
        media::SoundBuffer::Next next;
        double speed = 1.0;
        {
            const std::lock_guard lock(sound_mutex_);
            next = sound_line_.pull(Clock::now());
            speed = sound_line_.speed();
        }
        std::span<const std::int16_t> samples = silence_;
        if (next.kind == media::SoundBuffer::Kind::frame) samples = sound_->decode(next.frame);
        else if (next.kind == media::SoundBuffer::Kind::concealed) samples = sound_->conceal();
        SDL_PutAudioStreamData(stream, samples.data(), static_cast<int>(samples.size_bytes()));
        additional -= static_cast<int>(samples.size_bytes());
        // A line that is longer or shorter than aimed for is brought back by playing a little
        // faster or slower, which is not heard.
        if (std::abs(speed - sound_speed_) >= kSpeedStep || (speed == 1.0 && sound_speed_ != 1.0)) {
            sound_speed_ = speed;
            SDL_SetAudioStreamFrequencyRatio(stream, static_cast<float>(speed));
        }
    }
}

// On the window's thread, with the controller open: how full its battery is and whether it charges.
void Player::note_battery() {
    int percent = -1;
    const SDL_PowerState state = SDL_GetGamepadPowerInfo(pad_, &percent);
    const char* how = state == SDL_POWERSTATE_CHARGING ? "charging" : state == SDL_POWERSTATE_CHARGED ? "full, on the cable"
                      : state == SDL_POWERSTATE_ON_BATTERY ? "on the battery" : state == SDL_POWERSTATE_NO_BATTERY ? "on the cable, no battery" : "";
    if (percent < 0 && *how == '\0') {
        battery_text_.clear();
        return;
    }
    battery_text_ = percent >= 0 ? std::format("{}%{}{}", percent, *how != '\0' ? ", " : "", how) : std::string(how);
    // Once per session: a controller that runs out in the middle of a game.
    if (state == SDL_POWERSTATE_ON_BATTERY && percent >= 0 && percent <= 15 && !std::exchange(battery_warned_, true)) {
        log::warn("play: the controller's battery is at {}%", percent);
    }
}

void Player::open_haptics_device() {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; devices != nullptr && i < count && haptics_audio_ == nullptr; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (name == nullptr || SDL_strcasestr(name, "DualSense") == nullptr) continue;
        // Its four channels: the speaker's two, then the two actuators.
        const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(media::HapticsUpsampler::kChannels), static_cast<int>(media::HapticsUpsampler::kRate)};
        haptics_audio_ = SDL_OpenAudioDeviceStream(devices[i], &spec, nullptr, nullptr);
        if (haptics_audio_ != nullptr && !SDL_ResumeAudioStreamDevice(haptics_audio_)) {
            SDL_DestroyAudioStream(haptics_audio_);
            haptics_audio_ = nullptr;
        }
        if (haptics_audio_ != nullptr) haptics_device_ = name;
    }
    SDL_free(devices);
    if (haptics_audio_ != nullptr) {
        log::info("play: haptics go to the controller's own sound device ({})", haptics_device_);
        // A desktop gives a new sound device a fraction of its volume (40% is a sixteenth of the swing); the
        // strength of the haptics is this program's setting, and the actuators' two channels are set to it.
        const HapticsVolume volume = set_haptics_volume("DualSense", haptics_volume_);
        if (volume.changed) {
            log::info("play: the volume of the controller's two haptics channels was {}%; it is set to {}% (--haptics-volume; its speaker's volume is "
                      "left as it is)",
                      volume.before, haptics_volume_);
        } else if (volume.found && volume.problem.empty()) {
            log::info("play: the controller's haptics channels are at {}% of their volume, as set", volume.before);
        } else {
            log::warn("play: the volume of the controller's haptics channels could not be {}: {}; if haptics are weak, set the volume of \"{}\" "
                      "in the desktop's sound settings",
                      volume.found ? "set" : "looked at", volume.problem.empty() ? "no such device was listed" : volume.problem, haptics_device_);
        }
    }
    else log::info("play: no sound device of a DualSense is there (it has one on USB only): haptics become rumble");
}

void Player::play_haptics(const transport::SoundFrame& frame) {
    ++haptics_frames_;
    if (!std::exchange(haptics_looked_, true)) open_haptics_device();
    if (haptics_audio_ != nullptr) {
        const auto samples = haptics_upsampler_.frame(frame.data);
        if (samples.empty()) {
            ++haptics_unusable_;
            return;
        }
        const int bytes_per_ms = static_cast<int>(media::HapticsUpsampler::kRate * media::HapticsUpsampler::kChannels * sizeof(std::int16_t) / 1000);
        if (SDL_GetAudioStreamQueued(haptics_audio_) > kMaxHapticsQueuedMs * bytes_per_ms) {
            // A bunch after a pause on the network: a shake that comes late is worse than none.
            SDL_ClearAudioStream(haptics_audio_);
            ++haptics_let_go_;
        }
        SDL_PutAudioStreamData(haptics_audio_, samples.data(), static_cast<int>(samples.size_bytes()));
        ++haptics_played_;
        return;
    }
    const auto level = media::haptics_level(frame.data);
    if (!level) {
        ++haptics_unusable_;
        return;
    }
    haptics_peak_.left = std::max(haptics_peak_.left, level->left);
    haptics_peak_.right = std::max(haptics_peak_.right, level->right);
    if (++haptics_since_rumble_ < kHapticsFramesPerRumble) return;
    const media::HapticsLevel peak = std::exchange(haptics_peak_, {});
    haptics_since_rumble_ = 0;
    // Nothing to say while the controller rests and is to go on resting.
    if (peak == media::HapticsLevel{} && haptics_rumble_ == media::HapticsLevel{}) return;
    haptics_rumble_ = peak;
    const std::lock_guard lock(pad_mutex_);
    if (pad_ != nullptr && SDL_RumbleGamepad(pad_, static_cast<Uint16>(peak.left * 257), static_cast<Uint16>(peak.right * 257), kHapticsRumbleLifeMs)) ++haptics_rumbled_;
}

void Player::on_event(const SDL_Event& event) {
    using transport::Button;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (pad_ == nullptr) {
            const std::lock_guard pad_lock(pad_mutex_);
            pad_ = SDL_OpenGamepad(event.gdevice.which);
            if (pad_ != nullptr) {
                pad_is_dualsense_ = SDL_GetGamepadType(pad_) == SDL_GAMEPAD_TYPE_PS5;
                // The light bar's color, if the console named one before the controller was there.
                if (feedback_.light && SDL_SetGamepadLED(pad_, (*feedback_.light)[0], (*feedback_.light)[1], (*feedback_.light)[2])) ++feedback_.lights_passed;
                pad_id_ = event.gdevice.which;
                const char* name = SDL_GetGamepadName(pad_);
                pad_name_ = name != nullptr ? name : "a controller";
                // Motion, where the controller has it.
                const bool gyro = SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_GYRO, true);
                const bool accel = SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_ACCEL, true);
                log::info("play: controller: {}{}{}", pad_name_, SDL_GetNumGamepadTouchpads(pad_) > 0 ? ", with a touchpad" : "",
                          gyro && accel ? ", with motion sensors" : "");
                note_battery();
                if (!battery_text_.empty()) log::info("play: the controller's battery: {}", battery_text_);
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ != nullptr && event.gdevice.which == pad_id_) {
            {
                const std::lock_guard pad_lock(pad_mutex_);
                release_triggers();
                SDL_CloseGamepad(pad_);
                pad_ = nullptr;
            }
            log::info("play: the controller is gone");
            // Nothing stays pressed.
            const std::lock_guard lock(input_mutex_);
            buttons_ = transport::Buttons{};
            state_ = transport::InputState{};
            input_changed_ = true;
        }
        break;
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
        if (pad_ != nullptr && event.jbattery.which == pad_id_) note_battery();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (event.gbutton.which != pad_id_) break;
        std::optional<Button> button;
        switch (event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: button = Button::cross; break;
        case SDL_GAMEPAD_BUTTON_EAST: button = Button::circle; break;
        case SDL_GAMEPAD_BUTTON_WEST: button = Button::square; break;
        case SDL_GAMEPAD_BUTTON_NORTH: button = Button::triangle; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = Button::left; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = Button::right; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: button = Button::up; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = Button::down; break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: button = Button::l1; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: button = Button::r1; break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: button = Button::l3; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: button = Button::r3; break;
        case SDL_GAMEPAD_BUTTON_START: button = Button::options; break;
        case SDL_GAMEPAD_BUTTON_BACK: button = Button::share; break;
        case SDL_GAMEPAD_BUTTON_GUIDE: button = Button::ps; break;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: button = Button::touchpad; break;
        // A DualSense Edge's back buttons: what the user said they are.
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
            button = back_left_;
            if (event.gbutton.down) ++back_presses_;
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
            button = back_right_;
            if (event.gbutton.down) ++back_presses_;
            break;
        default: break;
        }
        if (button) set_button(*button, event.gbutton.down);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.which != pad_id_) break;
        const std::lock_guard lock(input_mutex_);
        const std::int16_t value = event.gaxis.value;
        // A trigger goes from 0 to 32767; the console takes 0 to 255.
        const auto pull = static_cast<std::uint8_t>(std::clamp(static_cast<int>(value), 0, 32767) >> 7);
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: state_.left_x = value; break;
        case SDL_GAMEPAD_AXIS_LEFTY: state_.left_y = value; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: state_.right_x = value; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: state_.right_y = value; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: buttons_.l2 = pull; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: buttons_.r2 = pull; break;
        default: break;
        }
        input_changed_ = true;
        break;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
        if (event.gtouchpad.which != pad_id_ || event.gtouchpad.finger < 0 || event.gtouchpad.finger > 1) break;
        const std::lock_guard lock(input_mutex_);
        transport::Touch& touch = buttons_.touches[static_cast<std::size_t>(event.gtouchpad.finger)];
        if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
            touch.id = next_touch_;
            next_touch_ = static_cast<std::uint8_t>((next_touch_ + 1) & 0x7f);
        }
        touch.down = event.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP;
        touch.x = static_cast<std::uint16_t>(std::clamp(event.gtouchpad.x, 0.0F, 1.0F) * kTouchWidth);
        touch.y = static_cast<std::uint16_t>(std::clamp(event.gtouchpad.y, 0.0F, 1.0F) * kTouchHeight);
        input_changed_ = true;
        break;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (event.gsensor.which != pad_id_) break;
        const std::lock_guard lock(input_mutex_);
        if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
            state_.gyro_x = event.gsensor.data[0];
            state_.gyro_y = event.gsensor.data[1];
            state_.gyro_z = event.gsensor.data[2];
        } else if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
            // From meters per second squared to multiples of the earth's pull.
            state_.accel_x = event.gsensor.data[0] / SDL_STANDARD_GRAVITY;
            state_.accel_y = event.gsensor.data[1] / SDL_STANDARD_GRAVITY;
            state_.accel_z = event.gsensor.data[2] / SDL_STANDARD_GRAVITY;
        }
        input_changed_ = true;
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        // Without a controller: enough keys to get around a console's menus.
        if (event.key.repeat) break;
        std::optional<Button> button;
        switch (event.key.key) {
        case SDLK_LEFT: button = Button::left; break;
        case SDLK_RIGHT: button = Button::right; break;
        case SDLK_UP: button = Button::up; break;
        case SDLK_DOWN: button = Button::down; break;
        case SDLK_RETURN: button = Button::cross; break;
        case SDLK_BACKSPACE: button = Button::circle; break;
        case SDLK_T: button = Button::triangle; break;
        case SDLK_S: button = Button::square; break;
        case SDLK_O: button = Button::options; break;
        case SDLK_ESCAPE: button = Button::ps; break;
        default: break;
        }
        if (button) set_button(*button, event.key.down);
        break;
    }
    default: break;
    }
}

std::vector<std::string> Player::report() {
    std::vector<std::string> lines;
    if (decoder_) take_counters();
    const std::uint64_t microseconds = microseconds_, slowest = slowest_;
    if (!decoder_error_.empty()) lines.push_back(std::format("Pictures could not be decoded: {}.", decoder_error_));
    if (frames_ != 0 && decoder_error_.empty()) {
        lines.push_back(std::format("Played ({}): {} pictures decoded from {} frames, {} shown, {} replaced by a newer one before the screen took them, "
                                    "{} held back as damaged, {} frames gave no picture; {:.2f} ms a frame, {:.2f} ms the slowest; most frames waiting for "
                                    "the decoder: {}.",
                                    decoder_text_, pictures_, frames_, shown_, replaced_, held_back_, refused_,
                                    static_cast<double>(microseconds) / 1000.0 / static_cast<double>(std::max<std::uint64_t>(frames_, 1)),
                                    static_cast<double>(slowest) / 1000.0, longest_queue_));
        if (unfollowed_.empty()) {
            lines.push_back(std::format("Behind lost frames: {} frames were decoded from the nearest frame held in place of a lost one, {} pictures shown "
                                        "were built on those until the console's repair, {} frames could not be decoded from anything held.",
                                        changed_, approximate_, undecodable_));
        } else {
            lines.push_back(std::format("Behind lost frames the picture stands until the console's repair: {}.", unfollowed_));
        }
        lines.push_back(std::format("Frames arrived this far apart: {}.", arrival_spacing_.text()));
        lines.push_back(std::format("Frames that came late with none lost: {} were held up on the way (the next one came right behind), {} the console "
                                    "sent late (the next one kept its distance: the game's own pace).",
                                    late_on_the_way_, late_sent_));
        lines.push_back(std::format("Pictures went to the screen this far apart: {}.", handed_spacing_.text()));
        if (pacer_) {
            lines.push_back(std::format("Pacing: {} of {} pictures were held back to follow the one before by a frame's time, by {:.1f} ms on average "
                                        "and {:.1f} ms at most; {} had newer frames waiting behind them (a bunch after a pause) and were shown at once.",
                                        paced_, pictures_, static_cast<double>(paced_microseconds_) / 1000.0 / static_cast<double>(std::max<std::uint64_t>(paced_, 1)),
                                        static_cast<double>(paced_most_microseconds_) / 1000.0, paced_late_));
        } else {
            lines.emplace_back("Pacing: off; every picture was shown the moment it was there.");
        }
        lines.push_back(std::format("Smoothness: the picture stood for over {:.0f} ms {} times (the longest: {:.0f} ms): {} with frames lost or not "
                                    "shown, {} after a pause in arriving, {} with the decoder behind, {} unexplained.",
                                    kStoodMs, stood_, longest_stood_ms_, stood_lost_, stood_arrival_, stood_decoder_, stood_other_));
    }
    if (sound_) {
        media::SoundBuffer::Counters line;
        std::size_t aim = 0;
        {
            const std::lock_guard lock(sound_mutex_);
            line = sound_line_.counters();
            aim = sound_line_.aim();
        }
        lines.push_back(std::format("Sound: {} frames played, {} filled in for frames that did not arrive, {} left out to keep up with the picture{}; "
                                    "the line of frames ran empty {} times: the sound before was continued for {} frames' time and {} were silent; "
                                    "the device took up to {} frames at a time, and {} to {} waited when it did ({:.1f} on average, aimed at {} "
                                    "in the end; a frame is 10 ms).",
                                    line.played, line.filled, line.let_go, audio_ == nullptr ? " (no sound device)" : "", line.ran_empty, line.concealed,
                                    line.silent, line.largest_period, line.least_depth, line.most_depth,
                                    static_cast<double>(line.depth_sum) / static_cast<double>(std::max<std::uint64_t>(line.periods, 1)), aim));
    }
    lines.push_back(pad_name_.empty() ? std::string("Controller: none was connected (the keyboard's arrow keys, Enter and Backspace stood in).")
                                      : std::format("Controller: {}{}{}.", pad_name_, battery_text_.empty() ? "" : "; battery: ", battery_text_));
    if (back_left_ || back_right_ || back_presses_ != 0) {
        lines.push_back(std::format("Back buttons: the left one is {}, the right one is {}; they were pressed {} times.",
                                    back_left_ ? transport::button_name(*back_left_) : std::string_view("nothing"),
                                    back_right_ ? transport::button_name(*back_right_) : std::string_view("nothing"), back_presses_));
    }
    {
        const std::lock_guard lock(pad_mutex_);
        const Feedback& f = feedback_;
        lines.push_back(std::format("For the controller the console sent: rumble {} times ({} reached the controller; the strongest {} and {} of 255), a "
                                    "light bar color {} times ({} reached it{}), trigger effects {} times ({} reached a DualSense), and {} frames of "
                                    "haptics ({} played on the controller's own sound device{}, {} times turned into rumble, {} let go as too late, {} "
                                    "unusable); controller information {} times (player {}, haptics strength {}, trigger strength {}, {} asked to take "
                                    "the controller's position as its rest).",
                                    f.rumbles, f.rumbles_passed, f.strongest_left, f.strongest_right, f.lights, f.lights_passed,
                                    f.light ? std::format("; the last: {}, {}, {}", (*f.light)[0], (*f.light)[1], (*f.light)[2]) : std::string(),
                                    f.trigger_effects, f.trigger_effects_passed, haptics_frames_, haptics_played_,
                                    haptics_device_.empty() ? std::string() : std::format(" \"{}\"", haptics_device_), haptics_rumbled_, haptics_let_go_,
                                    haptics_unusable_, f.infos, f.player, f.haptics_strength, f.triggers_strength, f.motion_resets));
    }
    return lines;
}

}  // namespace

int run_player(SessionPlan plan, const PlayOptions& options) {
    // Sound and controllers, beside the window's own start of the picture side.
    SDL_SetAppMetadata(PSVP_NAME, PSVP_VERSION, PSVP_APP_ID);
    // Ten milliseconds of sound at a time, a console's frame: a larger piece is that much more delay.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) log::warn("play: no sound: {}", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log::warn("play: no controllers: {}", SDL_GetError());

    // What a PS5 is told the controller is, when the session starts: a DualSense when one is
    // connected then. To it the console sends that controller's haptics and its triggers' effects.
    // A DualSense that is plugged in later is used as any controller.
    plan.dualsense = options.controller == PlayOptions::Controller::dualsense;
    if (options.controller == PlayOptions::Controller::by_what_is_connected) {
        SDL_UpdateGamepads();
        int count = 0;
        if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) plan.dualsense = plan.dualsense || SDL_GetGamepadTypeForID(pads[i]) == SDL_GAMEPAD_TYPE_PS5;
            SDL_free(pads);
        }
    }

    int window_code = 0;
    int session_code = 0;
    {
        Player player(static_cast<double>(plan.video.max_fps), options.pacing, options.haptics_volume, options.back_left, options.back_right);
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        plan.on_media = [&](transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) { player.on_media(media, info); };
        plan.controller = [&](transport::Buttons& buttons, transport::InputState& state) { return player.controller(buttons, state); };
        plan.wants_keyframe = [&] { return player.wants_keyframe(); };
        plan.on_console_data = [&](const std::vector<transport::ConsoleData>& data, std::chrono::microseconds) { player.on_console_data(data); };
        plan.stop = [&] { return stop.load(); };
        std::thread session([&] {
            session_code = run_session(plan);
            done.store(true);
            render::wake_window();
        });

        // The window opens when there is a stream to show; a session that fails before needs none.
        player.wait_for_stream_or(done);
        if (!done.load()) {
            render::WindowOptions window;
            window.frame_limit = options.frames;
            window.dialogs = !options.frames.has_value();
            window.next_picture = [&] { return player.next_picture(); };
            window.finished = [&] { return done.load(); };
            window.on_event = [&](const SDL_Event& event) { player.on_event(event); };
            if (const auto file = window_place_file()) {
                window.place = load_window_place(*file);
                window.on_place = [file = *file](const WindowPlace& place) {
                    if (!save_window_place(file, place)) log::warn("window: its size could not be kept in {}", file.string());
                };
            }
            window_code = render::run_window(window);
        }
        stop.store(true);
        session.join();
        player.finish();
        for (const std::string& line : player.report()) {
            std::printf("%s\n", line.c_str());
            log::record(line);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (SDL_WasInit(0) == 0) SDL_Quit();
    return window_code != 0 ? window_code : session_code;
}

}  // namespace psvp::app
