/* The reference's generated message coder, behind plain C functions
 * (reference_messages.c). Each encoder returns the number of bytes written, 0 on failure. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSVP_REF_MAX_RESOLUTIONS 8

size_t psvp_ref_encode_big(uint32_t version, const char *session_key, const char *launch_spec, const uint8_t *encrypted_key,
                           size_t encrypted_key_size, const uint8_t *public_key, size_t public_key_size, const uint8_t *signature,
                           size_t signature_size, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_plain(uint32_t type, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_controller(bool dualsense, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_audio_info(const uint8_t *audio_header, size_t audio_header_size, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_disconnect(const char *reason, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_corrupt_frames(uint32_t start, uint32_t end, uint8_t *out, size_t out_size);
size_t psvp_ref_encode_connection_quality(uint32_t target_bitrate, uint32_t upstream_bitrate, float upstream_loss,
                                          bool disable_upstream_audio, double rtt, uint64_t loss, uint8_t *out, size_t out_size);

struct psvp_ref_bang {
    uint32_t server_version;
    uint32_t token;
    bool encrypted_key_accepted;
    bool version_accepted;
    uint8_t public_key[128];
    size_t public_key_size;
    uint8_t signature[32];
    size_t signature_size;
};
bool psvp_ref_decode_bang(const uint8_t *in, size_t in_size, struct psvp_ref_bang *out);

struct psvp_ref_stream_info {
    size_t resolutions;
    uint32_t width[PSVP_REF_MAX_RESOLUTIONS];
    uint32_t height[PSVP_REF_MAX_RESOLUTIONS];
    uint8_t video_header[PSVP_REF_MAX_RESOLUTIONS][256];
    size_t video_header_size[PSVP_REF_MAX_RESOLUTIONS];
    uint8_t audio_header[14];
    size_t audio_header_size;
};
bool psvp_ref_decode_stream_info(const uint8_t *in, size_t in_size, struct psvp_ref_stream_info *out);

#ifdef __cplusplus
}
#endif
