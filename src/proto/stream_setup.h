// Setting a stream up on its own connection: the client's opening message with
// the launch specification and its public key, the console's answer with its
// key, the keys both sides make from them, and the stream information.
// docs/protocol/stream-setup.md describes it. No sockets and no clock in
// here: the caller passes datagrams and the time in and sends what comes out.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "proto/frame_assembler.h"
#include "proto/input.h"
#include "proto/key_exchange.h"
#include "proto/media_sender.h"
#include "proto/probe.h"
#include "proto/rp_cipher.h"
#include "proto/session.h"
#include "proto/sound.h"
#include "proto/stream_message.h"
#include "proto/transport.h"

namespace psvp::proto::transport {

// The version of the stream's protocol each family is spoken to in.
[[nodiscard]] constexpr std::uint32_t stream_version(ConsoleKind kind) noexcept { return kind == ConsoleKind::ps5 ? 12 : 9; }

// How much smaller than the path's largest packet a datagram of a split message is kept.
inline constexpr std::uint32_t kSplitMargin = 50;

struct VideoProfile {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t max_fps = 60;
    std::uint32_t bitrate_kbps = 10000;
    Codec codec = Codec::h264;
};

struct StreamRequest {
    ConsoleKind kind = ConsoleKind::ps5;
    std::string session_id;        // names the session in the opening message
    RpCipher session_cipher;       // masks the launch specification
    ProbeResult network;           // what the probe measured, or its defaults
    VideoProfile video;
    bool dualsense = false;        // the controller the console is told about
};

// What the console's answer to the opening message showed.
struct BangFacts {
    enum class Code { none, valid, invalid, unchecked };

    std::uint32_t server_version = 0;
    bool signature_matches = false;   // the signature of its public key, checked with the handshake key
    Code code = Code::unchecked;      // the authentication code of the packet that carried the answer
    std::size_t session_key_size = 0;
    bool session_key_is_ours = false; // it repeated the session ID the client sent
    std::uint16_t chunks = 1;
};

struct StreamCounters {
    std::uint64_t video_packets = 0;      // with the right authentication code
    std::uint64_t audio_packets = 0;
    std::uint64_t media_rejected = 0;     // with a wrong one
    std::uint64_t media_before_keys = 0;  // arrived before there was a key to check them with
    std::uint64_t media_unreadable = 0;   // with the right code and a header that makes no sense
    std::uint64_t loss_reports = 0;       // "these frames did not arrive whole", sent to the console
    std::uint64_t keyframe_requests = 0;
    std::uint64_t congestion_reports = 0;
    std::uint64_t input_states = 0;       // input state packets sent
    std::uint64_t other_datagrams = 0;    // of a kind this version does not read
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t split_messages = 0;     // messages from the console that came in more than one chunk
    std::map<std::uint32_t, std::uint64_t> messages;    // from the console after the set-up, by message type
    std::map<std::uint8_t, std::uint64_t> other_data;   // data that is not a message (rumble and the like), by kind
};

// Frames that are ready for a decoder, in order.
struct MediaOutput {
    std::vector<AssembledFrame> video;
    std::vector<SoundFrame> sound;  // sound and haptics, as they came
};

// The client's side, from a finished handshake on the stream port.
class ClientStream {
public:
    enum class State { idle, awaiting_bang, awaiting_stream_info, ready, closed, failed };

    static constexpr std::chrono::milliseconds kAnswerWait{5000};
    static constexpr std::chrono::milliseconds kHeartbeatEvery{1000};
    static constexpr std::chrono::milliseconds kCongestionEvery{200};
    static constexpr std::chrono::milliseconds kInputEvery{200};  // when nothing about the controller changes
    static constexpr std::uint16_t kReportChannel = 2;
    static constexpr std::size_t kMaxUnreported = 8;
    static constexpr std::uint16_t kSessionChannel = 1;
    static constexpr std::uint16_t kStreamInfoAckChannel = 9;

    ClientStream(std::uint32_t local_tag, const Endpoint& console, StreamRequest request, const KeyExchange& keys,
                 const Key16& handshake_key);

