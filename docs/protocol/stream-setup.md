# Setting a stream up

After the control channel has named the session and the network probe has measured the path
([probe.md](probe.md)), the client opens a second connection of the stream transport ([transport.md](transport.md)),
on UDP port 9296. On it the two sides exchange keys, the console says what the stream will carry, and from then
on every packet is authenticated.

**[read]** The connection speaks version 12 of the stream protocol to a PS5 and version 9 to a PS4. Its opening
handshake is the one of the probe connection. **[seen]** A PS5 on system software 14.23 answers with version 12.

## The order of things

1. The client sends its **opening message**: the launch specification, its public key and a signature.
2. The console sends its **answer**: whether it accepts, its own public key and signature.
3. Both sides make the **stream keys**. Everything sent after this carries an authentication code.
4. The console sends the **stream information**: picture sizes with their headers, the sound format.
5. The client answers with three messages: "stream information received", the controller it has, and the
   sound format of its microphone.
6. The client sends a **heartbeat** every second. Either side ends the stream with a **goodbye**.

**[read]** The client waits five seconds for the answer and five more for the stream information. The stream
information may be handed over before the client is done with the answer; it is then kept and used after it.

## The handshake key and the key exchange

The client makes sixteen random bytes, the *handshake key*, and a key pair on the curve secp256k1.

- **[checked]** A public key travels as the 65 bytes of its uncompressed point (`04`, X, Y).
- **[checked]** Its signature is HMAC-SHA256 of those 65 bytes under the handshake key: 32 bytes.
- **[checked]** The shared secret is the 32-byte X coordinate of the product of one side's private key and the
  other's public point.

The handshake key reaches the console inside the launch specification, which only the console can open. A
signature therefore shows that a public key comes from the other side of this session.

**[seen]** A PS5 signs its public key the same way: the signature in its answer is the one this rule gives.

*Chosen difference.* The reference takes the console's signature and never looks at it. This client computes
what the signature must be and reports whether it matches. A PS5 whose signature does not match gets no
stream. For a PS4 a mismatch is only reported: what a PS4 signs with has not been seen, and a wrong guess
there would refuse every stream.

## The launch specification

One line of JSON. **[checked]** Its text is, with the variable parts in braces:

```
{"sessionId":"sessionId4321","streamResolutions":[{"resolution":{"width":{W},"height":{H}},"maxFps":{FPS},
"score":10}],"network":{"bwKbpsSent":{KBPS},"bwLoss":0.001000,"mtu":{MTU},"rtt":{RTT},"ports":[53,2053]},
"slotId":1,"appSpecification":{"minFps":30,"minBandwidth":0,"extTitleId":"ps3","version":1,"timeLimit":1,
"startTimeout":100,"afkTimeout":100,"afkTimeoutDisconnect":100},"konan":{"ps3AccessToken":"accessToken",
"ps3RefreshToken":"refreshToken"},"requestGameSpecification":{"model":"bravia_tv","platform":"android",
"audioChannels":"5.1","language":"sp","acceptButton":"X","connectedControllers":["xinput","ds3","ds4"],
"yuvCoefficient":"bt601","videoEncoderProfile":"hw4.1","audioEncoderProfile":"audio1"{PS5-A}},
"userProfile":{"onlineId":"psnId","npId":"npId","region":"US","languagesUsed":["en","jp"]},{PS5-B}
"handshakeKey":"{KEY}"}
```

(shown here on several lines; it is sent as one, without the line breaks).

| Part | Content |
|---|---|
| W, H, FPS | the picture size and the highest frame rate asked for |
| KBPS | the bitrate asked for, in kbit/s |
| MTU | the largest packet the console may send: the inbound size the probe measured |
| RTT | the round trip the probe measured, in whole milliseconds |
| KEY | the handshake key in base64 |
| PS5-A | for a PS5: `,"adaptiveStreamMode": "resize"` (with that space); for a PS4 nothing |
| PS5-B | for a PS5: `"videoCodec":"avc",` or `"videoCodec":"hevc",` and then `"dynamicRange":"SDR",` or `"dynamicRange":"HDR",`; for a PS4 nothing |

Everything else is fixed. The fixed values describe no real client; they are the ones other clients send and
consoles accept, and none has been tried with another value.

**[checked]** The text travels sealed: the text and one closing zero byte, combined by exclusive or with the
output of the session cipher ([session.md](session.md)) run over as many zero bytes as message number 0, then
written in base64.

## The opening message and its answer

Their fields are listed in [messages.md](messages.md). **[checked]** Both are coded as described there.

The client's opening message carries: the stream protocol version; as *session key* the session ID (below); the
sealed launch specification; an *encrypted key* of four zero bytes; its public key and signature.

