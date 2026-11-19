// Fuzz target for the transport's control packets and its opening handshake:
// any datagram, at any step, is handled or ignored.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "proto/transport.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace psvp::proto::transport;
    const std::span<const std::uint8_t> datagram(data, size);
    if (const auto packet = parse_control(datagram)) {
        // What was read writes back to the same bytes, apart from the kind byte's high bits.
        const auto again = encode_control(*packet);
        if (again.size() != size) std::abort();
        for (std::size_t i = 1; i < size; ++i) {
            if (again[i] != data[i]) std::abort();
        }
    }
    const TimePoint now{};
    ClientHandshake client(0x01020304);
    (void)client.start(now);
    (void)client.on_datagram(datagram, now);
    (void)client.on_datagram(datagram, now);
    ConsoleHandshake console(0x0a0b0c0d, {});
    (void)console.on_datagram(datagram);
    (void)console.on_datagram(datagram);
    return 0;
}
