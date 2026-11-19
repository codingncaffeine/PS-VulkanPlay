// Results computed by the reference implementation for fixed inputs
// (handshake key 10 11 .. 1f, shared secret 80 81 .. 9f, the text "The quick
// brown fox jumps over the lazy dog", packets of forty counted bytes at key
// position 42, private keys 01 02 .. 20 and a1 a2 .. c0), printed by
// tools/oracle/run.sh --vectors-stream. They let the unit tests check the
// stream's keys on machines that do not have the reference.
#pragma once

#include <string_view>

namespace psvp::test::stream_vectors {

// keys of the client's direction, computed by the reference
constexpr std::string_view k_client_key = "07f122c5bf8ec2989a13696237baa99a";
constexpr std::string_view k_client_iv = "f765dabcad9d1b7bfeb78b3a65215e1a";
constexpr std::string_view k_client_mac_key = "6b5ad9d02c6a657c93fd4f807b52c225";
constexpr std::string_view k_client_mac_key_at_135001 = "8155881fc50f0bc28a72ef70878c6155";
constexpr std::string_view k_client_code_at_0 = "ef9fea66";
constexpr std::string_view k_client_code_at_42 = "f1ca82b0";
constexpr std::string_view k_client_code_at_135001 = "81bb9750";
constexpr std::string_view k_client_sealed_at_37 = "455147b409d2e0d3d331abad27d74514946e73ecb7d175267068a8baca775fa1d62d298a01fe5c1f6fdf3f";
constexpr std::string_view k_client_control_packet_code = "c6ece798";
constexpr std::string_view k_client_sound_packet_code = "0698280f";
// keys of the console's direction, computed by the reference
constexpr std::string_view k_console_key = "4784871f2dff3d6a7a48ef830cd59b76";
constexpr std::string_view k_console_iv = "9f3a937f3c756b380d5af5b9a1227eaf";
constexpr std::string_view k_console_mac_key = "d2c6433beae57d60e47b591f55694c8d";
constexpr std::string_view k_console_mac_key_at_135001 = "a9bcf48e035620a7f196bd8e7261ee5a";
constexpr std::string_view k_console_code_at_0 = "5e9d7bc1";
constexpr std::string_view k_console_code_at_42 = "7d7ee5cd";
constexpr std::string_view k_console_code_at_135001 = "1817f7a1";
constexpr std::string_view k_console_sealed_at_37 = "df569e553eed733156d0fd34f1f1ee3f366fda61e2584e720d713aed20e1c9880d7b60f8e510f8a2143a23";
constexpr std::string_view k_console_control_packet_code = "6b3c7e68";
constexpr std::string_view k_console_sound_packet_code = "e87d9355";
// the key exchange of two fixed private keys; the reference holds the second
constexpr std::string_view k_public_b = "047fe88a048002318b8c95655892209237a4c81420909194274cc207c14bc19877e744611f01a81021c5131a190064d1502f0a69b2ccf9178a2acff5de578a341f";
constexpr std::string_view k_signature_b = "1b889060ab6c2134185e33adeb1c8858adbecaba07f6b3ed5eaf931487969d87";
constexpr std::string_view k_shared_secret = "bda64a228d373cd08750907449fcb0fa2d6fd98c3197ca864aeaefbdebbd66ab";
// the launch specification, formatted by the reference (ps4)
constexpr std::string_view k_ps4_launch_spec = R"spec({"sessionId":"sessionId4321","streamResolutions":[{"resolution":{"width":1280,"height":720},"maxFps":60,"score":10}],"network":{"bwKbpsSent":10000,"bwLoss":0.001000,"mtu":1454,"rtt":3,"ports":[53,2053]},"slotId":1,"appSpecification":{"minFps":30,"minBandwidth":0,"extTitleId":"ps3","version":1,"timeLimit":1,"startTimeout":100,"afkTimeout":100,"afkTimeoutDisconnect":100},"konan":{"ps3AccessToken":"accessToken","ps3RefreshToken":"refreshToken"},"requestGameSpecification":{"model":"bravia_tv","platform":"android","audioChannels":"5.1","language":"sp","acceptButton":"X","connectedControllers":["xinput","ds3","ds4"],"yuvCoefficient":"bt601","videoEncoderProfile":"hw4.1","audioEncoderProfile":"audio1"},"userProfile":{"onlineId":"psnId","npId":"npId","region":"US","languagesUsed":["en","jp"]},"handshakeKey":"EBESExQVFhcYGRobHB0eHw=="})spec";
// the launch specification, formatted by the reference (ps5)
constexpr std::string_view k_ps5_launch_spec = R"spec({"sessionId":"sessionId4321","streamResolutions":[{"resolution":{"width":1280,"height":720},"maxFps":60,"score":10}],"network":{"bwKbpsSent":10000,"bwLoss":0.001000,"mtu":1454,"rtt":3,"ports":[53,2053]},"slotId":1,"appSpecification":{"minFps":30,"minBandwidth":0,"extTitleId":"ps3","version":1,"timeLimit":1,"startTimeout":100,"afkTimeout":100,"afkTimeoutDisconnect":100},"konan":{"ps3AccessToken":"accessToken","ps3RefreshToken":"refreshToken"},"requestGameSpecification":{"model":"bravia_tv","platform":"android","audioChannels":"5.1","language":"sp","acceptButton":"X","connectedControllers":["xinput","ds3","ds4"],"yuvCoefficient":"bt601","videoEncoderProfile":"hw4.1","audioEncoderProfile":"audio1","adaptiveStreamMode": "resize"},"userProfile":{"onlineId":"psnId","npId":"npId","region":"US","languagesUsed":["en","jp"]},"videoCodec":"hevc","dynamicRange":"SDR","handshakeKey":"EBESExQVFhcYGRobHB0eHw=="})spec";

}  // namespace psvp::test::stream_vectors
