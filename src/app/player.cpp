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

#include "app/controller_list.h"
#include "app/controller_presses.h"
#include "app/stand_in_touch.h"
#include "app/haptics_volume.h"
#include "app/log.h"
#include "app/pretend.h"
#include "app/stream_menu.h"
#include "app/window_place.h"
#include "media/sound_decoder.h"
#include "media/frame_pacer.h"
#include "media/haptics.h"
#include "media/sound_buffer.h"
#include "proto/controller_feedback.h"
#include "proto/motion.h"
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
constexpr std::uint64_t kShareTapMs = 80;  // how long share stays pressed when it stood behind the d-pad's swipes

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
    // With a `cushion_ms`, that much delay is kept so that frames which come that much late are shown in place.
    // With `gpu_decoding`, the decoder waits for the window to say which graphics device to decode on.
    Player(double frames_per_second, bool pacing, int cushion_ms, int haptics_volume, std::optional<transport::Button> back_left,
           std::optional<transport::Button> back_right, bool gpu_decoding)
        : haptics_volume_(haptics_volume), back_left_(back_left), back_right_(back_right), decode_thread_([this] { decode_loop(); }) {
        const std::lock_guard lock(jobs_mutex_);
        if (pacing) pacer_.emplace(frames_per_second, std::chrono::milliseconds(cushion_ms));
        if (!gpu_decoding) {
            gpu_decided_ = true;
            gpu_why_not_ = "software was asked for";
        }
    }
    // ---- on the window's thread ----
    // The device the window lends for decoding, or nothing and why not. Frames wait for this.
    void set_gpu(std::optional<media::GpuDevice> gpu, const std::string& why_not) {
        {
            const std::lock_guard lock(jobs_mutex_);
            // Software was asked for: decided at the start, and that is the reason that stands.
            if (gpu_decided_) return;
            gpu_ = std::move(gpu);
            gpu_why_not_ = why_not;
            gpu_decided_ = true;
        }
        jobs_wake_.notify_all();
    }
    // Before the window's device goes: decoding ends, and every picture decoded on it is let go.
    // What the user chose for the controller; before the session starts.
    void set_controller_settings(const ControllerSettings& settings) { settings_ = settings; }
    // The window's line about when pictures reached the screen (the driver's own times), on its thread
    // before the report is made on the same one.
    void set_screen_times(const std::string& line) { screen_times_ = line; }
    // The window's line about the screen (render::WindowOptions::on_screen_state), for the statistics.
    void set_screen_state(const std::string& line) {
        const std::lock_guard lock(stats_mutex_);
        screen_state_ = line;
    }
    void drop_gpu() {
        finish();
        if (decoder_) {
            take_counters();
            decoder_text_ = decoder_->description();
            decoder_.reset();
        }
        {
            const std::lock_guard lock(picture_mutex_);
            latest_.reset();
        }
        const std::lock_guard lock(jobs_mutex_);
        gpu_.reset();
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
        for (const auto& [id, pad] : others_) SDL_CloseGamepad(pad);
    }

    // ---- on the session's thread ----
    void on_media(transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) {
        if (!info) return;
        if (!media.video.empty() && reconnecting_.exchange(false)) {
            ++comebacks_;
            const double out = lost_at_ ? std::chrono::duration<double>(Clock::now() - *lost_at_).count() : 0.0;
            longest_out_s_ = std::max(longest_out_s_, out);
            log::info("play: the stream is back after {:.1f} s", out);
            render::wake_window();
        }
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
                if (pad_ != nullptr && SDL_RumbleGamepad(pad_, rumble_at(rumble->left, settings_.rumble), rumble_at(rumble->right, settings_.rumble),
                                                         rumble->left != 0 || rumble->right != 0 ? kRumbleLifeMs : 0)) {
                    ++feedback_.rumbles_passed;
                }
            } else if (item.kind == transport::data_type::kPadInfo) {
                const auto info = transport::parse_controller_info(item.bytes);
                if (!info) continue;
                ++feedback_.infos;
                if (info->reset_motion) {
                    ++feedback_.motion_resets;
                    motion_reset_wanted_.store(true);  // taken on the next motion reading, beside the rest of the input
                }
                // The player lights under the touchpad: the console's number is the one SDL takes (0 = the first player).
                if ((feedback_.infos == 1 || feedback_.player != info->player) && settings_.player_lights) {
                    log::debug("play: the console sets the player lights to player {}", info->player + 1);
                    if (pad_ != nullptr) SDL_SetGamepadPlayerIndex(pad_, info->player);
                }
                feedback_.player = info->player;
                feedback_.haptics_strength = info->haptics;
                feedback_.triggers_strength = info->triggers;
                if (feedback_.light != info->light) {
                    feedback_.light = info->light;
                    ++feedback_.lights;
                    log::debug("play: the console sets the light bar to {}, {}, {} (player {})", info->light[0], info->light[1], info->light[2], info->player);
                    // Unless the user chose the light bar themselves.
                    if (settings_.light_bar == LightBar::console && pad_ != nullptr && SDL_SetGamepadLED(pad_, info->light[0], info->light[1], info->light[2])) {
                        ++feedback_.lights_passed;
                    }
                }
            } else if (item.kind == transport::data_type::kTriggerEffects) {
                const auto effects = transport::parse_trigger_effects(item.bytes);
                if (!effects) continue;
                ++feedback_.trigger_effects;
                log::debug("play: the console sets the triggers' effects: left type {:#04x}, right type {:#04x}", effects->left_type, effects->right_type);
                // Only a DualSense has triggers that take them; and only while the user has them on.
                if (!settings_.trigger_effects) {
                    ++triggers_held_back_;
                } else if (pad_ != nullptr && pad_is_dualsense_) {
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
        if (latest_) {
            stats_width_ = latest_->width;
            stats_height_ = latest_->height;
        }
        return latest_;
    }
    // The statistics over the picture, while they are shown: worked out again twice a second.
    std::vector<std::string> stats_lines();
    // The menu over the stream, while it is open.
    std::optional<MenuView> menu_view() {
        if (!menu_.is_open()) return std::nullopt;
        menu_.set_state(stats_shown_.load(), fullscreen());
        return menu_.view();
    }
    // ---- on the session's thread ----
    // The connection was lost (`attempt` 1) or a new session to bring it back did not work (2 and on):
    // the window says so over the last picture, and the frames of the next stream go to a new decoder.
    void connection_lost(int attempt) {
        if (attempt == 1) {
            ++losses_;
            lost_at_ = Clock::now();
        }
        reconnect_try_.store(attempt);
        reconnecting_.store(true);
        {
            const std::lock_guard lock(jobs_mutex_);
            jobs_.clear();
            restart_stream_ = true;
        }
        log::warn("play: the connection was lost; trying to make it again (try {})", attempt);
        render::wake_window();
    }
    [[nodiscard]] bool reconnecting() const { return reconnecting_.load(); }
    // While a console that went to rest is woken between two tries.
    void set_waking(bool waking) {
        waking_.store(waking);
        if (waking) ++wakes_;
        render::wake_window();
    }
    [[nodiscard]] bool waking() const { return waking_.load(); }
    // On the window's thread: what is said over the picture while the connection is made again.
    std::string notice_text() const {
        if (!reconnecting_.load()) return {};
        if (waking_.load()) return "Connection lost. The console went to rest: waking it...";
        const int attempt = reconnect_try_.load();
        return attempt <= 1 ? std::string("Connection lost. Reconnecting...") : std::format("Connection lost. Reconnecting (try {})...", attempt);
    }

    // Chosen in the menu: the window closes; and the console is sent to rest mode.
    [[nodiscard]] bool quit_asked() const { return quit_asked_.load(); }
    [[nodiscard]] bool rest_asked() const { return rest_asked_.load(); }
    void show_stats(bool shown) { stats_shown_.store(shown); }
    void on_event(const SDL_Event& event);
    // For checks: presses the pretended controller as its file says. On the window's thread, as it goes round.
    void follow_taps() {
        tick_stand_in(SDL_GetTicks());
        if (!taps_ || pad_ == nullptr) return;
        taps_->follow(SDL_GetTicks());
    }
    void set_taps(const std::string& file) { taps_.emplace(file); }

    // ---- on the session's thread ----
    void on_quality(const proto::ConnectionQuality& quality) {
        const std::lock_guard lock(stats_mutex_);
        quality_ = quality;
    }

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

    // The connection lost and made again: the counts are the session thread's.
    std::atomic<bool> reconnecting_{false};
    std::atomic<bool> waking_{false};
    int wakes_ = 0;  // on the session's thread
    std::atomic<int> reconnect_try_{0};
    std::optional<Clock::time_point> lost_at_;
    std::uint64_t losses_ = 0, comebacks_ = 0;
    double longest_out_s_ = 0.0;
    bool restart_stream_ = false;  // under jobs_mutex_: the next frame begins a new stream

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
    std::string screen_times_;
    std::string screen_state_;  // under stats_mutex_
    ControllerSettings settings_;
    std::int16_t raw_left_x_ = 0, raw_left_y_ = 0, raw_right_x_ = 0, raw_right_y_ = 0;  // the sticks before the dead zone
    std::uint64_t triggers_held_back_ = 0, fn_presses_ = 0;                       // the driver's own times of the pictures on the screen, when the window closed
    std::optional<Clock::time_point> last_keyframe_ask_;
    std::atomic<bool> keyframe_needed_{false};

    // The graphics card to decode on: set by the window's thread under the jobs' lock, read by the decode thread.
    std::optional<media::GpuDevice> gpu_;
    bool gpu_decided_ = false;
    std::string gpu_why_not_;
    const media::GpuDevice* decode_gpu() {
        const std::lock_guard lock(jobs_mutex_);
        if (!gpu_ && !gpu_why_not_.empty() && !gpu_said_) {
            gpu_said_ = true;
            log::info("play: decoding in software: {}", gpu_why_not_);
        }
        return gpu_ ? &*gpu_ : nullptr;
    }
    bool gpu_said_ = false;
    // Where the decoder decodes, said in the log and over the picture when it changes (its check passed or failed).
    std::string codec_name_;
    bool decoder_on_gpu_ = false;
    int decoder_checks_left_ = -1;
    std::string decoder_software_reason_;
    std::uint64_t gpu_pictures_ = 0, gpu_checked_ = 0;
    void note_decoder_state(bool opened) {
        const bool on_gpu = decoder_->on_gpu();
        const int checks_left = decoder_->checks_left();
        const std::string& reason = decoder_->software_reason();
        if (!opened && on_gpu == decoder_on_gpu_ && checks_left == decoder_checks_left_ && reason == decoder_software_reason_) return;
        decoder_on_gpu_ = on_gpu;
        decoder_checks_left_ = checks_left;
        decoder_software_reason_ = reason;
        decoder_text_ = decoder_->description();
        if (opened) log::info("play: decoding with {}", decoder_text_);
        else if (on_gpu && checks_left == 0) log::info("play: the graphics card's first pictures were the software decoder's, sample for sample: decoding there");
        else if (!reason.empty()) log::warn("play: decoding in software from now on: {}", reason);
        const std::lock_guard stats_lock(stats_mutex_);
        stats_codec_ = std::format("{} ({})", codec_name_, on_gpu ? (checks_left > 0 ? "graphics card, checking" : "graphics card") : "software");
    }

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
        gpu_pictures_ += counters.gpu_pictures;
        gpu_checked_ += counters.checked;
        unfollowed_ = decoder_->unfollowed_reason();
    }
    // When to show each picture. Set before the first frame is there; the decode thread's from then on.
    std::optional<media::FramePacer> pacer_;
    std::uint64_t paced_ = 0, paced_late_ = 0, paced_microseconds_ = 0, paced_most_microseconds_ = 0;
    std::uint64_t through_ = 0, through_microseconds_ = 0;  // pictures handed on, and how long after their frames arrived
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

    // For the statistics over the picture. The counts are the decoding thread's, read by the window's.
    std::atomic<bool> stats_shown_{false};
    std::atomic<std::uint64_t> stats_decoded_{0}, stats_decode_microseconds_{0}, stats_lost_{0};
    std::mutex stats_mutex_;
    std::optional<proto::ConnectionQuality> quality_;  // under stats_mutex_
    std::string stats_codec_;                          // under stats_mutex_
    // On the window's thread.
    unsigned stats_width_ = 0, stats_height_ = 0;
    std::vector<std::string> stats_cache_;
    std::optional<Clock::time_point> stats_at_;
    std::uint64_t stats_shown_at_ = 0, stats_decoded_at_ = 0, stats_microseconds_at_ = 0;
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
    // Makes `pad` the controller that plays: the console's light bar and player lights, or what the
    // user chose, and its motion sensors. With pad_mutex_ held.
    void adopt(SDL_Gamepad* pad, SDL_JoystickID id);
    // The mouse, a touch screen and a d-pad standing in for the touchpad (stand_in_touch.h): what
    // they say now, told as the touchpad's fingers and its press. On the window's thread.
    void stand_in_changed();
    void tick_stand_in(std::uint64_t now_ms);
    // Nothing pressed, nothing turned: after the controller that plays went away or changed.
    void forget_input() {
        dpad_touch_ = DpadTouch{};
        stand_in_fingers_ = {};
        stand_in_pressed_ = false;
        const std::lock_guard lock(input_mutex_);
        buttons_ = transport::Buttons{};
        state_ = transport::InputState{};
        motion_ = proto::MotionTracker{};
        input_changed_ = true;
    }
    bool haptics_looked_ = false;
    SDL_AudioStream* haptics_audio_ = nullptr;
    std::string haptics_device_;
    media::HapticsUpsampler haptics_upsampler_;
    media::HapticsLevel haptics_peak_, haptics_rumble_;
    unsigned haptics_since_rumble_ = 0;
    std::uint64_t haptics_frames_ = 0, haptics_played_ = 0, haptics_rumbled_ = 0, haptics_unusable_ = 0, haptics_let_go_ = 0;

    // The menu over the stream, on the window's thread.
    StreamMenu menu_;
    PressReader menu_presses_;
    bool chord_l1_ = false, chord_r1_ = false, chord_options_ = false;
    int menu_stick_x_ = 0, menu_stick_y_ = 0;
    SDL_WindowID window_id_ = 0;
    std::uint64_t menu_opened_ = 0;
    std::string menu_chosen_;  // the last thing chosen in it, for the report
    std::atomic<bool> quit_asked_{false}, rest_asked_{false};
    std::optional<Taps> taps_;
    [[nodiscard]] bool fullscreen() const {
        SDL_Window* window = SDL_GetWindowFromID(window_id_);
        return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    }
    void open_menu();
    void menu_event(const SDL_Event& event);
    void menu_act(MenuAction action);

    // The controller.
    std::mutex input_mutex_;
    transport::Buttons buttons_;
    transport::InputState state_;
    proto::MotionTracker motion_;                // the controller's rotation, from its motion sensors
    std::atomic<bool> motion_reset_wanted_{false};  // the console asked to "take this position"
    bool input_changed_ = false;
    SDL_Gamepad* pad_ = nullptr;
    SDL_JoystickID pad_id_ = 0;
    std::uint8_t next_touch_ = 0;
    std::string pad_name_;
    std::vector<std::pair<SDL_JoystickID, SDL_Gamepad*>> others_;  // connected, not playing (pad_mutex_)
    std::uint64_t takeovers_ = 0;  // times another controller took over
    PointerTouch pointer_touch_;                 // the mouse and a touch screen on the window
    DpadTouch dpad_touch_;                       // share + the d-pad, on a controller with no touchpad
    std::array<bool, 2> stand_in_fingers_{};     // the touchpad's fingers a stand-in put down
    bool stand_in_pressed_ = false;              // the touchpad's press a stand-in made
    std::optional<std::uint64_t> share_up_at_ms_;  // share pressed for a tap of it: let go then
    std::uint64_t stand_in_touches_ = 0;         // fingers the stand-ins put down, for the report
    bool pad_through_steam_ = false;  // Steam's virtual controller: the real one's haptics, effects and lights out of reach

    std::atomic<bool> started_{false};
    std::thread decode_thread_;  // last: it starts with everything above in place
};