    [[nodiscard]] Datagrams start(TimePoint now);
    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    // To be called when deadline() has passed: sends again, gives up, or sends the next heartbeat.
    [[nodiscard]] Datagrams on_timeout(TimePoint now);
    [[nodiscard]] TimePoint deadline() const;
    // Says goodbye. The state becomes closed; the goodbye is sent again until the console has it.
    [[nodiscard]] Datagrams disconnect(TimePoint now);
    [[nodiscard]] bool goodbye_pending() const noexcept { return goodbye_ && messenger_.waiting() > 0; }
    // The frames that became ready since the last call.
    [[nodiscard]] MediaOutput take_media() { return std::exchange(media_, {}); }
    // The controller's sticks and sensors, as the next input state packets will tell them.
    void set_input(const InputState& state) noexcept { input_ = state; }
    // Asks the console for a picture that needs no earlier one. Nothing while the stream is not set up.
    [[nodiscard]] Datagrams request_keyframe(TimePoint now);
    [[nodiscard]] const FrameCounters& video() const noexcept { return video_.counters(); }
    [[nodiscard]] const SoundCounters& sound() const noexcept { return sound_.counters(); }
    [[nodiscard]] const SoundCounters& haptics() const noexcept { return haptics_.counters(); }

    [[nodiscard]] State state() const noexcept { return state_; }
    // Why it failed, or the reason the console gave when it closed the stream.
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] bool closed_by_console() const noexcept { return closed_by_console_; }
    [[nodiscard]] const std::optional<BangFacts>& bang() const noexcept { return bang_; }
    [[nodiscard]] const std::optional<StreamInfo>& info() const noexcept { return info_; }
    [[nodiscard]] const StreamCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::uint64_t control_rejected() const noexcept { return messenger_.rejected(); }
    // The opening message: its size and how many datagrams it went out in.
    [[nodiscard]] std::size_t opening_size() const noexcept { return opening_size_; }
    [[nodiscard]] std::size_t opening_datagrams() const noexcept { return opening_datagrams_; }

private:
    void send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now);
    void handle(Datagrams& out, Messenger::Received& received, TimePoint now);
    void on_message(Datagrams& out, const DataChunk& chunk, const StreamMessage& message, TimePoint now);
    void on_bang(Datagrams& out, const DataChunk& chunk, const BangMessage& bang, TimePoint now);
    void on_stream_info(Datagrams& out, const StreamInfo& info, TimePoint now);
    void fail(std::string why);
    void on_media(std::span<const std::uint8_t> datagram, TimePoint now);
    void note_lost(const FrameOutput& frames);
    void send_reports(Datagrams& out, TimePoint now);
    void send_congestion(Datagrams& out);
    void send_input(Datagrams& out);

    Messenger messenger_;
    StreamRequest request_;
    KeyExchange keys_;
    Key16 handshake_key_;
    std::size_t max_datagram_;
    State state_ = State::idle;
    TimePoint step_deadline_{};
    TimePoint next_heartbeat_{};
    std::string reason_;
    bool closed_by_console_ = false;
    bool goodbye_ = false;
    TimePoint next_congestion_{};
    TimePoint next_input_{};
    InputState input_;
    std::uint16_t input_sequence_ = 0;
    std::shared_ptr<SendKeys> send_keys_;
    std::shared_ptr<ReceiveKeys> receive_keys_;
    std::optional<BangFacts> bang_;
    std::optional<StreamInfo> info_;
    std::optional<StreamInfo> early_info_;
    StreamCounters counters_;
    std::size_t opening_size_ = 0;
    std::size_t opening_datagrams_ = 0;
    FrameAssembler video_;
    SoundReceiver sound_;
    SoundReceiver haptics_;
    MediaOutput media_;
    std::vector<FrameRange> unreported_;   // lost frames the console has not been told about yet
    std::vector<std::uint8_t> scratch_;    // a packet's payload while it is decrypted
    std::uint64_t interval_received_ = 0;  // sound packets since the last congestion report
    std::uint64_t interval_lost_ = 0;
};

