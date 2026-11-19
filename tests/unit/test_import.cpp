#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "app/account_id.h"
#include "app/chiaki_import.h"
#include "check.h"
#include "proto/hex.h"

using psvp::app::decode_ini_value;

namespace {

std::string fixture() {
    std::ifstream stream(std::string(PSVP_TEST_FIXTURES) + "/Chiaki.conf", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

std::string hex(const std::vector<std::uint8_t>& bytes) { return psvp::proto::hex_encode(bytes); }

}  // namespace

// The escapes as Qt's settings class writes them (the lines are from a file it wrote).
PSVP_TEST(import_undoes_the_value_escapes) {
    CHECK(decode_ini_value("PS5").text == "PS5");
    CHECK(decode_ini_value(R"("ap key, with = and ;")").text == "ap key, with = and ;");
    CHECK(decode_ini_value(R"("ywT7cR8BAAA=")").text == "ywT7cR8BAAA=");
    CHECK(decode_ini_value("\"Wohnzimmer PS5; \\\"S\xc3\xbc" "d\\\", =1\"").text == "Wohnzimmer PS5; \"S\xc3\xbc" "d\", =1");

    const auto mac = decode_ini_value(R"ini(@ByteArray(\x2\0^\x10 0))ini");
    CHECK(mac.is_byte_array && hex(mac.bytes) == "02005e102030");
    const auto key = decode_ini_value(R"ini("@ByteArray(\0\n\"1\\\x7f\xff;,=)@ a\xfe\r)")ini");
    CHECK(key.is_byte_array && hex(key.bytes) == "000a22315c7fff3b2c3d29402061fe0d");
    const auto plain = decode_ini_value(R"ini("@ByteArray( !\"#$%&'()*+,-./)")ini");
    CHECK(plain.is_byte_array && hex(plain.bytes) == "202122232425262728292a2b2c2d2e2f");
    const auto regist_key = decode_ini_value(R"ini(@ByteArray(5e6f7a8b\0\0\0\0\0\0\0\0))ini");
    CHECK(regist_key.is_byte_array && regist_key.bytes.size() == 16 && regist_key.bytes[8] == 0);
}

PSVP_TEST(import_reads_other_escapes_the_format_allows) {
    CHECK(decode_ini_value(R"(a\tb\\c)").text == "a\tb\\c");
    CHECK(decode_ini_value(R"(\x41\x42)").text == "AB");
    CHECK(decode_ini_value(R"(\101\102)").text == "AB");
    CHECK(decode_ini_value(R"(\xfc)").text == "\xc3\xbc");  // a character number above 127, as text
    CHECK(decode_ini_value(R"(@@home)").text == "@home");
    CHECK(!decode_ini_value("plain").is_byte_array);
    CHECK(decode_ini_value("").text.empty());
    CHECK(decode_ini_value("trailing\\").text == "trailing\\");
}

PSVP_TEST(import_reads_a_file_written_by_the_other_client) {
    const std::string text = fixture();
    CHECK(!text.empty());
    const auto config = psvp::app::parse_chiaki_config(text);
    CHECK(config.has_value());
    if (!config) return;
    CHECK(config->account_id.has_value() && psvp::app::account_id_decimal(*config->account_id) == "1234567890123");
    CHECK(config->consoles.size() == 2);
    if (config->consoles.size() != 2) return;

    const auto& ps5 = config->consoles[0];
    CHECK(ps5.kind == psvp::proto::ConsoleKind::ps5);
    CHECK(ps5.nickname == "Wohnzimmer PS5; \"S\xc3\xbc" "d\", =1");
    CHECK(psvp::proto::hex_encode(ps5.mac) == "02005e102030");
    CHECK(ps5.regist_key == "5e6f7a8b");
    CHECK(psvp::proto::hex_encode(ps5.key) == "000a22315c7fff3b2c3d29402061fe0d");
    CHECK(ps5.key_type == 2);
    CHECK(ps5.ap_ssid == "PS5-123");
    CHECK(ps5.ap_key == "ap key, with = and ;");

    const auto& ps4 = config->consoles[1];
    CHECK(ps4.kind == psvp::proto::ConsoleKind::ps4);
    CHECK(ps4.nickname == "Den");
    CHECK(ps4.regist_key == "0a1b2c3d");
    CHECK(psvp::proto::hex_encode(ps4.key) == "101112131415161718191a1b1c1d1e1f");

    CHECK(config->skipped.size() == 1);
    CHECK(config->skipped.size() == 1 && config->skipped[0].find("Attic") != std::string::npos);
    CHECK(config->skipped.size() == 1 && config->skipped[0].find("older than 8.00") != std::string::npos);
}

PSVP_TEST(import_of_a_file_without_consoles_is_empty) {
    const auto empty = psvp::app::parse_chiaki_config("");
    CHECK(empty.has_value() && empty->consoles.empty() && !empty->account_id.has_value());
    const auto settings_only = psvp::app::parse_chiaki_config("[settings]\npsn_account_id=\"ywT7cR8BAAA=\"\n");
    CHECK(settings_only.has_value() && settings_only->account_id.has_value() && settings_only->consoles.empty());
}

PSVP_TEST(import_skips_entries_it_cannot_use_and_says_why) {
    const std::string text = "[registered_hosts]\n"
                             "1\\target=1000100\n"
                             "1\\server_nickname=No Key\n"
                             "1\\server_mac=@ByteArray(abcdef)\n"
                             "1\\rp_regist_key=@ByteArray(12345678)\n"
                             "2\\target=1000000\n"
                             "2\\server_nickname=Unknown Kind\n"
                             "3\\target=1000100\n"
                             "3\\server_nickname=Short Mac\n"
                             "3\\server_mac=@ByteArray(abc)\n"
                             "3\\rp_regist_key=@ByteArray(12345678)\n"
                             "3\\rp_key=@ByteArray(0123456789abcdef)\n"
                             "size=3\n";
    const auto config = psvp::app::parse_chiaki_config(text);
    CHECK(config.has_value());
    if (!config) return;
    CHECK(config->consoles.empty());
    CHECK(config->skipped.size() == 3);
    CHECK(!psvp::app::parse_chiaki_config("[registered_hosts]\nsize=100000\n").has_value());
}

PSVP_TEST(import_knows_where_the_other_clients_keep_their_file) {
    const std::map<std::string, std::string, std::less<>> values{{"HOME", "/home/someone"}};
    const auto candidates = psvp::app::chiaki_config_candidates([&](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    });
    CHECK(candidates.size() == 3);
    if (candidates.size() != 3) return;
    CHECK(candidates[0] == "/home/someone/.config/Chiaki/Chiaki.conf");
    CHECK(candidates[1] == "/home/someone/.var/app/io.github.streetpea.Chiaki4deck/config/Chiaki/Chiaki.conf");
    CHECK(candidates[2] == "/home/someone/.var/app/re.chiaki.Chiaki/config/Chiaki/Chiaki.conf");
}