**[read]** The console's answer says whether it accepted the version and the encrypted key, and carries the
console's public key and signature. A client gives up when either is not accepted or the key is missing.
**[seen]** A PS5 accepted an opening message built this way, asking for 1920x1080 at 60 in HEVC, and repeated
the opening message's session key in its answer.

### Which session ID

The console names the session on the control channel. **[seen]** A PS5 on system software 14.23 sends a length
byte and sixteen letters ([session.md](session.md)).

**[read]** The reference accepts only IDs of 24 characters or more and otherwise invents one: the seconds of
its clock in decimal followed by 48 random bytes in base64, about 70 characters. Against such a PS5 it
therefore always sends an invented ID, and streams.

**[seen]** A PS5 accepts an invented ID of that form, and it accepts its own.

*Chosen difference.* This client sends the console's own ID, which lets the opening message go out in one
packet. `--session-id invented` sends one of the reference's form instead.

## The stream keys

Each direction has its own keys, made from the shared secret and the handshake key. The direction is a number:
**2** for what the client sends, **3** for what the console sends.

**[checked]** HMAC-SHA256 under the shared secret of the 21 bytes `01`, the direction, `00`, the handshake key,
`01`, `00` gives 32 bytes: the first sixteen are the direction's **key**, the last sixteen its **starting
vector**.

In what follows the starting vector is taken as a number with its least significant byte first, to which a
count is added.

- **[checked] Key stream.** Block number *n* of the key stream is AES-128 under the key of (starting vector +
  *n*). Byte *p* of the key stream, the *key position* *p*, is byte *p* mod 16 of block *p* / 16. Data is
  encrypted and decrypted by exclusive or with it.
- **[checked] Authentication key.** The first one is SHA-256 of the key followed by the starting vector, its
  two halves combined by exclusive or. It serves key positions up to 45000. For the positions of span number
  *i* = (position - 1) / 45000, from 1 on, the key is SHA-256 of the first authentication key followed by
  (starting vector + *i* x 44910), folded the same way.
- **[checked] Authentication code.** AES-128-GCM under the authentication key, with the sixteen bytes of
  (starting vector + position / 16) as its starting vector, nothing encrypted, the packet as the authenticated
  data; the first four bytes of the tag are the code.

### Where a packet carries its code

The code is computed over the whole packet with the code's own four bytes as zero. Every sealed packet also
carries the low 32 bits of its key position.

| Kind of packet | Code at | Key position at | Position's bytes in the code |
|---|---|---|---|
| 0 control | 5 | 9 | as zero |
| 2 picture, 3 sound | 10 | 14 | as they are |
| 5 congestion report | 7 | 11 | as zero |
| 1 input history, 6 input state | 8 | 4 | as they are |

**[checked]** for kinds 0, 2, 3 and 5 against the reference's own function. **[checked]** for kind 6 by the
reference's own client: every input packet it sent in a session carried the code this layout gives.

A consequence: where the position's bytes count as zero, the code depends on the position only through its
sixteen-byte block, so a change within the position's low four bits goes unnoticed.

### Key positions

A sender starts at position 0 when it gets its keys. Each packet it seals takes the current position, and the
position then moves on by the packet's *counted size* plus that size's remainder by sixteen.

| Packet | Counted size |
|---|---|
| a message in a data chunk | the bytes of the message (of the chunk's part of it) |
| a data acknowledgment | the whole packet, 29 bytes |
| a congestion report | 15 |
| an input packet | what follows its twelve header bytes, and sixteen more |

**[checked]** In a session of the reference's own client every one of its sealed packets (acknowledgments,
messages, heartbeats, congestion reports and input packets) sat where this rule puts it.

A packet sent again is sent as it was, with its first position and code.

**[checked]** A receiver follows the 32 bits through their wrap: a position that is ahead of the last accepted
one by less than 2^31 and numerically smaller has passed the top; one that is behind by less than 2^31 and
numerically larger is a late packet from before it. A position counts as accepted once its packet proved
authentic.

Before a side has its keys it sends position 0 and a code of four zero bytes. So the opening message and the
console's acknowledgment of it are unsealed. **[seen]** A PS5 does not seal its answer either: it carries a
code of four zero bytes. Everything a PS5 sent after it carried the right code (some 1400 packets in one
session, none refused).

