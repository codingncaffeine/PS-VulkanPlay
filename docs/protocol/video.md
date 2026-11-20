# Picture and sound

Once a stream is set up ([stream-setup.md](stream-setup.md)) the console sends the picture and the sound in
packets of their own kinds (2 and 3, see [transport.md](transport.md)). They may be lost: nothing sends them
again. Each carries an authentication code and its key position, and its payload is encrypted.

The layouts here are the ones of stream protocol versions 9 (PS4) and 12 (PS5).

## The header

All numbers have their most significant byte first.

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | the kind in the low four bits (2 picture, 3 sound); bit 4: three more header bytes come before the payload |
| 1 | 2 | packet index |
| 3 | 2 | frame index |
| 5 | 4 | the units word (below) |
| 9 | 1 | codec |
| 10 | 4 | authentication code |
| 14 | 4 | key position, its low 32 bits |

A **picture** packet goes on with three bytes: a 16-bit word that is not understood, and a byte whose top three
bits are the *adaptive stream index*: which of the stream information's picture sizes the frame is in. Its
header is 21 bytes.

A **sound** packet goes on with one byte that is not understood: 19 bytes. A PS5's has one byte more, 20 in
all; the value 2 in it marks a packet for the controller's **haptics** instead of the sound.

With bit 4 of the first byte set, three more bytes follow the header; they are not read.

A packet is its header and at least one byte of payload; shorter ones are dropped.

*Chosen differences.*
- **[read]** The reference drops every picture packet shorter than 25 bytes. A frame's last unit can be
  shorter than that (a padding count and one or two bytes of the frame make a packet of 24 or 25); this client
  takes it.
- **[read]** The reference takes a sound packet that announces the three extra bytes and is too short to hold
  them, and goes on with a payload that begins behind the packet's end. This client drops it.

**[checked]** Apart from these two, both accept and refuse the same packets.

### The units word

| Kind | Bits, from the top |
|---|---|
| picture | 11: unit index; 11: units in the frame, minus one; 10: how many of them are repair units |
| sound | 8: unit index; 8: frames in the packet, minus one; 8: bytes per frame; 4: repeated frames; 4: new frames |

**[checked]** for both, with every other header field, against the reference's reader.

### The payload