void Player::decode_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(jobs_mutex_);
            // The first decoder opens on the graphics card the window lends, so frames wait until it said.
            jobs_wake_.wait(lock, [&] { return closing_ || (!jobs_.empty() && gpu_decided_); });
            if (closing_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            // A new session's stream: a new decoder, opened with its header, whatever its picture size.
            if (std::exchange(restart_stream_, false)) size_index_.reset();
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
            } else if (auto opened = media::VideoDecoder::open(*codec, job.header, decode_gpu())) {
                decoder_.emplace(std::move(*opened));
                decoder_text_ = decoder_->description();
                codec_name_ = *codec == media::VideoCodec::h264 ? "H.264" : "HEVC";
                note_decoder_state(true);
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
        stats_lost_.fetch_add(job.lost_before, std::memory_order_relaxed);
        longest_arrival_since_ms_ = std::max(longest_arrival_since_ms_, job.since_arrival_ms);
        const auto began = Clock::now();
        auto decoded = decoder_->decode(job.index, job.data);
        note_decoder_state(false);
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
        stats_decoded_.fetch_add(1, std::memory_order_relaxed);
        stats_decode_microseconds_.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ready - began).count()),
                                             std::memory_order_relaxed);
        longest_behind_since_ms_ = std::max(longest_behind_since_ms_, std::chrono::duration<double, std::milli>(ready - job.arrived).count());
        if (pacer_) {
            std::unique_lock lock(jobs_mutex_);
            // With more frames waiting behind this one than a cushion is made of, the stream is behind (a bunch
            // came after a pause): the picture is shown at once, so that the pause does not stay as delay.
            // It could have been ready when its frame had arrived and was decoded; what it waited before
            // that, behind the picture before it, is delay it carries already.
            const media::FramePacer::Slot slot = pacer_->schedule(job.arrived + (ready - began), ready, jobs_.size());
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
        through_microseconds_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - job.arrived).count());
        ++through_;
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
void Player::stand_in_changed() {
    const StandInTouch& pointer = pointer_touch_.touch();
    const StandInTouch& dpad = dpad_touch_.touch();
    const std::lock_guard lock(input_mutex_);
    for (std::size_t i = 0; i < 2; ++i) {
        const std::optional<TouchSpot>& spot = pointer.fingers[i] ? pointer.fingers[i] : dpad.fingers[i];
        transport::Touch& touch = buttons_.touches[i];
        if (spot) {
            // A new finger gets a new number; a controller's own finger in that place is not taken over.
            if (!stand_in_fingers_[i]) {
                if (touch.down) continue;
                touch.id = next_touch_;
                next_touch_ = static_cast<std::uint8_t>((next_touch_ + 1) & 0x7f);
                ++stand_in_touches_;
            }
            touch.down = true;
            touch.x = static_cast<std::uint16_t>(spot->x * kTouchWidth);
            touch.y = static_cast<std::uint16_t>(spot->y * kTouchHeight);
            stand_in_fingers_[i] = true;
            input_changed_ = true;
        } else if (stand_in_fingers_[i]) {
            touch.down = false;
            stand_in_fingers_[i] = false;
            input_changed_ = true;
        }
    }
    const bool pressed = pointer.pressed || dpad.pressed;
    if (pressed != stand_in_pressed_) {
        stand_in_pressed_ = pressed;
        buttons_.set(transport::Button::touchpad, pressed);
        input_changed_ = true;
    }
}