// The console's side, as the mock console and the tests play it.
class ConsoleStream {
public:
    struct Options {
        ConsoleKind kind = ConsoleKind::ps5;
        RpCipher session_cipher;
        std::string session_id;                    // the one it handed out on the control channel
        std::vector<StreamResolution> resolutions;
        AudioHeader audio;
        std::size_t max_datagram = kMtuCeiling - kSplitMargin;
        bool accept_version = true;
        bool wrong_signature = false;              // sign its public key with a key that is not the handshake key
        std::chrono::milliseconds media_every{0};  // send a sound packet this often; 0: none
        bool corrupt_media = false;                // change a byte of each packet after sealing it
        // The picture and what goes wrong with it. Frames count from 1; every frame is fixed by seed and index.
        std::chrono::milliseconds frame_every{0};  // send a picture frame this often; 0: none
        unsigned frames = 0;                       // stop after this many; 0: go on
        std::uint64_t seed = 1;
        std::size_t frame_bytes = 6000;            // a frame is half to one and a half times this
        std::size_t unit_data = 1000;
        unsigned repair_units = 2;
        unsigned lose_every = 0;                   // every frame whose index this divides loses...
        unsigned lose_units = 0;                   // ...this many of its packets, from its first on
        unsigned late_every = 0;                   // every such frame's last packet goes out after the next frame's first
        std::size_t sound_bytes = 40;              // per sound frame
        unsigned sound_repeats = 2;                // earlier frames sent again in every sound packet
        unsigned lose_sound_every = 0;             // every sound packet whose frame index this divides is not sent
        unsigned haptics_every = 0;                // a haptics packet with every Nth sound packet (PS5); 0: none
    };

    ConsoleStream(std::uint32_t local_tag, const Endpoint& client, Options options, const KeyExchange& keys);

    [[nodiscard]] Datagrams on_datagram(std::span<const std::uint8_t> datagram, TimePoint now);
    [[nodiscard]] Datagrams on_timer(TimePoint now);
    [[nodiscard]] std::optional<TimePoint> deadline() const;
    // Closes the stream from the console's side, with a reason.
    [[nodiscard]] Datagrams disconnect(const std::string& reason, TimePoint now);

    // One line per thing the client did, in order; taking them empties the list.
    [[nodiscard]] std::vector<std::string> take_events();
    // One line that counts the packets that are not messages, and tells whether the
    // key positions of everything the client sealed follow one another as its sizes say.
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] bool keyed() const noexcept { return messenger_.keyed(); }
    [[nodiscard]] bool disconnected() const noexcept { return disconnected_; }

private:
    void send(Datagrams& out, const StreamMessage& message, std::uint16_t channel, TimePoint now);
    void on_message(Datagrams& out, const DataChunk& chunk, TimePoint now);
    void on_big(Datagrams& out, const DataChunk& chunk, const BigMessage& big, TimePoint now);

    Messenger messenger_;
    Options options_;
    KeyExchange keys_;
    std::shared_ptr<SendKeys> send_keys_;
    std::shared_ptr<ReceiveKeys> receive_keys_;
    void send_frame(Datagrams& out);
    void send_sound(Datagrams& out);

    bool disconnected_ = false;
    std::optional<TimePoint> next_media_;
    std::optional<TimePoint> next_frame_;
    std::optional<MediaSender> media_;
    std::uint16_t frame_index_ = 0;
    std::uint16_t sound_index_ = 0;
    std::uint16_t haptics_index_ = 0;
    std::uint64_t sound_packets_ = 0;
    bool keyframe_wanted_ = true;
    std::optional<std::vector<std::uint8_t>> held_;  // a packet that goes out late on purpose
    std::uint64_t frames_sent_ = 0;
    std::uint64_t packets_withheld_ = 0;
    std::uint64_t congestion_received_ = 0;
    std::uint64_t congestion_lost_ = 0;
    std::uint64_t input_states_ = 0;      // input state packets that could be read
    std::uint64_t input_at_rest_ = 0;     // of them, those of a controller that nothing is done with
    std::uint64_t input_unreadable_ = 0;
    std::optional<std::uint16_t> input_sequence_;
    std::uint64_t input_out_of_order_ = 0;
    std::uint64_t heartbeats_ = 0;
    std::map<std::uint8_t, std::uint64_t> authentic_;  // packets that are not messages, by kind
    std::map<std::uint8_t, std::uint64_t> rejected_;
    std::uint64_t before_keys_ = 0;
    // The key position of each authentic packet from the client, and the size it counts for.
    std::map<std::uint64_t, std::size_t> positions_;
    bool positions_checkable_ = true;
    std::vector<std::string> events_;
};

}  // namespace psvp::proto::transport
