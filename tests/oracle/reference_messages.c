/* The reference's generated message coder, behind plain C functions for the
 * differential tests (tests/oracle/diff_stream.cpp). Test only: it is compiled
 * against the reference's library and is never part of anything that ships. */
#include "reference_messages.h"

#include <stdlib.h>
#include <string.h>

#include <pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <takion.pb.h>

/* pb_utils.h */
#include "pb_utils.h"

static size_t encode(const tkproto_TakionMessage *msg, uint8_t *out, size_t out_size) {
    pb_ostream_t stream = pb_ostream_from_buffer(out, out_size);
    if (!pb_encode(&stream, tkproto_TakionMessage_fields, msg)) return 0;
    return stream.bytes_written;
}

size_t psvp_ref_encode_big(uint32_t version, const char *session_key, const char *launch_spec, const uint8_t *encrypted_key,
                           size_t encrypted_key_size, const uint8_t *public_key, size_t public_key_size, const uint8_t *signature,
                           size_t signature_size, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    ChiakiPBBuf encrypted = {encrypted_key_size, (uint8_t *)encrypted_key};
    ChiakiPBBuf public_buf = {public_key_size, (uint8_t *)public_key};
    ChiakiPBBuf signature_buf = {signature_size, (uint8_t *)signature};
    msg.type = tkproto_TakionMessage_PayloadType_BIG;
    msg.has_big_payload = true;
    msg.big_payload.client_version = version;
    msg.big_payload.session_key.arg = (void *)session_key;
    msg.big_payload.session_key.funcs.encode = chiaki_pb_encode_string;
    msg.big_payload.launch_spec.arg = (void *)launch_spec;
    msg.big_payload.launch_spec.funcs.encode = chiaki_pb_encode_string;
    msg.big_payload.encrypted_key.arg = &encrypted;
    msg.big_payload.encrypted_key.funcs.encode = chiaki_pb_encode_buf;
    msg.big_payload.ecdh_pub_key.arg = &public_buf;
    msg.big_payload.ecdh_pub_key.funcs.encode = chiaki_pb_encode_buf;
    msg.big_payload.ecdh_sig.arg = &signature_buf;
    msg.big_payload.ecdh_sig.funcs.encode = chiaki_pb_encode_buf;
    return encode(&msg, out, out_size);
}

size_t psvp_ref_encode_plain(uint32_t type, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = (tkproto_TakionMessage_PayloadType)type;
    return encode(&msg, out, out_size);
}

size_t psvp_ref_encode_controller(bool dualsense, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = tkproto_TakionMessage_PayloadType_CONTROLLERCONNECTION;
    msg.has_controller_connection_payload = true;
    msg.controller_connection_payload.has_connected = true;
    msg.controller_connection_payload.connected = true;
    msg.controller_connection_payload.has_controller_id = false;
    msg.controller_connection_payload.has_controller_type = true;
    msg.controller_connection_payload.controller_type = dualsense ? tkproto_ControllerConnectionPayload_ControllerType_DUALSENSE
                                                                  : tkproto_ControllerConnectionPayload_ControllerType_DUALSHOCK4;
    return encode(&msg, out, out_size);
}

size_t psvp_ref_encode_audio_info(const uint8_t *audio_header, size_t audio_header_size, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    ChiakiPBBuf header = {audio_header_size, (uint8_t *)audio_header};
    msg.type = tkproto_TakionMessage_PayloadType_STREAMINFO;
    msg.has_stream_info_payload = true;
    msg.stream_info_payload.audio_header.arg = &header;
    msg.stream_info_payload.audio_header.funcs.encode = chiaki_pb_encode_buf;
    return encode(&msg, out, out_size);
}

size_t psvp_ref_encode_corrupt_frames(uint32_t start, uint32_t end, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = tkproto_TakionMessage_PayloadType_CORRUPTFRAME;
    msg.has_corrupt_payload = true;
    msg.corrupt_payload.start = start;
    msg.corrupt_payload.end = end;
    return encode(&msg, out, out_size);
}

