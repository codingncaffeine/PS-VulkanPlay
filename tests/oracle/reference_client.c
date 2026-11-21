// The reference implementation's own client code, driven from the command
// line, for one purpose: to show that the mock console the tests use is a
// console as far as the reference can tell. If the reference can find it,
// register with it and wake it, the mock is not merely agreeing with our own
// reading of the protocol.
//
//   psvp_reference_client discover HOST ps5|ps4
//   psvp_reference_client register HOST ps5|ps4 ACCOUNT_BASE64 PIN
//   psvp_reference_client wake HOST ps5|ps4 REGIST_KEY
//   psvp_reference_client session HOST ps5|ps4 REGIST_KEY SECRET_HEX SECONDS
//       starts a whole session and stops it after SECONDS (the mock console
//       only goes as far as this project has implemented)
//       With PSVP_REFERENCE_TAPS=1 it presses right one second after the session
//       is connected and cross half a second later, each for 100 ms.
//       With PSVP_REFERENCE_WATCH=1 it asks for 1080p, 60 pictures a second and HEVC, as `play`
//       does, and prints only what tells how the console judges the link: its view of the
//       connection (target bitrate, round-trip value, loss) and the messages the reference sends
//       again, each with the time since the program started. For a comparison on a real link.
//       With PSVP_REFERENCE_DUMP=FOLDER in the environment it keeps what the
//       session hands to a decoder: FOLDER/video.bin (every picture sample, in
//       order) and FOLDER/sound.bin (every sound frame, in order).
//
// Test tool only: built by the oracle lane, never installed.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/controller.h>
#include <chiaki/discovery.h>
#include <chiaki/log.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>

static volatile int g_done = 0;
static volatile int g_ok = 0;
static volatile int g_connected = 0;

static void wait_done(int tenths) {
    for (int i = 0; i < tenths && !g_done; ++i) usleep(100 * 1000);
}

static void on_host(ChiakiDiscoveryHost *host, void *user) {
    (void)user;
    printf("reference: found name=%s type=%s state=%s host-id=%s system-version=%s port=%u\n",
           host->host_name ? host->host_name : "", host->host_type ? host->host_type : "",
           chiaki_discovery_host_state_string(host->state), host->host_id ? host->host_id : "",
           host->system_version ? host->system_version : "", (unsigned)host->host_request_port);
    g_ok = 1;
    g_done = 1;
}

