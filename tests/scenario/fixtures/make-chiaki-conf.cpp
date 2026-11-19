// Writes tests/scenario/fixtures/Chiaki.conf with Qt's own settings class, the
// way Chiaki and chiaki-ng write theirs, so that the import is tested against
// the real file format and not against our idea of it. Not part of the build:
//
//   g++ -std=c++17 -fPIC make-chiaki-conf.cpp $(pkg-config --cflags --libs Qt6Core) -o make-chiaki-conf
//   ./make-chiaki-conf Chiaki.conf
//
// Every value is made up; no console or account stands behind it.
#include <QByteArray>
#include <QSettings>
#include <QString>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    QSettings settings(QString::fromLocal8Bit(argv[1]), QSettings::IniFormat);
    settings.setValue("settings/psn_account_id", "ywT7cR8BAAA=");

    settings.beginWriteArray("registered_hosts");

    // A PS5, with a name full of characters the format has to quote or escape,
    // and a secret holding every kind of byte that needs care.
    settings.setArrayIndex(0);
    settings.setValue("target", 1000100);
    settings.setValue("ap_ssid", "PS5-123");
    settings.setValue("ap_bssid", "02005e102030");
    settings.setValue("ap_key", "ap key, with = and ;");
    settings.setValue("ap_name", "PS5");
    settings.setValue("server_nickname", QString::fromUtf8("Wohnzimmer PS5; \"S\xc3\xbc" "d\", =1"));
    settings.setValue("server_mac", QByteArray::fromHex("02005e102030"));
    settings.setValue("rp_regist_key", QByteArray("5e6f7a8b\0\0\0\0\0\0\0\0", 16));
    settings.setValue("rp_key_type", 2);
    settings.setValue("rp_key", QByteArray::fromHex("000a22315c7fff3b2c3d29402061fe0d"));
    settings.setValue("console_pin", "");

    // A PS4 on current system software.
    settings.setArrayIndex(1);
    settings.setValue("target", 1000);
    settings.setValue("server_nickname", "Den");
    settings.setValue("server_mac", QByteArray::fromHex("02005e102031"));
    settings.setValue("rp_regist_key", QByteArray("0a1b2c3d\0\0\0\0\0\0\0\0", 16));
    settings.setValue("rp_key_type", 2);
    settings.setValue("rp_key", QByteArray::fromHex("101112131415161718191a1b1c1d1e1f"));

    // A PS4 registered long ago, on system software this program does not speak to.
    settings.setArrayIndex(2);
    settings.setValue("target", 800);
    settings.setValue("server_nickname", "Attic");
    settings.setValue("server_mac", QByteArray::fromHex("02005e102032"));
    settings.setValue("rp_regist_key", QByteArray("77778888\0\0\0\0\0\0\0\0", 16));
    settings.setValue("rp_key_type", 2);
    settings.setValue("rp_key", QByteArray::fromHex("202122232425262728292a2b2c2d2e2f"));

    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError ? 0 : 1;
}