void Player::tick_stand_in(std::uint64_t now_ms) {
    if (share_up_at_ms_ && now_ms >= *share_up_at_ms_) {
        share_up_at_ms_.reset();
        set_button(transport::Button::share, false);
    }
    const StandInTouch before = dpad_touch_.touch();
    dpad_touch_.tick(now_ms);
    if (dpad_touch_.touch() != before) stand_in_changed();
}

void Player::adopt(SDL_Gamepad* pad, SDL_JoystickID id) {
    pad_ = pad;
    pad_id_ = id;
    pad_is_dualsense_ = SDL_GetGamepadType(pad_) == SDL_GAMEPAD_TYPE_PS5;
    // The light bar: the user's color, off, or the console's if it named one before the controller was there.
    if (settings_.light_bar == LightBar::fixed) SDL_SetGamepadLED(pad_, settings_.light_color[0], settings_.light_color[1], settings_.light_color[2]);
    else if (settings_.light_bar == LightBar::off) SDL_SetGamepadLED(pad_, 0, 0, 0);
    else if (feedback_.light && SDL_SetGamepadLED(pad_, (*feedback_.light)[0], (*feedback_.light)[1], (*feedback_.light)[2])) ++feedback_.lights_passed;
    // And the player lights, if it named a player; none when the user turned them off.
    if (!settings_.player_lights) SDL_SetGamepadPlayerIndex(pad_, -1);
    else if (feedback_.infos != 0) SDL_SetGamepadPlayerIndex(pad_, feedback_.player);
    const char* name = SDL_GetGamepadName(pad_);
    pad_name_ = name != nullptr ? name : "a controller";
    pad_through_steam_ = is_steam_virtual(SDL_GetGamepadVendor(pad_), SDL_GetGamepadProduct(pad_));
    if (pad_through_steam_) log::warn("play: {}", kThroughSteamText);
    // Motion, where the controller has it and the user did not turn it off.
    const bool gyro = settings_.motion && SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_GYRO, true);
    const bool accel = settings_.motion && SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_ACCEL, true);
    const std::string chosen = describe(settings_);
    log::info("play: controller: {}{}{}{}", pad_name_, SDL_GetNumGamepadTouchpads(pad_) > 0 ? ", with a touchpad" : "",
              gyro && accel ? ", with motion sensors" : "", chosen.empty() ? std::string() : "; chosen: " + chosen);
    note_battery();
    if (!battery_text_.empty()) log::info("play: the controller's battery: {}", battery_text_);
}

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
    if (pad_ != nullptr && SDL_RumbleGamepad(pad_, rumble_at(peak.left, settings_.rumble), rumble_at(peak.right, settings_.rumble), kHapticsRumbleLifeMs)) {
        ++haptics_rumbled_;
    }
}