**[checked]** The client's acknowledgment of the answer is already sealed, at position 0: the keys exist by the
time it is written (the reference's own client sends it so).

Packets that arrived before the keys and still wait for their turn are checked when the keys arrive, and
dropped if their code is wrong. This client hands over one message at a time until it has keys, so that the
answer can be acted on before the stream information behind it is looked at.

## The stream information

Sent by the console once the keys exist ([messages.md](messages.md) lists its fields): the picture sizes it
can send, each with the header its decoder needs first, and the sound format.

**[read]** The sound format is fourteen bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | channels |
| 1 | 1 | bits per sample |
| 2 | 4 | sample rate |
| 6 | 4 | samples per frame |
| 10 | 4 | 1 |

A client gives up when the sound format is not fourteen bytes long. **[read]** It keeps at most eight picture
sizes.

**[seen]** A PS5 names one picture size, the one asked for, with the parameter sets a decoder needs first, each
behind a start code: for 1280x720 in H.264 a header of 40 bytes (Main profile), for 1920x1080 in HEVC 92 bytes
(Main), for the same with HDR asked for 103 bytes (Main 10, ten bits). Its sound format each time: 2 channels,
16 bits, 48000 Hz, 480 samples per frame.

**[checked]** The client then sends, in this order and each as one data chunk with flags 1:

| Message | Channel | Bytes |
|---|---|---|
| stream information received (type 14) | 9 | `08 0e` |
| controller connection (type 21): connected, type 2 (DualShock 4) or 6 (DualSense) | 1 | `08 15 b2 01 04 10 01 18 02` (or `.. 06`) |
| stream information (type 13) with only the sound format of its microphone: 1 channel, 16 bits, 48000 Hz, 480 samples | 1 | `08 0d 7a 10 12 0e 01 10 00 00 bb 80 00 00 01 e0 00 00 00 01` |

and then acknowledges the console's stream information.

## Heartbeat and goodbye

**[read]** Once the stream is set up the client sends a heartbeat (type 3, `08 03`, channel 1) every second.
To end the stream it sends a goodbye with the reason `Client Disconnecting` on channel 1. A console that ends
it sends a goodbye with its own reason (`Server shutting down` when it is switched off).

**[seen]** A PS5 sends, all on channel 0: heartbeats of its own, about three a second; at the start one data
chunk of kind 9 (information for the controller) and one "stream information received" (type 14); and its view
of the connection (type 16, [messages.md](messages.md)) every 1.2 seconds or so. It acknowledged the goodbye.
Streams held for up to twenty seconds with a resting controller's input states were not closed by it.

**[seen]** After a goodbye a PS5 answers a new session request with "already in use" for some five to ten
seconds. Once, a PS5 that discovery had just shown as ready refused the connection to its session port and
then answered "already in use" for thirty seconds; what state it was in is not known.

## Messages too large for one packet

**[read]** A message that does not fit a datagram of (the smaller of the path's two largest packets) - 50 bytes
is sent in several data chunks:

- the first chunk as always, with the nine-byte header that ends in the kind of content, and flags 0;
- each further chunk with an eight-byte header (sequence number, channel, two zero bytes) directly followed
  by the next part of the message, and flags 0;
- the last chunk with flags 1.

So flag 1 says "the message ends here", and a message in one chunk has it. Each chunk has its own sequence
number and is acknowledged like any other.

With a session ID of about 70 characters the opening message is some 1430 bytes and never fits one datagram
of 1404; with the console's own sixteen letters it is 1377 bytes and does. **[seen]** A PS5 took an opening
message that came in two chunks.

*Chosen difference.* The reference splits what it sends and never joins what it receives. This client joins:
chunks without flag 1 are collected, in order, until the one with it. A message that grows past 64 KiB is
dropped.

## What has been confirmed

- **[checked]** by differential test against the reference (`tools/oracle/run.sh`, 91,606 comparisons): the key
  exchange in both directions between the two implementations, the stream keys, the key stream, the
  authentication keys and codes at positions around every boundary, whole-packet codes, the following of key
  positions, the launch specification and its sealed form, and the coding of every message named here.
- **[checked]** by the reference's own client against the mock console (`tests/scenario/phase2c.sh`): it takes
  the mock's answer, makes its keys, accepts the sealed stream information and reports the session as
  connected; the mock finds the right code on every packet it sends and each at the position the rule gives.
- **[checked]** by the mock console on the reference's own client (`tests/scenario/phase2d.sh`): the payload
  of an input packet is encrypted from sixteen bytes behind its key position on ([input.md](input.md)).
- **[seen]** in four sessions with a PS5 on system software 14.23 (2026-11-19): the set-up from the opening
  message to the goodbye, as marked above, with the console's own session ID and with an invented one, for
  720p H.264, 1080p HEVC and 1080p HEVC with HDR. Still to be seen: a PS4.

`src/proto/key_exchange.h`, `src/proto/stream_cipher.h`, `src/proto/launch_spec.h`, `src/proto/stream_setup.h`