size_t psvp_ref_encode_disconnect(const char *reason, uint8_t *out, size_t out_size) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = tkproto_TakionMessage_PayloadType_DISCONNECT;
    msg.has_disconnect_payload = true;
    msg.disconnect_payload.reason.arg = (void *)reason;
    msg.disconnect_payload.reason.funcs.encode = chiaki_pb_encode_string;
    return encode(&msg, out, out_size);
}

bool psvp_ref_decode_bang(const uint8_t *in, size_t in_size, struct psvp_ref_bang *out) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    memset(out, 0, sizeof(*out));
    ChiakiPBDecodeBuf public_buf = {sizeof(out->public_key), 0, out->public_key};
    ChiakiPBDecodeBuf signature_buf = {sizeof(out->signature), 0, out->signature};
    msg.bang_payload.ecdh_pub_key.arg = &public_buf;
    msg.bang_payload.ecdh_pub_key.funcs.decode = chiaki_pb_decode_buf;
    msg.bang_payload.ecdh_sig.arg = &signature_buf;
    msg.bang_payload.ecdh_sig.funcs.decode = chiaki_pb_decode_buf;
    pb_istream_t stream = pb_istream_from_buffer(in, in_size);
    if (!pb_decode(&stream, tkproto_TakionMessage_fields, &msg)) return false;
    if (msg.type != tkproto_TakionMessage_PayloadType_BANG || !msg.has_bang_payload) return false;
    out->server_version = msg.bang_payload.server_version;
    out->token = msg.bang_payload.token;
    out->encrypted_key_accepted = msg.bang_payload.encrypted_key_accepted;
    out->version_accepted = msg.bang_payload.version_accepted;
    out->public_key_size = public_buf.size;
    out->signature_size = signature_buf.size;
    return true;
}

struct resolutions {
    struct psvp_ref_stream_info *out;
};

static bool decode_resolution(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    (void)field;
    struct psvp_ref_stream_info *out = *arg;
    tkproto_ResolutionPayload resolution;
    memset(&resolution, 0, sizeof(resolution));
    uint8_t header[sizeof(out->video_header[0])];
    ChiakiPBDecodeBuf header_buf = {sizeof(header), 0, header};
    resolution.video_header.arg = &header_buf;
    resolution.video_header.funcs.decode = chiaki_pb_decode_buf;
    if (!pb_decode(stream, tkproto_ResolutionPayload_fields, &resolution)) return false;
    if (out->resolutions >= PSVP_REF_MAX_RESOLUTIONS) return true;
    const size_t i = out->resolutions++;
    out->width[i] = resolution.width;
    out->height[i] = resolution.height;
    out->video_header_size[i] = header_buf.size;
    memcpy(out->video_header[i], header, header_buf.size);
    return true;
}

bool psvp_ref_decode_stream_info(const uint8_t *in, size_t in_size, struct psvp_ref_stream_info *out) {
    tkproto_TakionMessage msg;
    memset(&msg, 0, sizeof(msg));
    memset(out, 0, sizeof(*out));
    ChiakiPBDecodeBuf audio_buf = {sizeof(out->audio_header), 0, out->audio_header};
    msg.stream_info_payload.audio_header.arg = &audio_buf;
    msg.stream_info_payload.audio_header.funcs.decode = chiaki_pb_decode_buf;
    msg.stream_info_payload.resolution.arg = out;
    msg.stream_info_payload.resolution.funcs.decode = decode_resolution;
    pb_istream_t stream = pb_istream_from_buffer(in, in_size);
    if (!pb_decode(&stream, tkproto_TakionMessage_fields, &msg)) return false;
    if (msg.type != tkproto_TakionMessage_PayloadType_STREAMINFO || !msg.has_stream_info_payload) return false;
    out->audio_header_size = audio_buf.size;
    return true;
}
