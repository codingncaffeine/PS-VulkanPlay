# The network probe

Before the stream, a client opens a short connection to UDP port 9297 to learn three numbers: the round trip
time, and the largest packet that passes in each direction. Sizes on this page are those of whole IP packets;
a datagram is 28 bytes smaller (IPv4 and UDP headers).

The connection starts with the transport's opening handshake and uses its numbered messages
([transport.md](transport.md), [messages.md](messages.md)). Nothing on it is encrypted.

## The exchange

1. **Version.** The client sends a protocol version request listing version 9 (channel 1). The console
   answers with the version it chose.
2. **Opening.** The client sends an opening message with client version 9 and empty session key, launch
   specification and encrypted key (channel 1). The console sends its answer.
3. **Round trip.** The client sends *echo on* (channel 8) and waits for its acknowledgment. Then ten pings,
   one after the other: a sound packet (older layout, see the transport page) of 548 bytes with codec `0xff`,
   frame index 0, the ping's number as unit index and 2048 units in the frame; its content is four zero
   bytes, a random 32-bit tag, and zeros. The console sends each ping back as it came. The client waits up
   to one second for each; the round trip is the average over those that came back. Then *echo off*,
   acknowledged.
4. **Largest packet towards the client.** The client asks for a packet of a given size with a size request
   (channel 8; a new id each time, counting from 1), and the console sends one picture packet of that size
   whose frame index is the request's id. The client searches between 576 and 1454, starting with 1454: a
   size that arrives becomes the lower bound, one that does not arrive in three tries becomes the upper
   bound, the next size is halfway, until the bounds are one apart. The answer is the lower bound.
5. **Largest packet towards the console.** The client sends a client size test, on, with id 1 and the size
   from step 4 as both of its sizes. The console sends the same message back. Then the same search, starting
   at the size from step 4, with pings padded to the size under test; the console sends back those that
   reach it. At the end the client sends a client size test, off, with id 2, the search's upper bound as its
   size and the size from step 4 as the inbound size, and waits for its acknowledgment.
6. **Goodbye** (channel 1), and the client closes the connection without waiting.

The wait for an answer in the two searches is five times the round trip, at least 20 ms and at most 500 ms.
Every other answer is waited for five seconds. If the probe fails, a stream starts with a round trip of 1 ms
and 1454 for both sizes.

## Chosen differences from the reference

- The reference waits at least 5 ms in the searches; this client at least 20 ms. A wait that is too short
  reports a smaller packet size than the path allows, and costs nothing when every answer arrives.
- The reference's search towards the console starts below the ceiling whenever step 4 found less than 1454,
  and then never tries 1454 itself: it reports 1453 on a path that passes 1454. This client tries the
  ceiling once at the end of such a search.
- The reference accepts only the answer to its latest try of a size. This client also accepts a late answer
  to an earlier try of the same size.
- The padding of the size pings is the six letters the reference sends, repeated. A console is known to
  answer those; nothing else has been tried.

## What has been confirmed

- **[checked]** The reference's own probe completes against the mock console, measures the limits the mock
  was given (1300 in, 1203 out), and each of its messages is byte for byte the one this client sends.
- **[seen]** A PS5 on system software 14.23, on a wired home network, took this client through all six
  steps: ten of ten pings answered, a round trip of 3.2 to 3.7 ms, 1454 in both directions (2026-11-19).
  A path with a smaller limit has not been seen with a console.
