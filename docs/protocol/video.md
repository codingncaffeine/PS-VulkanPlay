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

With bit 4 of the first byte set, three more bytes follow the header; they are not read. **[seen]** A PS5
sets it on all packets of a frame or on none. In its HEVC streams those are the frames with more than one
source unit; in an H.264 stream they were three frames of 591. What the three bytes say is not known; with
them skipped every frame decodes.

**[seen]** The codec byte of a PS5's picture packets is 3 in an H.264 stream and 6 in a HEVC stream, with HDR
or without. The 16-bit word is 0 on repair units. On source units of a HEVC stream it is 510, the number of
64x64 blocks in a 1920x1080 picture; in a 720p H.264 stream it is 3600, the number of 16x16 blocks in the
picture, on a frame's only source unit, and smaller numbers on the units of a frame that has several. The
adaptive stream index is 0 with one picture size on offer.

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

**[seen]** A PS5 moves its key position on, for a picture or sound packet, by the payload's size rounded up
to a whole number of sixteen bytes, and sixteen more: in four sessions all but 140 of 9,744 steps from one
packet to the next were exactly that. Those were larger by 16, 32 or 48, where messages of the console's lay
between, and one by a whole packet, which never arrived. A client never needs this, because every packet names its position; the mock
console counts the same way.

## A picture frame

A frame travels as *source units*, which carry it, and *repair units*, from which lost units can be restored.
One packet carries one unit. The number of source units is the frame's units minus its repair units.

**[checked]** Every unit of a frame has one size, the *unit size*: the payload size of a repair unit. A
source unit's payload begins with a 16-bit count of padding; the payload is that much shorter than the unit
size. The frame is the source units in order, each without its first two bytes.

**[seen]** The frames of a PS5's streams, put together this way behind the picture size's header, are
streams a decoder takes without one error (`ffprobe`): 591 frames of 720p H.264, 1188 and 289 frames of 1080p
HEVC, 291 frames of 1080p HEVC in ten bits. Each stream begins with one keyframe of 25 to 49 source units;
the frames after it have one to fourteen. Every frame came with exactly one repair unit.

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

The factors are distinct only for up to 256 units, so this client takes a frame of more units as whole only
by its source units.

This client hands a frame on the moment it has enough units, and does not wait for the rest of them.

*Chosen differences.*
- A restored unit whose padding count is larger than the unit makes the frame *lost*. The reference leaves
  such a unit out and hands the damaged frame on.
- A frame of which too few units arrived is never handed on. The reference hands on what it has when the
  frame's last unit arrives.

### Order, waiting and giving up

Frame indices count up by one and wrap around at 65536. **[seen]** A stream's first frame is frame 1, and it
is a keyframe.

**[read]** The reference sorts picture packets by their packet index before it looks at them, waits up to
16 ms for a missing one, and works on one frame at a time.

*Chosen difference.* This client works on up to four frames at once and never holds a packet back:

- A packet goes to its frame at once, whatever order the packets come in.
- Frames are handed on in order. A whole frame waits behind an earlier one that is not whole yet.
- The oldest frame under way is given up 16 ms after the first packet of any later frame arrived, or 16 ms
  after its own last unit arrived, or 2 ms after a later frame became whole, whichever is first, if it is
  still not whole then.
- A packet of a frame four or more ahead of the oldest one gives the oldest ones up at once.
- A packet of a frame that was handed on or given up is counted and dropped.

So a packet that was overtaken costs nothing as long as it arrives within 16 ms and before the next frame
is whole (or within 2 ms of that). Loss that cannot be repaired holds the picture up by 2 ms once the next
frame is there, and by 16 ms at most. **[seen]** In 25 minutes of three PS5 sessions 426 frames were given up
and not one packet of a given-up frame arrived afterwards: on a home network the packets of a frame that
do not come before the next frame's were lost. Waiting the full 16 ms held the frame after each lost one
back by a frame's time, and the report to the console with it.

### Telling the console

Frames that were given up are reported in a message of type 5 ([messages.md](messages.md)) with the first
and the last frame's index; a message of type 25 asks the console for a frame that needs no earlier one (a
keyframe). **[checked]** Both messages are byte for byte the reference's. Both go out on channel 2, with
flags 1 (**[read]**; **[seen]** for the report, which a PS5 acted on).

**[seen]** After a report the console codes a frame that needs none of the reported ones. In a PS5's HEVC
stream every frame after the first lists the nine frames before it as kept and is predicted from the one
just before it. When frame 576 of a stream was lost whole (both of its packets), the report went out 16 ms
after frame 577 began. Frames 577 to 579 were still predicted from the frame before each, so they could not
be shown right. Frame 580, 67 ms after the lost one, was predicted from frame 575, the last one before the
loss, and the frames after it from the one before again: the picture was whole after four frames, without a
keyframe. A decoder has to keep going over the three frames in between. One that refuses every frame whose
list names a frame it does not have never comes back, since each later frame lists nine.

**[read]** The reference reports from the frame after the last one it handed on up to the frame before the one
that just began, and asks for a keyframe when restoring a frame fails. It also reads the beginning of every
frame to see which earlier frame it refers to, and drops a frame whose reference it does not have. This
client leaves that to its decoder: every frame that arrives is decoded, and a picture the decoder marks as
built on a frame it did not have is not shown; the last whole picture stays up until the console's repair.
**[seen]** On the stream above, FFmpeg 9 marked exactly the three frames between the loss and the repair.
Older versions of FFmpeg do not mark them; there such pictures are shown, for a few frames.

### Congestion reports

**[read]** Every 200 ms the client sends a packet of kind 5. **[checked]** It is fifteen bytes:

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

**[seen]** A PS5 sends a sound packet every 10 ms with one new frame and two repeated ones, each 80 bytes, the
first packet carrying frame 1. The frames are Opus (full band, 10 ms, stereo): wrapped in an Ogg container
they decode without an error (3,992 frames in four sessions). Without a DualSense announced it sends no
haptics packets.

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
- **[seen]** in four sessions with a PS5 on system software 14.23 (2026-11-19; 720p H.264, 1080p HEVC twice,
  1080p HEVC with HDR; forty seconds in all): 5,756 picture and 3,992 sound packets, every one with the right
  code; the pictures decode and the sound decodes, as marked above. No packet was lost that a frame needed.
- **[seen]** in a fifth session (2026-11-20, 1080p HEVC, twelve seconds, 2,951 picture and 1,196 sound
  packets): two packets in a row were lost, the one source unit and the one repair unit of a frame. The frame
  was given up after 16 ms and reported, and the console answered as described under "Telling the console".
  Restoring a lost unit of a real stream is still to be seen.

`src/proto/media_packet.h`, `src/proto/erasure.h`, `src/proto/frame_assembler.h`, `src/proto/sound.h`,
`src/proto/media_sender.h` (the mock console's side)