static void on_regist(ChiakiRegistEvent *event, void *user) {
    (void)user;
    if (event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS) {
        const ChiakiRegisteredHost *host = event->registered_host;
        char regist_key[CHIAKI_SESSION_AUTH_SIZE + 1] = {0};
        memcpy(regist_key, host->rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
        printf("reference: registered nickname=%s mac=", host->server_nickname);
        for (int i = 0; i < 6; ++i) printf("%02x", host->server_mac[i]);
        printf(" regist-key=%s key-type=%u key=", regist_key, (unsigned)host->rp_key_type);
        for (int i = 0; i < 16; ++i) printf("%02x", host->rp_key[i]);
        printf("\n");
        g_ok = 1;
    } else {
        printf("reference: registration %s\n",
               event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED ? "canceled" : "failed");
    }
    g_done = 1;
}

static void on_session_event(ChiakiEvent *event, void *user) {
    (void)user;
    if (event->type == CHIAKI_EVENT_QUIT) {
        printf("reference: session ended: %s\n", chiaki_quit_reason_string(event->quit.reason));
        g_done = 1;
    } else if (event->type == CHIAKI_EVENT_LOGIN_PIN_REQUEST) {
        printf("reference: the console asks for its passcode\n");
    } else if (event->type == CHIAKI_EVENT_CONNECTED) {
        printf("reference: connected\n");
        g_connected = 1;
    }
}

static FILE *g_video = NULL;
static FILE *g_sound = NULL;
static unsigned g_video_samples = 0;
static unsigned g_sound_frames = 0;
static unsigned g_sound_concealed = 0;

static bool on_video_sample(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user) {
    (void)frames_lost; (void)frame_recovered; (void)user;
    if (g_video) fwrite(buf, 1, buf_size, g_video);
    ++g_video_samples;
    return true;
}

static void on_audio_header(ChiakiAudioHeader *header, void *user) {
    (void)header; (void)user;
}

static void on_audio_frame(uint8_t *buf, size_t buf_size, void *user) {
    (void)user;
    if (!buf) { ++g_sound_concealed; return; }  /* a frame it gave up on */
    if (g_sound) fwrite(buf, 1, buf_size, g_sound);
    ++g_sound_frames;
}

#include <time.h>
static struct timespec g_started;
// PSVP_REFERENCE_WATCH: the lines that say how the console judges the link, with their time.
static void watch_log(ChiakiLogLevel level, const char *msg, void *user) {
    (void)level; (void)user;
    if (!strstr(msg, "connection quality") && !strstr(msg, "re-sending") && !strstr(msg, "Hit max retries")) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = (double)(now.tv_sec - g_started.tv_sec) + (double)(now.tv_nsec - g_started.tv_nsec) / 1e9;
    printf("%.1f s: %s\n", seconds, msg);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s discover|register|wake HOST ps5|ps4 [...]\n", argv[0]);
        return 2;
    }
    const char *command = argv[1];
    const char *host = argv[2];
    const bool ps5 = strcmp(argv[3], "ps5") == 0;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (chiaki_lib_init() != CHIAKI_ERR_SUCCESS) return 3;
    ChiakiLog log;
    const bool watch = getenv("PSVP_REFERENCE_WATCH") != NULL;
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    if (watch) chiaki_log_init(&log, CHIAKI_LOG_ALL, watch_log, NULL);
    else chiaki_log_init(&log, CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG), chiaki_log_cb_print, NULL);

    if (strcmp(command, "discover") == 0) {
        ChiakiDiscovery discovery;
        if (chiaki_discovery_init(&discovery, &log, AF_INET) != CHIAKI_ERR_SUCCESS) return 3;
        ChiakiDiscoveryThread thread;
        if (chiaki_discovery_thread_start_oneshot(&thread, &discovery, on_host, NULL) != CHIAKI_ERR_SUCCESS) return 3;
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4);
        inet_pton(AF_INET, host, &address.sin_addr);
        ChiakiDiscoveryPacket packet;
        memset(&packet, 0, sizeof(packet));
        packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
        packet.protocol_version = ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
        chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&address, sizeof(address));
        wait_done(30);
        chiaki_discovery_thread_stop(&thread);
        chiaki_discovery_fini(&discovery);
        if (!g_ok) printf("reference: nothing found\n");
        return g_ok ? 0 : 1;
    }

    if (strcmp(command, "register") == 0 && argc == 6) {
        ChiakiRegistInfo info;
        memset(&info, 0, sizeof(info));
        info.target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
        info.host = host;
        info.broadcast = false;
        size_t account_size = sizeof(info.psn_account_id);
        if (chiaki_base64_decode(argv[4], strlen(argv[4]), info.psn_account_id, &account_size) != CHIAKI_ERR_SUCCESS ||
            account_size != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            fprintf(stderr, "bad account id\n");
            return 2;
        }
        info.pin = (uint32_t)strtoul(argv[5], NULL, 10);
        ChiakiRegist regist;
        if (chiaki_regist_start(&regist, &log, &info, on_regist, NULL) != CHIAKI_ERR_SUCCESS) return 3;
        wait_done(120);
        chiaki_regist_stop(&regist);
        chiaki_regist_fini(&regist);
        return g_ok ? 0 : 1;
    }

    if (strcmp(command, "wake") == 0 && argc == 5) {
        const uint64_t credential = (uint64_t)strtoull(argv[4], NULL, 16);
        const ChiakiErrorCode result = chiaki_discovery_wakeup(&log, NULL, host, credential, ps5);
        printf("reference: wake-up %s\n", result == CHIAKI_ERR_SUCCESS ? "sent" : "not sent");
        return result == CHIAKI_ERR_SUCCESS ? 0 : 1;
    }

    if (strcmp(command, "session") == 0 && argc == 7) {
        ChiakiConnectInfo info;
        memset(&info, 0, sizeof(info));
        info.ps5 = ps5;
        info.host = host;
        strncpy(info.regist_key, argv[4], sizeof(info.regist_key));
        if (strlen(argv[5]) != 32) return 2;
        for (int i = 0; i < 16; ++i) info.morning[i] = (uint8_t)((hex_value(argv[5][2 * i]) << 4) | hex_value(argv[5][2 * i + 1]));
        chiaki_connect_video_profile_preset(&info.video_profile, watch ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p : CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
                                            CHIAKI_VIDEO_FPS_PRESET_60);
        if (watch) info.video_profile.codec = CHIAKI_CODEC_H265;
        info.video_profile_auto_downgrade = true;
        static ChiakiSession session;
        if (chiaki_session_init(&session, &info, &log) != CHIAKI_ERR_SUCCESS) return 3;
        chiaki_session_set_event_cb(&session, on_session_event, NULL);
        const char *dump = getenv("PSVP_REFERENCE_DUMP");
        if (dump) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/video.bin", dump);
            g_video = fopen(path, "wb");
            snprintf(path, sizeof(path), "%s/sound.bin", dump);
            g_sound = fopen(path, "wb");
            if (!g_video || !g_sound) return 3;
        }
        chiaki_session_set_video_sample_cb(&session, on_video_sample, NULL);
        ChiakiAudioSink sink;
        memset(&sink, 0, sizeof(sink));
        sink.header_cb = on_audio_header;
        sink.frame_cb = on_audio_frame;
        chiaki_session_set_audio_sink(&session, &sink);
        if (chiaki_session_start(&session) != CHIAKI_ERR_SUCCESS) return 3;
        if (getenv("PSVP_REFERENCE_TAPS")) {
            /* In steps of 10 ms from the moment the session is connected: right down at 1 s and up at 1.1 s,
             * cross down at 1.5 s and up at 1.6 s. */
            ChiakiControllerState pad;
            chiaki_controller_state_set_idle(&pad);
            int since = 0;
            for (int i = 0; i < atoi(argv[6]) * 100 && !g_done; ++i) {
                usleep(10 * 1000);
                if (!g_connected) continue;
                ++since;
                uint32_t buttons = pad.buttons;
                if (since == 100) buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
                if (since == 110) buttons &= ~(uint32_t)CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
                if (since == 150) buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
                if (since == 160) buttons &= ~(uint32_t)CHIAKI_CONTROLLER_BUTTON_CROSS;
                if (buttons != pad.buttons) {
                    pad.buttons = buttons;
                    chiaki_session_set_controller_state(&session, &pad);
                }
            }
        }
        wait_done(g_done ? 0 : (getenv("PSVP_REFERENCE_TAPS") ? 0 : atoi(argv[6]) * 10));
        chiaki_session_stop(&session);
        chiaki_session_join(&session);
        chiaki_session_fini(&session);
        if (g_video) fclose(g_video);
        if (g_sound) fclose(g_sound);
        printf("reference: %u picture samples, %u sound frames, %u sound frames given up on\n", g_video_samples, g_sound_frames,
               g_sound_concealed);
        printf("reference: session stopped\n");
        return 0;
    }

    fprintf(stderr, "unknown command or wrong number of arguments\n");
    return 2;
}