std::vector<std::string> Player::stats_lines() {
    if (!stats_shown_.load()) {
        stats_at_.reset();
        stats_cache_.clear();
        return {};
    }
    const auto now = Clock::now();
    if (stats_at_ && now - *stats_at_ < std::chrono::milliseconds(500)) return stats_cache_;
    std::uint64_t shown = 0;
    {
        const std::lock_guard lock(picture_mutex_);
        shown = shown_;
    }
    const std::uint64_t decoded = stats_decoded_.load(std::memory_order_relaxed);
    const std::uint64_t microseconds = stats_decode_microseconds_.load(std::memory_order_relaxed);
    std::optional<proto::ConnectionQuality> quality;
    std::string codec, screen;
    {
        const std::lock_guard lock(stats_mutex_);
        quality = quality_;
        codec = stats_codec_;
        screen = screen_state_;
    }
    std::vector<std::string> lines{"Statistics (F3 hides them)"};
    std::string picture = stats_width_ == 0 ? std::string("No picture yet") : std::format("{}\u00d7{}", stats_width_, stats_height_);
    if (!codec.empty()) picture += " " + codec;
    if (stats_at_) {
        const double seconds = std::chrono::duration<double>(now - *stats_at_).count();
        picture += std::format(" \u00b7 {:.0f} pictures a second", static_cast<double>(shown - stats_shown_at_) / seconds);
        if (decoded > stats_decoded_at_) {
            lines.push_back(picture);
            picture.clear();
            lines.push_back(std::format("Decoding: {:.1f} ms a picture", static_cast<double>(microseconds - stats_microseconds_at_) /
                                                                         static_cast<double>(decoded - stats_decoded_at_) / 1000.0));
        }
    }
    if (!picture.empty()) lines.push_back(picture);
    if (quality && quality->target_bitrate) {
        lines.push_back(std::format("Console: aims at {:.1f} Mbit/s \u00b7 round trip {:.0f} \u00b7 loss {}",
                                    static_cast<double>(*quality->target_bitrate) / 1e6, quality->rtt.value_or(0.0), quality->loss.value_or(0)));
    } else {
        lines.emplace_back("Console: no view of the connection yet");
    }
    lines.push_back(std::format("Frames lost on the way: {}", stats_lost_.load(std::memory_order_relaxed)));
    if (!screen.empty()) lines.push_back(screen);
    stats_at_ = now;
    stats_shown_at_ = shown;
    stats_decoded_at_ = decoded;
    stats_microseconds_at_ = microseconds;
    stats_cache_ = lines;
    return lines;
}

