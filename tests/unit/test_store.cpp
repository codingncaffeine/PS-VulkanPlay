#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "app/account_id.h"
#include "app/console_store.h"
#include "check.h"
#include "net/socket.h"

using psvp::app::ConsoleStore;
using psvp::app::StoredConsole;

namespace {

// A folder of this test's own, emptied at its start.
std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / ("psvp-test-" + std::to_string(::getpid())) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

StoredConsole sample(const char* nickname, std::uint8_t last_mac_byte) {
    StoredConsole console;
    console.registration.kind = psvp::proto::ConsoleKind::ps5;
    console.registration.nickname = nickname;
    console.registration.mac = {0x02, 0x00, 0x5e, 0x10, 0x20, last_mac_byte};
    console.registration.regist_key = "1a2b3c4d";
    console.registration.key = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    console.registration.key_type = 2;
    console.registration.ap_ssid = "PS5-123";
    console.last_address = "192.168.1.20";
    return console;
}

}  // namespace

PSVP_TEST(account_id_reads_both_written_forms) {
    const auto from_number = psvp::app::parse_account_id("1234567890123");
    const auto from_base64 = psvp::app::parse_account_id("ywT7cR8BAAA=");
    CHECK(from_number.has_value() && from_base64.has_value());
    if (!from_number || !from_base64) return;
    CHECK(*from_number == *from_base64);
    CHECK(psvp::app::account_id_base64(*from_number) == "ywT7cR8BAAA=");
    CHECK(psvp::app::account_id_decimal(*from_base64) == "1234567890123");
    CHECK(psvp::app::account_id_decimal(psvp::app::parse_account_id("18446744073709551615").value()) == "18446744073709551615");
    CHECK(psvp::app::account_id_decimal(psvp::app::parse_account_id("0").value()) == "0");
}

PSVP_TEST(account_id_refuses_anything_else) {
    for (const char* text : {"", "abc", "-5", "12 34", "18446744073709551616", "ywT7cR8BAAA", "ywT7cR8BAAAA", "ywT7cR8BAA==",
                             "!wT7cR8BAAA=", "0x1234"}) {
        CHECK(!psvp::app::parse_account_id(text).has_value());
    }
}

PSVP_TEST(store_of_a_missing_file_is_empty) {
    const auto store = psvp::app::load_store(scratch("missing") / "consoles.json");
    CHECK(store.has_value() && store->consoles.empty() && !store->account_id.has_value());
}

PSVP_TEST(store_keeps_what_was_saved) {
    const auto file = scratch("roundtrip") / "deeper" / "consoles.json";
    ConsoleStore store;
    store.account_id = psvp::app::parse_account_id("1234567890123");
    store.put(sample("Living Room", 0x30));
    store.put(sample("Den \"two\"", 0x31));
    CHECK(psvp::app::save_store(file, store).has_value());

    const auto loaded = psvp::app::load_store(file);
    CHECK(loaded.has_value());
    if (!loaded) return;
    CHECK(loaded->account_id == store.account_id);
    CHECK(loaded->consoles.size() == 2);
    const auto& first = loaded->consoles[0];
    CHECK(first.registration.nickname == "Living Room");
    CHECK(first.registration.kind == psvp::proto::ConsoleKind::ps5);
    CHECK(first.registration.mac == store.consoles[0].registration.mac);
    CHECK(first.registration.regist_key == "1a2b3c4d");
    CHECK(first.registration.key == store.consoles[0].registration.key);
    CHECK(first.registration.key_type == 2);
    CHECK(first.registration.ap_ssid == "PS5-123");
    CHECK(first.last_address == "192.168.1.20");
    CHECK(loaded->consoles[1].registration.nickname == "Den \"two\"");
}

PSVP_TEST(store_file_is_for_its_owner_only_and_leaves_no_scraps) {
    const auto dir = scratch("mode");
    const auto file = dir / "consoles.json";
    ConsoleStore store;
    store.put(sample("Living Room", 0x30));
    CHECK(psvp::app::save_store(file, store).has_value());
    CHECK(psvp::app::save_store(file, store).has_value());  // over an existing file too
    struct stat info {};
    CHECK(::stat(file.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++entries;
    }
    CHECK(entries == 1);
}

PSVP_TEST(store_replaces_a_console_with_the_same_address) {
    ConsoleStore store;
    store.put(sample("Old Name", 0x30));
    StoredConsole renamed = sample("New Name", 0x30);
    renamed.last_address.clear();
    store.put(renamed);
    CHECK(store.consoles.size() == 1);
    CHECK(store.consoles[0].registration.nickname == "New Name");
    CHECK(store.consoles[0].last_address == "192.168.1.20");  // a known address is not forgotten
}

PSVP_TEST(store_finds_by_name_or_hardware_address) {
    ConsoleStore store;
    store.put(sample("Living Room", 0x30));
    store.put(sample("Den", 0x31));
    CHECK(store.find("living room") != nullptr && store.find("living room")->registration.nickname == "Living Room");
    CHECK(store.find("DEN") != nullptr);
    CHECK(store.find("02:00:5e:10:20:31") != nullptr && store.find("02:00:5e:10:20:31")->registration.nickname == "Den");
    CHECK(store.find("02005E102030") != nullptr);
    CHECK(store.find("02-00-5e-10-20-30") != nullptr);
    CHECK(store.find("Kitchen") == nullptr);
    CHECK(store.find("") == nullptr);
    CHECK(store.find("02:00:5e:10:20:99") == nullptr);
}

PSVP_TEST(store_refuses_a_damaged_file_and_says_which) {
    const auto dir = scratch("damaged");
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream(dir / name) << text;
        return psvp::app::load_store(dir / name);
    };
    const auto not_json = write("a.json", "{ this is not json");
    CHECK(!not_json.has_value() && not_json.error().find("a.json") != std::string::npos);
    CHECK(!write("b.json", "[]").has_value());
    CHECK(!write("c.json", R"({"version": 99, "consoles": []})").has_value());
    CHECK(!write("d.json", R"({"version": 1, "account_id": "nonsense", "consoles": []})").has_value());
    CHECK(!write("e.json", R"({"version": 1, "consoles": [{"kind": "PS6"}]})").has_value());
    CHECK(!write("f.json", R"({"version": 1, "consoles": [{"kind": "PS5", "mac": "0200", "key": "", "regist_key": "x"}]})").has_value());
    CHECK(write("g.json", R"({"version": 1, "consoles": []})").has_value());
}

PSVP_TEST(addresses_are_read_and_written) {
    using psvp::net::parse_ipv4;
    CHECK(parse_ipv4("192.168.1.20") == 0xc0a80114U);
    CHECK(parse_ipv4("0.0.0.0") == 0U);
    CHECK(parse_ipv4("255.255.255.255") == 0xffffffffU);
    CHECK(psvp::net::ip_to_string(0xc0a80114U) == "192.168.1.20");
    CHECK(psvp::net::to_string(psvp::net::Address{0x7f000001U, 9295}) == "127.0.0.1:9295");
    for (const char* text : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.x", "1..2.3", " 1.2.3.4", "1.2.3.4 ", "1.2.3.-4",
                             "0001.2.3.4", "localhost"}) {
        CHECK(!parse_ipv4(text).has_value());
    }
}