**[checked]** The payload is encrypted with the console's key stream, beginning at the packet's key position
plus 16. (The reference's own client decrypts what the mock console encrypts this way, and finds the frames.)

**[read]** How far a console moves its key position on for a picture or sound packet is not known: a client
never needs it, because every packet names its position. The mock console counts the payload and sixteen bytes
more, the way an input packet is counted.

## A picture frame

A frame travels as *source units*, which carry it, and *repair units*, from which lost units can be restored.
One packet carries one unit. The number of source units is the frame's units minus its repair units.

**[checked]** Every unit of a frame has one size, the *unit size*: the payload size of a repair unit. A
source unit's payload begins with a 16-bit count of padding; the payload is that much shorter than the unit
size. The frame is the source units in order, each without its first two bytes.

So a frame's first packet tells the unit size, whichever unit it is: a repair unit by its payload size, a
source unit by its payload size plus its padding count.

### Restoring lost units

**[checked]** The repair units are sums of the source units, byte by byte, in the field of 256 elements with
the polynomial x^8 + x^4 + x^3 + x^2 + 1. With *m* repair units, source unit *j* enters repair unit *i* with
the factor 1 / (*i* xor (*m* + *j*)). A source unit takes part with its whole unit size: its padding count,
its bytes, and zeros up to the unit size.

Any *k* of a frame's *k* + *m* units give the frame: the source units that arrived are taken out of as many
repair units as source units are missing, and what remains is solved for the missing ones. A restored unit
carries its padding count again, which tells how many of its bytes belong to the frame.

**[checked]** The factors are distinct only for up to 256 units. A frame of more units is whole only by its
source units.

This client hands a frame on the moment it has enough units, and does not wait for the rest of them.

*Chosen differences.*
- A restored unit whose padding count is larger than the unit makes the frame *lost*. The reference leaves
  such a unit out and hands the damaged frame on.
- A frame of which too few units arrived is never handed on. The reference hands on what it has when the
  frame's last unit arrives.

### Order, waiting and giving up

Frame indices count up by one and wrap around at 65536. **[read]** A stream's first frame is frame 1.

**[read]** The reference sorts picture packets by their packet index before it looks at them, waits up to
16 ms for a missing one, and works on one frame at a time.

*Chosen difference.* This client works on up to four frames at once and never holds a packet back:

- A packet goes to its frame at once, whatever order the packets come in.
- Frames are handed on in order. A whole frame waits behind an earlier one that is not whole yet.
- The oldest frame under way is given up 16 ms after the first packet of any later frame arrived, or 16 ms
  after its own last unit arrived, whichever is first, if it is still not whole then.
- A packet of a frame four or more ahead of the oldest one gives the oldest ones up at once.
- A packet of a frame that was handed on or given up is counted and dropped.

So a packet that was overtaken costs nothing as long as it arrives within 16 ms, and loss that cannot be
repaired holds the picture up by 16 ms at most.

### Telling the console

**[checked]** Frames that were given up are reported in a message of type 5 ([messages.md](messages.md))
with the first and the last frame's index, on channel 2, with flags 1. **[read]** The console then codes the
next frames so that they need none of the reported ones.

**[checked]** A message of type 25, on channel 2, asks the console for a frame that needs no earlier one (a
keyframe).

**[read]** The reference reports from the frame after the last one it handed on up to the frame before the one
that just began, and asks for a keyframe when restoring a frame fails. It also reads the beginning of every
frame to see which earlier frame it refers to, and drops a frame whose reference it does not have. This
client does not look into frames yet; that comes with the decoder.

### Congestion reports

**[checked]** Every 200 ms the client sends a packet of kind 5, fifteen bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | 5 |
| 1 | 2 | 0 |
| 3 | 2 | picture and sound packets that arrived since the last report |
| 5 | 2 | packets that did not |
| 7 | 4 | authentication code |
| 11 | 4 | key position |

It counts fifteen bytes of key position ([stream-setup.md](stream-setup.md)).

This client counts a frame's units once the frame is four frames back, so that units which were merely late
are not reported as lost; for the sound it counts the frames that never came, not even as a repeat.

## Sound

**[read]** Codec 5 is Opus; the stream information names channels, sample rate and samples per frame. Packets
of another codec are dropped.

A sound packet carries whole frames, all of one size: first its *new* frames, whose indices are the packet's
frame index and the ones after it, then its *repeated* frames, the ones just before the frame index, oldest
first. With one new and two repeated frames, the packet of frame 7 carries frames 7, 5 and 6.

**[read]** A packet is dropped when new and repeated frames do not add up to its frame count, or its payload
is not that many frames of the size it names. At the start of a stream a repeated frame may name an index
before 1; it is not a frame.

Haptics packets are a second stream of the same form, with frame indices of their own.

*Chosen difference.* This client hands every frame on once and in order, taking a frame from a later packet's
repeats when its own packet was lost; it adds no waiting. The reference at the pinned commit collects frames
in a small buffer first.

## What has been confirmed

- **[checked]** by differential test against the reference (`tools/oracle/run.sh`): the header of both kinds
  for both families, on 30,000 random packets, read by us and by the reference and written by us and read
  back by the reference; the repair code in both directions, on 4,000 frames; frames put together from units
  with loss, on 3,000 frames, against the reference's frame processor; the congestion report and the two
  messages, byte for byte.
- **[checked]** by the reference's own client against the mock console (`tests/scenario/phase2d.sh`): it takes
  the mock's picture and sound packets, with and without lost and overtaken packets, and hands to its decoder
  exactly the bytes this client keeps with `session --dump`.
- Nothing on this page has been seen on a real console yet.

`src/proto/media_packet.h`, `src/proto/erasure.h`, `src/proto/frame_assembler.h`, `src/proto/sound.h`,
`src/proto/media_sender.h` (the mock console's side)
