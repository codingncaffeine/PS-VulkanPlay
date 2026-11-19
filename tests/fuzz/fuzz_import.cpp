// Fuzz target for the import of another client's settings file: any file is
// read or refused, and every console it yields is complete.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "app/chiaki_import.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)psvp::app::decode_ini_value(text);
    if (const auto config = psvp::app::parse_chiaki_config(text)) {
        for (const auto& console : config->consoles) {
            if (console.regist_key.empty() || console.regist_key.size() > 16) std::abort();
        }
    }
    return 0;
}