void Player::open_menu() {
    menu_.open(stats_shown_.load(), fullscreen());
    menu_presses_ = PressReader{};
    ++menu_opened_;
    // What was held goes up for the console: nothing stays pressed on it while the menu is open.
    {
        const std::lock_guard lock(input_mutex_);
        buttons_ = transport::Buttons{};
        state_.left_x = state_.left_y = state_.right_x = state_.right_y = 0;
        input_changed_ = true;
    }
    log::info("play: the menu over the stream opened");
    render::wake_window();
}

void Player::menu_act(MenuAction action) {
    switch (action) {
    case MenuAction::none: return;
    case MenuAction::resume: menu_chosen_ = "back to the game"; break;
    case MenuAction::toggle_stats:
        stats_shown_.store(!stats_shown_.load());
        menu_chosen_ = stats_shown_.load() ? "statistics shown" : "statistics hidden";
        break;
    case MenuAction::toggle_fullscreen:
        if (SDL_Window* window = SDL_GetWindowFromID(window_id_)) SDL_SetWindowFullscreen(window, !fullscreen());
        menu_chosen_ = "full screen";
        break;
    case MenuAction::quit:
        menu_chosen_ = "quit";
        quit_asked_.store(true);
        break;
    case MenuAction::rest_and_quit:
        menu_chosen_ = "quit and put the console to rest";
        rest_asked_.store(true);
        quit_asked_.store(true);
        break;
    }
    log::info("play: chosen in the menu: {}", menu_chosen_);
    render::wake_window();
}

// While the menu is open, what is pressed moves through it and reaches the console no more.
void Player::menu_event(const SDL_Event& event) {
    std::vector<Press> presses;
    const std::uint64_t now = SDL_GetTicks();
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (event.gbutton.which != pad_id_) return;
        std::optional<Press> press;
        switch (event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: press = Press::up; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: press = Press::down; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: press = Press::left; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: press = Press::right; break;
        case SDL_GAMEPAD_BUTTON_SOUTH: press = Press::accept; break;
        case SDL_GAMEPAD_BUTTON_EAST: press = Press::back; break;
        default: break;
        }
        if (press) menu_presses_.button(*press, event.gbutton.down, now, presses);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.which != pad_id_) return;
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) menu_stick_x_ = event.gaxis.value;
        else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) menu_stick_y_ = event.gaxis.value;
        else return;
        menu_presses_.stick(menu_stick_x_, menu_stick_y_, now, presses);
        break;
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key) {
        case SDLK_UP: presses.push_back(Press::up); break;
        case SDLK_DOWN: presses.push_back(Press::down); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!event.key.repeat) presses.push_back(Press::accept);
            break;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
            if (!event.key.repeat) presses.push_back(Press::back);
            break;
        default: break;
        }
        break;
    default: return;
    }
    for (const Press press : presses) menu_act(menu_.press(press));
    render::wake_window();
}

