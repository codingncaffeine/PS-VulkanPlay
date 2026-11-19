// Fuzz target for the JSON reader: any text is read or refused, and what was
// read writes back to text that reads to the same thing.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "app/json.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using psvp::app::Json;
    const auto json = Json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!json) return 0;
    const std::string once = json->dump();
    const auto again = Json::parse(once);
    if (!again || again->dump() != once) std::abort();
    return 0;
}
