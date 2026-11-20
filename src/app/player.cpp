#include "app/player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "app/log.h"
#include "media/sound_decoder.h"
#include "media/video_decoder.h"
#include "psvp/version.h"
#include "render/vk_window.h"

namespace psvp::app {

namespace {

namespace transport = proto::transport;

// Sound waiting to be played at most: what arrives beyond it is left out, so that the
// sound never falls behind the picture. A frame is 10 ms.
constexpr int kMaxSoundQueuedMs = 60;
// Frames filled in for lost ones at most, per gap: a long gap is silence, not a held note.
constexpr std::uint32_t kMaxConcealed = 5;
// The touchpad as a console counts it (the reference's numbers).
constexpr float kTouchWidth = 1919.0F, kTouchHeight = 1079.0F;

class Player {
public:
    Player() : decode_thread_([this] { decode_loop(); }) {}
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
        if (pad_ != nullptr) SDL_CloseGamepad(pad_);
    }

    // ---- on the session's thread ----
    void on_media(transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) {
        if (!info) return;
        {
            const std::lock_guard lock(jobs_mutex_);
            for (auto& frame : media.video) {
                Job job;
                if (frame.adaptive_stream < info->resolutions.size()) job.header = info->resolutions[frame.adaptive_stream].video_header;
                job.size_index = frame.adaptive_stream;
                job.lost_before = frame.lost_before;
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
            }
        }
        started_.store(true);
        started_.notify_all();
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
        std::uint32_t lost_before = 0;
        std::vector<std::uint8_t> data;
    };
    void decode_loop();
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

    // The decoder's side: only the decode thread touches these until it has ended.
    std::optional<media::VideoDecoder> decoder_;
    std::optional<unsigned> size_index_;
    std::string decoder_text_, decoder_error_;
    std::uint64_t frames_ = 0, pictures_ = 0, held_back_ = 0, refused_ = 0, microseconds_ = 0, slowest_ = 0;

    // The newest picture, for the window.
    std::mutex picture_mutex_;
    std::optional<media::Picture> latest_;
    bool fresh_ = false;
    std::uint64_t shown_ = 0, replaced_ = 0;

    // Sound: only the session's thread touches these.
    std::optional<media::SoundDecoder> sound_;
    SDL_AudioStream* audio_ = nullptr;
    bool sound_failed_ = false;
    std::uint64_t sound_played_ = 0, sound_left_out_ = 0, sound_filled_ = 0;

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
            if (decoder_) {
                microseconds_ += decoder_->counters().microseconds;
                slowest_ = std::max(slowest_, decoder_->counters().slowest_microseconds);
            }
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
        auto picture = decoder_->decode(job.data);
        if (!picture) {
            ++refused_;
            continue;
        }
        ++pictures_;
        if (picture->damaged) {
            // Built on a frame that did not arrive: the last whole picture stays up until the
            // console's repair, a few frames later, instead of a picture with holes in it.
            ++held_back_;
            continue;
        }
        {
            const std::lock_guard lock(picture_mutex_);
            if (fresh_) ++replaced_;
            latest_ = std::move(*picture);
            fresh_ = true;
        }
        render::wake_window();
    }
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
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_ == nullptr || !SDL_ResumeAudioStreamDevice(audio_)) {
            log::warn("play: no sound: the sound device could not be opened: {}", SDL_GetError());
            sound_failed_ = true;
            return;
        }
        log::info("play: sound through SDL's {} driver, {} Hz, {} channels", SDL_GetCurrentAudioDriver(), format.rate, format.channels);
    }
    const int bytes_per_ms = static_cast<int>(format.rate * format.channels * 2 / 1000);
    const auto put = [&](std::span<const std::int16_t> samples, std::uint64_t& counted) {
        if (SDL_GetAudioStreamQueued(audio_) > kMaxSoundQueuedMs * bytes_per_ms) {
            ++sound_left_out_;
            return;
        }
        SDL_PutAudioStreamData(audio_, samples.data(), static_cast<int>(samples.size_bytes()));
        ++counted;
    };
    for (std::uint32_t i = 0; i < std::min(frame.lost_before, kMaxConcealed); ++i) put(sound_->conceal(), sound_filled_);
    put(sound_->decode(frame.data), sound_played_);
}

void Player::on_event(const SDL_Event& event) {
    using transport::Button;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (pad_ == nullptr) {
            pad_ = SDL_OpenGamepad(event.gdevice.which);
            if (pad_ != nullptr) {
                pad_id_ = event.gdevice.which;
                const char* name = SDL_GetGamepadName(pad_);
                pad_name_ = name != nullptr ? name : "a controller";
                // Motion, where the controller has it.
                const bool gyro = SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_GYRO, true);
                const bool accel = SDL_SetGamepadSensorEnabled(pad_, SDL_SENSOR_ACCEL, true);
                log::info("play: controller: {}{}{}", pad_name_, SDL_GetNumGamepadTouchpads(pad_) > 0 ? ", with a touchpad" : "",
                          gyro && accel ? ", with motion sensors" : "");
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ != nullptr && event.gdevice.which == pad_id_) {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
            log::info("play: the controller is gone");
            // Nothing stays pressed.
            const std::lock_guard lock(input_mutex_);
            buttons_ = transport::Buttons{};
            state_ = transport::InputState{};
            input_changed_ = true;
        }
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
    std::uint64_t microseconds = microseconds_, slowest = slowest_;
    if (decoder_) {
        microseconds += decoder_->counters().microseconds;
        slowest = std::max(slowest, decoder_->counters().slowest_microseconds);
    }
    if (!decoder_error_.empty()) lines.push_back(std::format("Pictures could not be decoded: {}.", decoder_error_));
    if (frames_ != 0 && decoder_error_.empty()) {
        lines.push_back(std::format("Played ({}): {} pictures decoded from {} frames, {} shown, {} replaced by a newer one before the screen took them, "
                                    "{} held back as damaged, {} frames gave no picture; {:.2f} ms a frame, {:.2f} ms the slowest; most frames waiting for "
                                    "the decoder: {}.",
                                    decoder_text_, pictures_, frames_, shown_, replaced_, held_back_, refused_,
                                    static_cast<double>(microseconds) / 1000.0 / static_cast<double>(std::max<std::uint64_t>(frames_, 1)),
                                    static_cast<double>(slowest) / 1000.0, longest_queue_));
    }
    if (sound_) {
        lines.push_back(std::format("Sound: {} frames played, {} filled in for frames that did not arrive, {} left out to keep up with the picture{}.",
                                    sound_played_, sound_filled_, sound_left_out_, audio_ == nullptr ? " (no sound device)" : ""));
    }
    lines.push_back(pad_name_.empty() ? std::string("Controller: none was connected (the keyboard's arrow keys, Enter and Backspace stood in).")
                                      : std::format("Controller: {}.", pad_name_));
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

    int window_code = 0;
    int session_code = 0;
    {
        Player player;
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        plan.on_media = [&](transport::MediaOutput& media, const std::optional<proto::StreamInfo>& info) { player.on_media(media, info); };
        plan.controller = [&](transport::Buttons& buttons, transport::InputState& state) { return player.controller(buttons, state); };
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
            window_code = render::run_window(window);
        }
        stop.store(true);
        session.join();
        player.finish();
        for (const std::string& line : player.report()) std::printf("%s\n", line.c_str());
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (SDL_WasInit(0) == 0) SDL_Quit();
    return window_code != 0 ? window_code : session_code;
}

}  // namespace psvp::app
