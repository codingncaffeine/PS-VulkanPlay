# The stream transport

Everything after the control channel travels over UDP, in a transport of the protocol's own that is modeled on
SCTP: a four-step opening handshake, numbered and acknowledged messages for what must arrive, and separate
packet types for what may be lost (picture, sound, input). A session uses it twice: first briefly on port 9297
to measure the network, then on port 9296 for the stream.

This page covers the packet layout and the opening handshake. Messages, acknowledgment, encryption and the
media packets follow as they are implemented.

## Packets

**[read]** The first byte of every datagram tells its kind in its low four bits:

| Value | Kind |
|---|---|
| 0 | control: handshake steps, messages and their acknowledgments |
| 1 | input history |
| 2 | video |
| 3 | audio |
| 4 | handshake |
| 5 | congestion report |
| 6 | input state |
| 8 | client information |

A control packet continues with a sixteen-byte header and a payload (numbers are most significant byte first):

| Offset | Size | Content |
|---|---|---|
| 1 | 4 | the tag of the receiving side (zero in the very first packet, when it is not known yet) |
| 5 | 4 | authentication code; zero until the stream keys exist |
| 9 | 4 | key position; zero until the stream keys exist |
| 13 | 1 | chunk type |
| 14 | 1 | chunk flags |
| 15 | 2 | payload size plus four |
| 17 | | the payload |

The size field must match the datagram: the bytes after the kind byte are always the field's value plus twelve.

| Chunk type | Meaning |
|---|---|
| 0 | data |
| 1 | init |
| 2 | init acknowledged |
| 3 | data acknowledged |
| 10 | cookie |
| 11 | cookie acknowledged |

## The opening handshake

Each side has a *tag*, a random 32-bit number that the other side then writes into every control packet it
sends.

1. **Init** (client). Header tag zero. Payload, sixteen bytes: the client's tag, a receive window of
   `0x19000`, the number of outbound streams (100), of inbound streams (100), and the client's first sequence
   number, which is its tag.
2. **Init acknowledged** (console). Header tag is the client's. Payload: the same five fields from the
   console's side, followed by a cookie of 32 bytes.
3. **Cookie** (client). Header tag is the console's. Payload: the cookie, unchanged.
4. **Cookie acknowledged** (console). Header tag is the client's. No payload.

**[read]** The client refuses an init acknowledgment whose tag is zero, whose stream counts are zero, whose
outbound count is above 100 or whose inbound count is below 100. It sends each of its two packets up to three
times, waiting five seconds for the answer each time, and ignores an init acknowledgment that arrives again
while it waits for the cookie's. The console's first sequence number is taken to be its tag.

## What has been confirmed

The layout and the handshake are **[read]**. They are tested the same two ways as the session start: the mock
console takes this client through the handshake, and the reference's own transport code completes the
handshake with the same mock (its log then says the transport is connected), with the mock recording what each
client's init carried so that the tests can compare them.

`src/proto/transport.h`
