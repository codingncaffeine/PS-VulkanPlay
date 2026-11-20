# The stream transport

Everything after the control channel travels over UDP, in a transport of the protocol's own that is modeled on
SCTP: a four-step opening handshake, numbered and acknowledged messages for what must arrive, and separate
packet types for what may be lost (picture, sound, input). A session uses it twice: first briefly on port 9297
to measure the network, then on port 9296 for the stream.

This page covers the packet layout, the opening handshake and the messages that must arrive. Encryption is
in [stream-setup.md](stream-setup.md), the stream's picture and sound packets in [video.md](video.md). The
messages themselves are listed in [messages.md](messages.md); the first connection's exchange is in
[probe.md](probe.md).

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

## Messages that must arrive

After the handshake each side numbers what it sends in *data* chunks, starting from the first sequence number
it announced, and counting up by one per chunk (the numbers wrap around after 2^32 - 1).

A data chunk's payload:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | sequence number |
| 4 | 2 | channel |
| 6 | 2 | zero |
| 8 | 1 | kind of content: 0 a message ([messages.md](messages.md)), 7 rumble, 9 controller information, 11 trigger effects |
| 9 | | the content |

The chunk flags are 1 for a message that fits one chunk; a larger message travels in several
([stream-setup.md](stream-setup.md#messages-too-large-for-one-packet)). **[read]** The channel is 1 for a
connection's opening messages, its heartbeats and its goodbye, 8 for the network probe's commands, 9 for the
answer to the stream information.

The receiver answers with a *data acknowledged* chunk (flags 0), whose payload is twelve bytes: the sequence
number up to which everything has arrived, its receive window (`0x19000`), the number of gap blocks and the
number of duplicates, both zero. **[read]** It sends the acknowledgment after it has acted on what arrived,
one for everything that came into order together.

On the stream connection every packet carries an authentication code once the keys exist
([stream-setup.md](stream-setup.md#the-stream-keys)); a packet with a wrong code changes nothing and is not
acknowledged.

- A receiver hands chunks on in the order of their numbers. One that arrives early is kept (up to sixteen
  ahead) until the ones before it are there.
- **[read]** A sender keeps up to sixteen unacknowledged chunks and sends each again every 200 ms, 25 times,
  then gives it up.
- *Chosen difference.* The reference does not answer a chunk it has already handed on; if its acknowledgment
  was lost, the sender repeats the chunk until it gives up. This client acknowledges such a chunk again.

**[seen]** A PS5 on system software 14.23 acknowledged data chunks built this way and sent its own, which
this client acknowledged, through the whole of the network probe (2026-11-19).

## Picture and sound packets on the probe connection

The probe connection uses the transport's older packet layout for picture (kind 2) and sound (kind 3)
packets; the stream's layouts differ and are described in [video.md](video.md).

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | kind in the low four bits; bit 4 set when three more header bytes follow at the end |
| 1 | 2 | packet index |
| 3 | 2 | frame index |
| 5 | 4 | unit index (11 bits), units in the frame minus one (11 bits), repair units (10 bits), from the top |
| 9 | 1 | codec |
| 10 | 4 | zero |
| 14 | 4 | key position; zero without encryption |
| 18 | 3 | picture packets only: a 16-bit word, then the adaptive stream index in the top three bits of a byte |

**[read]** A client may hand picture packets on in the order of their packet index and drop one whose index
it has already passed, so a sender counts that index up with every picture packet.

## What has been confirmed

**[seen]** A PS5 on system software 14.23 completed the handshake as described, announcing 100 streams each
way (2026-11-19). The layout and the handshake are tested the same two ways as the session start: the mock
console takes this client through the handshake, and the reference's own transport code completes the
handshake with the same mock (its log then says the transport is connected), with the mock recording what each
client's init carried so that the tests can compare them.

`src/proto/transport.h`