void Player::on_event(const SDL_Event& event) {
    using transport::Button;
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) window_id_ = event.window.windowID;
    // F3, as in many games: the statistics over the picture, and away again.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F3 && !event.key.repeat) {
        stats_shown_.store(!stats_shown_.load());
        render::wake_window();
        return;
    }
    // F1, or L1 + R1 + Options held together on the controller: the menu over the stream.
    // A press on a controller that does not play makes it the one that plays (two connected, or a
    // second picked up): the light bar, the player lights, the trigger effects and the rumble go to it.
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && pad_ != nullptr && event.gbutton.which != pad_id_) {
        bool took_over = false;
        {
            const std::lock_guard pad_lock(pad_mutex_);
            const auto next = std::ranges::find_if(others_, [&](const auto& one) { return one.first == event.gbutton.which; });
            if (next != others_.end()) {
                const auto [id, pad] = *next;
                others_.erase(next);
                release_triggers();
                SDL_RumbleGamepad(pad_, 0, 0, 0);
                others_.emplace_back(pad_id_, pad_);
                const std::string before = pad_name_;
                adopt(pad, id);
                ++takeovers_;
                log::info("play: {} plays now (a button was pressed on it; {} waits)", pad_name_, before);
                took_over = true;
            }
        }
        // Nothing the other one held stays pressed.
        if (took_over) forget_input();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1 && !event.key.repeat) {
        if (menu_.is_open()) menu_act(menu_.press(Press::back));
        else open_menu();
        return;
    }
    // A DualSense Edge's Fn buttons: what the user chose (nothing unless they did). Before the menu:
    // the one that opens it closes it too.
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.which == pad_id_ &&
        (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 || event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2)) {
        const FnAction action = event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 ? settings_.fn_left : settings_.fn_right;
        if (action != FnAction::none) ++fn_presses_;
        switch (action) {
        case FnAction::menu:
            if (menu_.is_open()) menu_act(menu_.press(Press::back));
            else open_menu();
            return;
        case FnAction::stats:
            stats_shown_.store(!stats_shown_.load());
            render::wake_window();
            return;
        case FnAction::fullscreen:
            if (SDL_Window* window = SDL_GetWindowFromID(window_id_)) SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
            return;
        case FnAction::none: break;
        }
    }
    if ((event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) && event.gbutton.which == pad_id_) {
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) chord_l1_ = event.gbutton.down;
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) chord_r1_ = event.gbutton.down;
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START) chord_options_ = event.gbutton.down;
        if (menu_.chord(chord_l1_, chord_r1_, chord_options_)) {
            open_menu();
            return;
        }
    }
    if (menu_.is_open()) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            menu_event(event);
            return;
        default: break;
        }
    }
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which);
        if (pad == nullptr) break;
        const std::lock_guard pad_lock(pad_mutex_);
        if (pad_ == nullptr) {
            adopt(pad, event.gdevice.which);
        } else {
            // Open too, so that a press on it is seen: then it is the one that plays.
            others_.emplace_back(event.gdevice.which, pad);
            const char* name = SDL_GetGamepadName(pad);
            log::info("play: also connected: {} (a press on it makes it the one that plays)", name != nullptr ? name : "a controller");
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ != nullptr && event.gdevice.which == pad_id_) {
            {
                const std::lock_guard pad_lock(pad_mutex_);
                release_triggers();
                SDL_CloseGamepad(pad_);
                pad_ = nullptr;
                // Another that is connected plays from now on.
                if (!others_.empty()) {
                    const auto [id, next] = others_.front();
                    others_.erase(others_.begin());
                    adopt(next, id);
                }
            }
            log::info("play: the controller is gone{}", pad_ != nullptr ? std::format("; {} plays now", pad_name_) : std::string());
            forget_input();
        } else {
            const std::lock_guard pad_lock(pad_mutex_);
            const auto gone = std::ranges::find_if(others_, [&](const auto& one) { return one.first == event.gdevice.which; });
            if (gone != others_.end()) {
                SDL_CloseGamepad(gone->second);
                others_.erase(gone);
            }
        }
        break;
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
        if (pad_ != nullptr && event.jbattery.which == pad_id_) note_battery();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (event.gbutton.which != pad_id_) break;
        // A controller with no touchpad: share held and the d-pad stand in for one.
        if (settings_.dpad_touch && pad_ != nullptr && SDL_GetNumGamepadTouchpads(pad_) == 0) {
            std::optional<DpadTouch::Key> key;
            switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_BACK: key = DpadTouch::Key::share; break;
            case SDL_GAMEPAD_BUTTON_DPAD_UP: key = DpadTouch::Key::up; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: key = DpadTouch::Key::down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: key = DpadTouch::Key::left; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: key = DpadTouch::Key::right; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: key = DpadTouch::Key::south; break;
            default: break;
            }
            const std::uint64_t now = SDL_GetTicks();
            if (key && dpad_touch_.key(*key, event.gbutton.down, now)) {
                // Share alone was share: pressed now, let go a moment later.
                if (dpad_touch_.take_share_tap()) {
                    set_button(Button::share, true);
                    share_up_at_ms_ = now + kShareTapMs;
                }
                stand_in_changed();
                break;
            }
        }
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
        // The sticks through the dead zone the user chose (both axes of a stick together: it is round).
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: raw_left_x_ = value; std::tie(state_.left_x, state_.left_y) = apply_dead_zone(raw_left_x_, raw_left_y_, settings_.dead_zone); break;
        case SDL_GAMEPAD_AXIS_LEFTY: raw_left_y_ = value; std::tie(state_.left_x, state_.left_y) = apply_dead_zone(raw_left_x_, raw_left_y_, settings_.dead_zone); break;
        case SDL_GAMEPAD_AXIS_RIGHTX: raw_right_x_ = value; std::tie(state_.right_x, state_.right_y) = apply_dead_zone(raw_right_x_, raw_right_y_, settings_.dead_zone); break;
        case SDL_GAMEPAD_AXIS_RIGHTY: raw_right_y_ = value; std::tie(state_.right_x, state_.right_y) = apply_dead_zone(raw_right_x_, raw_right_y_, settings_.dead_zone); break;
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
        if (event.gtouchpad.which != pad_id_ || event.gtouchpad.finger < 0 || event.gtouchpad.finger > 1 || !settings_.touchpad) break;
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
        // The sensor's own clock where it has one (nanoseconds), else when SDL saw the reading.
        const Uint64 ns = event.gsensor.sensor_timestamp != 0 ? event.gsensor.sensor_timestamp : event.gsensor.timestamp;
        const std::uint64_t us = ns / 1000;
        if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
            motion_.gyro({event.gsensor.data[0], event.gsensor.data[1], event.gsensor.data[2]}, us);
        } else if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
            // From meters per second squared to multiples of the earth's pull.
            motion_.accel({event.gsensor.data[0] / SDL_STANDARD_GRAVITY, event.gsensor.data[1] / SDL_STANDARD_GRAVITY,
                           event.gsensor.data[2] / SDL_STANDARD_GRAVITY}, us);
        } else {
            break;
        }
        if (motion_reset_wanted_.exchange(false)) motion_.take_as_rest();
        const proto::Axes& rate = motion_.rate();
        const proto::Axes& pull = motion_.acceleration();
        const proto::Rotation rotation = motion_.rotation();
        state_.gyro_x = rate[0];
        state_.gyro_y = rate[1];
        state_.gyro_z = rate[2];
        state_.accel_x = pull[0];
        state_.accel_y = pull[1];
        state_.accel_z = pull[2];
        state_.orient_x = rotation.x;
        state_.orient_y = rotation.y;
        state_.orient_z = rotation.z;
        state_.orient_w = rotation.w;
        input_changed_ = true;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION: {
        // A touch screen's own mouse events: it is read as fingers (below).
        const SDL_MouseID mouse = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.which : event.button.which;
        if (!settings_.pointer_touch || mouse == SDL_TOUCH_MOUSEID) break;
        SDL_Window* window = SDL_GetWindowFromID(event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.windowID : event.button.windowID);
        int width = 0, height = 0;
        if (window == nullptr || !SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0) break;
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            pointer_touch_.mouse_moved(event.motion.x / w, event.motion.y / h);
        } else {
            pointer_touch_.mouse_button(event.button.button == SDL_BUTTON_LEFT ? PointerTouch::MouseButton::left : PointerTouch::MouseButton::other,
                                        event.button.down, event.button.x / w, event.button.y / h);
        }
        stand_in_changed();
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_FINGER_MOTION:
        // On the window, from 0 to 1 across and down.
        if (!settings_.pointer_touch) break;
        if (event.type == SDL_EVENT_FINGER_MOTION) {
            pointer_touch_.finger_moved(event.tfinger.fingerID, event.tfinger.x, event.tfinger.y);
        } else {
            pointer_touch_.finger(event.tfinger.fingerID, event.type == SDL_EVENT_FINGER_DOWN, event.tfinger.x, event.tfinger.y,
                                  event.tfinger.timestamp / 1'000'000);
        }
        stand_in_changed();
        break;
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
        if (gpu_pictures_ != 0 || gpu_checked_ != 0) {
            lines.push_back(std::format("Decoding on the graphics card: {} of the {} pictures came out there; {} of its pictures were compared with the "
                                        "software decoder's first{}.",
                                        gpu_pictures_, pictures_, gpu_checked_,
                                        decoder_software_reason_.empty() ? std::string(", all the same") : std::format(", then: {}", decoder_software_reason_)));
        } else {
            const std::lock_guard lock(jobs_mutex_);
            if (!gpu_why_not_.empty()) lines.push_back(std::format("Decoding in software: {}.", gpu_why_not_));
        }
        if (losses_ != 0) {
            lines.push_back(std::format("The connection was lost {} times; it came back {} times{}.", losses_, comebacks_,
                                        comebacks_ != 0 ? std::format(" (after {:.1f} s at most)", longest_out_s_) : std::string()));
            if (wakes_ != 0) lines.push_back(std::format("The console went to rest while the connection was down and was woken {} times.", wakes_));
        }
        if (menu_opened_ != 0) lines.push_back(std::format("The menu over the stream was opened {} times; chosen in it last: {}.", menu_opened_,
                                                           menu_chosen_.empty() ? std::string("nothing") : menu_chosen_));
        lines.push_back(std::format("Frames arrived this far apart: {}.", arrival_spacing_.text()));
        lines.push_back(std::format("Frames that came late with none lost: {} were held up on the way (the next one came right behind), {} the console "
                                    "sent late (the next one kept its distance: the game's own pace).",
                                    late_on_the_way_, late_sent_));
        lines.push_back(std::format("Pictures went to the screen this far apart: {}.", handed_spacing_.text()));
        if (!screen_times_.empty()) lines.push_back(screen_times_ + ".");
        const double through_ms = static_cast<double>(through_microseconds_) / 1000.0 / static_cast<double>(std::max<std::uint64_t>(through_, 1));
        if (pacer_) {
            const auto cushion = std::chrono::duration_cast<std::chrono::milliseconds>(pacer_->cushion()).count();
            lines.push_back(std::format("Pacing: {} of {} pictures were held back to follow the one before by a frame's time, by {:.1f} ms on average "
                                        "and {:.1f} ms at most; {} had more frames waiting behind them than the cushion is long (a bunch after a pause) "
                                        "and were shown at once. {}; a picture was on its way to the screen {:.1f} ms after its frame had arrived, on average.",
                                        paced_, pictures_, static_cast<double>(paced_microseconds_) / 1000.0 / static_cast<double>(std::max<std::uint64_t>(paced_, 1)),
                                        static_cast<double>(paced_most_microseconds_) / 1000.0, paced_late_,
                                        cushion > 0 ? std::format("A cushion of {} ms was kept for frames that come late", cushion) : std::string("No cushion was kept"),
                                        through_ms));
        } else {
            lines.push_back(std::format("Pacing: off; every picture was shown the moment it was there, {:.1f} ms after its frame had arrived, on average.", through_ms));
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
    if (pad_through_steam_) lines.push_back(std::format("{}.", kThroughSteamText));
    if (stand_in_touches_ != 0) lines.push_back(std::format("The touchpad was stood in for: {} fingers put down by the mouse, a touch screen or the d-pad.", stand_in_touches_));
    if (takeovers_ != 0) lines.push_back(std::format("Another controller took over {} times (a press on a controller that did not play).", takeovers_));
    if (const std::string chosen = describe(settings_); !chosen.empty()) {
        lines.push_back(std::format("Controller settings: {}; trigger effects held back by them {} times, Fn buttons pressed {} times.", chosen,
                                    triggers_held_back_, fn_presses_));
    }
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
    if (options.pretend) only_pretended_controllers();
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log::warn("play: no controllers: {}", SDL_GetError());
    if (options.pretend && !attach_pretended(*options.pretend)) {
        log::error("play: --pretend takes a vendor and a product number, as 054c:0df2");
        return 64;
    }

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
        Player player(static_cast<double>(plan.video.max_fps), options.pacing, options.cushion_ms, options.haptics_volume, options.back_left, options.back_right,
                      options.decoder == PlayOptions::Decoder::automatic);
        player.set_controller_settings(options.controller_settings);
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        plan.on_media = [&](transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) { player.on_media(media, info); };
        plan.controller = [&](transport::Buttons& buttons, transport::InputState& state) { return player.controller(buttons, state); };
        plan.wants_keyframe = [&] { return player.wants_keyframe(); };
        plan.on_console_data = [&](const std::vector<transport::ConsoleData>& data, std::chrono::microseconds) { player.on_console_data(data); };
        plan.on_quality = [&](const proto::ConnectionQuality& quality) { player.on_quality(quality); };
        player.show_stats(options.stats);
        plan.stop = [&] { return stop.load(); };
        plan.rest_asked = [&] { return player.rest_asked(); };
        if (options.pretend_taps) player.set_taps(*options.pretend_taps);
        plan.reconnectable = options.reconnect_seconds > 0;
        std::thread session([&] {
            // A lost connection is made again with new sessions, for as long as the options say; a loss after
            // it came back begins that time again.
            std::optional<Clock::time_point> trying_since;
            int attempt = 0;
            for (;;) {
                session_code = run_session(plan);
                if (stop.load() || player.quit_asked() || !plan.reconnectable) break;
                const bool lost = session_code == kSessionLost;
                if (lost && !player.reconnecting()) {
                    trying_since = Clock::now();
                    attempt = 0;
                }
                if (!trying_since || (!lost && !player.reconnecting())) break;
                if (Clock::now() - *trying_since > std::chrono::seconds(options.reconnect_seconds)) {
                    log::warn("play: the connection did not come back within {} s", options.reconnect_seconds);
                    break;
                }
                player.connection_lost(++attempt);
                // A new session that did not start: the console may have gone to rest meanwhile.
                if (!lost && options.wake_resting) {
                    const auto left = std::chrono::seconds(options.reconnect_seconds) -
                                      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - *trying_since);
                    if (left.count() > 0) {
                        const int woke = options.wake_resting(left, [&] { player.set_waking(true); });
                        if (player.waking() && woke != 0) log::warn("play: the console went to rest and could not be woken again");
                        player.set_waking(false);
                    }
                }
                // A moment between tries, cut short when the window closes.
                for (int i = 0; i < 20 && !stop.load() && !player.quit_asked(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (session_code == kSessionLost) session_code = 1;
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
            window.finished = [&] {
                player.follow_taps();
                return done.load() || player.quit_asked();
            };
            window.on_event = [&](const SDL_Event& event) { player.on_event(event); };
            window.overlay_lines = [&] { return player.stats_lines(); };
            window.overlay_menu = [&] { return player.menu_view(); };
            window.overlay_notice = [&] { return player.notice_text(); };
            window.gpu_decoding = options.decoder == PlayOptions::Decoder::automatic;
            if (options.hdr_peak_nits) window.hdr_peak_nits = *options.hdr_peak_nits;
            window.scaler = options.scaler;
            window.deband = options.deband;
            window.hdr_output = options.hdr_output;
            window.stream_rate = static_cast<double>(plan.video.max_fps);
            window.on_gpu = [&](std::optional<media::GpuDevice> gpu, const std::string& why_not) { player.set_gpu(std::move(gpu), why_not); };
            window.before_device_gone = [&] { player.drop_gpu(); };
            window.on_screen_times = [&](const std::string& line) { player.set_screen_times(line); };
            window.on_screen_state = [&](const std::string& line) { player.set_screen_state(line); };
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
