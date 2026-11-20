# The stream's messages

The content of a data chunk of kind 0 ([transport.md](transport.md)) is one message in the field coding that
is widely known as the "protocol buffers" wire format. This page describes the coding as far as the protocol
uses it, and the messages this client implements so far.

## The field coding

A message is a sequence of fields. Each field starts with a *key*: the field's number shifted left by three
bits, plus the kind of value that follows.

| Kind | Value |
|---|---|
| 0 | a number in 7-bit groups, least significant group first, the top bit set on every byte but the last (at most ten bytes) |
| 1 | eight bytes, least significant first |
| 2 | a length (coded as kind 0), then that many bytes: text, raw bytes or a message inside the message |
| 5 | four bytes, least significant first |

The key itself is coded as a number of kind 0. A true or false value is the number 1 or 0. A reader skips
fields whose number it does not know. Kinds 3 and 4 are not used; this client refuses a message that has
them, that is cut short, or whose lengths point past its end.

A writer emits fields in the order of their numbers; a field that a message requires is written even when it
is zero or empty, an optional one only when it is set. Messages written this way are byte for byte those of
the reference **[checked]** (its probe against the mock console: every message equal to ours).

## The outer message

| Field | Kind | Content |
|---|---|---|
| 1 | number | the type (required) |
| 2 to 32 | message | the body that belongs to the type |

| Type | Name | Body field |
|---|---|---|
| 0 | opening message ("big") | 2 |
| 1 | its answer ("bang") | 3 |
| 3 | heartbeat | none |
| 5 | frames that did not arrive | 6 |
| 8 | goodbye | 10 |
| 12 | network probe | 14 |
| 13 | stream information | 15 |
| 14 | stream information received | none |
| 16 | the console's view of the connection | 17 |
| 21 | controller connection | 22 |
| 25 | keyframe request | none |
| 31 | protocol version request | 31 |
| 32 | its answer | 32 |

Thirty-three types exist (loss and corruption reports, connection quality, microphone connection and more);
each is described where it is implemented. A message of a type without a
body here is read as its type alone.

## Bodies

**Opening message** (client): 1 client version (number, required), 2 session key (text, required), 3 launch
specification (text, required), 4 encrypted key (bytes, required), 5 key-exchange public key (bytes), 6
key-exchange signature (bytes).

**Its answer** (console): 1 server version, 2 token, 3 encrypted key accepted, 4 version accepted, 5 session
key (all required), 8 key-exchange public key, 9 key-exchange signature.

**Frames that did not arrive** (client): 1 the first frame's index, 2 the last one's (numbers, both required).
See [video.md](video.md).

**Goodbye**: 1 reason (text, required). A client sends `Client Disconnecting`.

**Stream information**: 1 a picture size, once per size: a message of 1 width, 2 height, 3 the decoder's
header (bytes; all three required); 2 the sound format (bytes, required); 3 start timeout, 4 idle timeout, 5
idle timeout before disconnecting, 6 congestion control interval (numbers); 7 a sound channel, once per
channel: a message of 1 its type (a number), 2 its format in the form of the sound format (bytes), 3 whether
its frames are samples as they are (a number, 0 or 1; optional). **[read]** The reference does not read
field 7. This client keeps the list and prints it. **[seen]** A PS5 that was told of one DualSense listed
five channels: type 0 with 2 channels, 16 bits, 48000 Hz, frames of 480, not marked as samples (the sound),
and types 2, 3, 4 and 5, each with 2 channels, 16 bits, 3000 Hz, frames of 30, samples as they are (the form
of a DualSense's haptics). No type 1 was listed, and no channel of any other form: nothing in the list could
carry what a game plays on a controller's speaker. In a session of over an hour, packets came for two of the
five only: the stream byte of every sound packet ([video.md](video.md)) was 0 or 2, which fits that byte
being a channel's type. What types 3 to 5 are for has not been seen; haptics for a second, third and fourth
controller would fit their form.

**The console's view of the connection** (console): 1 the picture's target bitrate in bits per second, 2 an
upstream bitrate (numbers); 3 an upstream loss (four bytes, a floating-point number); 4 upstream sound off
(yes or no); 5 a round-trip value (eight bytes, a floating-point number); 6 a loss count (number).
**[seen]** A PS5 sends one every 1.2 seconds or so, with all six fields and two more (7 and 8) that are not
read. Its target bitrate was 97% of the bitrate the launch specification asked for (9,708,000 for 10,000
kbit/s; 14,563,000 for 15,000). Its round-trip value lay near 200 or near 400 on a path measured at 2 to 5 ms; what
it counts is not known.

**Controller connection**: 1 controller number, 2 connected or not, 3 controller type (1 DualShock 3, 2
DualShock 4, 3 Vita, 4 XInput, 5 mobile, 6 DualSense, 7 VR2 Sense).

**Protocol version request** (client): 1 a supported version, once per version. **Its answer**: 1 the
version chosen.

**Network probe**: 1 command (number, required) and the body of that command:

| Command | Body field | Body |
|---|---|---|
| 0 echo | 2 | 1 on or off (required) |
| 1 size request | 3 | 1 id, 2 size asked for (both required), 3 size sent, 4 count, 5 send delay, 6 delta |
| 4 client size test | 5 | 1 id, 2 size, 3 on or off (all required), 4 inbound size |

## What has been confirmed

**[seen]** A PS5 on system software 14.23 answered the version request, the opening message, the echo
command, the size requests and the client size test built as above, with messages this client read
(2026-11-19). **[checked]** The opening message with its key fields, the three messages a client sends after
the stream information, the heartbeat, the keyframe request, the report of frames that did not arrive, the
console's view of the connection and the goodbye are byte for byte what the reference's coder writes; the answer and the stream information as this project writes them are read by the
reference's coder to the same values.
