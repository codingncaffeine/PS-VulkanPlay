# Starting a session

A registered client starts a session in two steps over TCP port 9295: a short request that yields a nonce, and
a second connection, the control channel, that stays open for the whole session. Both identify the client by
what [registration](registration.md) gave it: the registration key and the shared secret.

What follows the control channel (the network probes and the stream itself) has its own pages.

## The session request

**[read]** The client connects, sends this and reads the answer (lines end with carriage return and line
feed; the connection is closed afterwards):

```
GET /sie/ps5/rp/sess/init HTTP/1.1
Host: <console address>:9295
User-Agent: remoteplay Windows
Connection: close
Content-Length: 0
RP-Registkey: <registration key>
Rp-Version: 1.0

```

For a PS4 the path is `/sie/ps4/rp/sess/init` and the version `10.0`. The registration key is written as the
hex of its characters, without the padding.

An accepting console answers with status 200 and `RP-Nonce`, the [base64](encodings.md) of sixteen bytes. Any
other answer is a refusal, with the reason in `RP-Application-Reason` (hex):

| Reason | Meaning |
|---|---|
| `80108b10` | Remote Play is already in use on this console |
| `80108b15` | Remote Play has crashed on the console |
| `80108b11` | the console speaks another protocol version (it names its own in `RP-Version`) |
| `80108b02` | the PSN account is not valid for this console |
| `80108b09` | the registration is not valid |

## The session cipher

The same cipher as in registration (AES-128 in CFB mode, the same starting-vector rule), with a key and a
nonce of its own. **[checked]** Both are derived from the console's nonce *N* and the shared secret *S*
through two fixed tables of thirty-two rows of 112 bytes. *A* is the first sixteen bytes of row `N[0] >> 3` of
the first table, *B* the first sixteen bytes of row `N[7] >> 3` of the second. For *i* from 0 to 15, modulo
256:

| | PS5 | PS4 |
|---|---|---|
| cipher nonce[*i*] | `(N[i] - 0x2d - i) XOR A[i]` | `(N[i] + 0x36 + i) XOR A[i]` |
| key[*i*] | `((S[i] + 0x18 + i) XOR N[i]) XOR B[i]` | `(((B[i] XOR S[i]) + 0x21 + i) XOR N[i])` |

Each side numbers the things it encrypts, starting at 0, and the number is the message number of the
starting-vector rule. The client's and the console's numbers are independent.

## The control channel

**[read]** A new connection to port 9295:

```
GET /sie/ps5/rp/sess/ctrl HTTP/1.1
Host: <console address>:9295
User-Agent: remoteplay Windows
Connection: keep-alive
Content-Length: 0
RP-Auth: <encrypted, number 0>
RP-Version: 1.0
RP-Did: <encrypted, number 1>
RP-ControllerType: 3
RP-ClientType: 11
RP-OSType: <encrypted, number 2>
RP-ConPath: 1
RP-PadProcNo: 3
RP-StartBitrate: <encrypted, number 3>
RP-StreamingType: <encrypted, number 4>

```

A PS4 gets the path `/sie/ps4/rp/sess/ctrl`, version `10.0`, and neither `RP-PadProcNo` nor
`RP-StreamingType`. The encrypted values are base64 of the ciphertext of:

| Header | Plain value |
|---|---|
| `RP-Auth` | the registration key in a sixteen-byte field, padded with zero bytes |
| `RP-Did` | a 32-byte device ID: `00 18 00 00 00 07 00 40 00 80`, sixteen random bytes, six zero bytes |
| `RP-OSType` | `Win10.0.0` and a zero byte |
| `RP-StartBitrate` | four zero bytes |
| `RP-StreamingType` | a 32-bit number, least significant byte first: 1 for H.264, 2 for HEVC, 3 for HEVC with HDR |

The console answers with status 200 and `RP-Server-Type`: base64 of sixteen bytes encrypted with the
console's number 0. The first byte of the plain value tells the model: 0 a PS4, 1 a PS4 Pro, 2 a PS5.
`RP-Prohibit: 1` may be present. The reference retries the request once if no answer comes within five
seconds.

## Control messages

After the answer, both sides send messages on the same connection:

| Bytes | Content |
|---|---|
| 4 | payload size, most significant byte first |
| 2 | message type, most significant byte first |
| 2 | zero |
| payload size | the payload, encrypted with the session cipher |

**[read]** A message with a payload uses the sender's next number; a message without one uses none. The
client's numbers continue after those of the request headers (so its first message with a payload is number
5 on a PS5 and 4 on a PS4); the console's continue after `RP-Server-Type` (number 1). The reference never
accepts a message longer than 512 bytes in all.

| Type | Direction | Meaning | Plain payload |
|---|---|---|---|
| `0x33` | console | the session ID | one byte with the ID's length, then the ID: letters and digits |
| `0xfe` | console | heartbeat request | none |
| `0x1fe` | client | heartbeat reply | none |
| `0x04` | console | the console wants its login passcode | none |
| `0x8004` | client | the passcode | its digits as characters |
| `0x05` | console | login result | one byte: 0 accepted, 1 wrong passcode; sent also when no passcode was asked for |
| `0x50` | client | go to rest mode | none |
| `0x34` | console | switch to the stream connection | (described with the stream) |
| `0x36` | client | microphone muted or not | `00 01 01 59` muted, `00 01 00 59` not |
| `0x910` | client | display devices | four zero bytes |

Keyboard (`0x0d`, `0x20` to `0x25`), controller features (`0x13`, `0x11`), home (`0x14`), display (`0x01`,
`0x16`) and microphone connection (`0x30`) messages exist and are described where they are implemented.

**[read]** Once it has the session ID a client sends "microphone muted" twice and then the display devices
message. A client with a DualSense first sends `0x13` with `00 40 00` and a sixteen-byte `0x11`
(`a0 ab 51 bd d1 7e 00 00 ff ff ff ff ff 00 00 00`; what the bytes mean is not known). This client sends the
two, with the reference's bytes, when it announces a DualSense ([input.md](input.md#what-the-console-sends-for-the-controller)).

*Chosen difference.* This client's control connection sends each message at once (`TCP_NODELAY`). Left to the
system, a small message waits until the one before it has been acknowledged, which can hold a control message
back by tens of milliseconds.

**[seen]** A PS5 on system software 14.23 with no login passcode sent, unasked, a login result of 0, then a
session ID of sixteen letters behind a length byte of `0x10`, and a heartbeat request no more often than
every few seconds (2026-11-19). The reference takes the first byte for the letter `J` (which is the length
74) and refuses an ID shorter than 24 characters, so with this console it makes up an ID of its own: seconds
since its start, then 48 random bytes in base64. [stream-setup.md](stream-setup.md) says which of the two this
client sends.

The session ID names the session in the stream set-up that follows. A client answers every heartbeat request;
a console that stops getting answers ends the session.

## What has been confirmed

The cipher derivation is **[checked]** against the reference on random inputs. The request texts and the
message framing are **[read]**, and tested two ways: the mock console (`tools/mock-console`) decrypts and
checks what this client sends, and the reference implementation's own session code is run against the same
mock, which records what each of the two clients sent so that the tests can compare them. Nothing on this page
has been **[seen]** from a real console yet.

`src/proto/session.h`
