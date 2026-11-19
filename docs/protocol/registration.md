# Registration

Before a console lets a client connect, the client registers with it once. The console's owner opens a screen
that shows an eight-digit PIN; the client proves it knows the PIN and names the PSN account it acts for; the
console answers with a registration key and a secret that every later session uses.

This page covers PS5 and PS4 system software 8.00 and later. Older PS4 software registers differently and is
not implemented.

## The exchange

1. **Is the console taking registrations?** The client sends a five-byte datagram to UDP port 9295: `SRC3` and
   a NUL byte for a PS5, `SRC2` and a NUL byte for a PS4. A console that is showing its PIN answers with a
   datagram that begins with `RES3` or `RES2`. **[checked]** (against the reference's client) **[read]** The
   reference waits three seconds for the answer and then pauses a tenth of a second; so do we.
2. **The request.** The client connects to TCP port 9295, sends a request head and a payload, and reads the
   answer on the same connection.

## The request

**[checked]** The head, byte for byte what the reference sends (lines end with carriage return and line feed):

```
POST /sie/ps5/rp/sess/rgst HTTP/1.1
 HTTP/1.1
HOST: 10.0.2.15
User-Agent: remoteplay Windows
Connection: close
Content-Length: <payload size>
RP-Version: 1.0

```

For a PS4 the path is `/sie/ps4/rp/sess/rgst` and the version `10.0`. The second line and the fixed host
address look like accidents, but they are what consoles are known to accept, so they stay until a real console
has shown that something tidier works too.

**[checked]** The payload is 480 bytes followed by encrypted text:

| Offset | Size | Content |
|---|---|---|
| 0x000 | 480 | filler, every byte `A` (0x41), except the two fields below |
| 0x0c7 | 8 | the wrapped nonce, bytes 8 to 15 |
| 0x191 | 8 | the wrapped nonce, bytes 0 to 7 |
| 0x1e0 | rest | the encrypted text |

Two filler bytes double as selectors into fixed tables:

- the low five bits of the byte at 0x18d choose the **key offset** (0 to 31);
- the high five bits of the byte at 0x000 choose the **wrap offset** (0 to 31).

With `A` as filler they are always 1 and 8. **[read]** A console presumably honors any pair; nobody here has
tried another, so we send what the reference sends.

The encrypted text, before encryption:

```
Client-Type: dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f
Np-AccountId: <account ID>
```

The account ID is the account's 64-bit number as eight bytes, least significant first, in
[base64](encodings.md).

## The cipher

`src/proto/rp_cipher.h`. Everything in this section is **[checked]**: 62,000 comparisons with the reference per
run, on random nonces, PINs, offsets, counters and texts.

- **Nonce.** Sixteen fresh random bytes chosen by the client.
- **Key.** Sixteen bytes: byte *i* is entry `i * 32 + key offset` of a fixed table of 512 bytes (one table per
  console family). The PIN, as a 32-bit number, most significant byte first, is then XORed over bytes 12 to 15.
- **Wrapped nonce.** What travels in the payload: byte *i* is `(nonce[i] XOR mask[i]) + bias + i`, modulo 256,
  where `mask[i]` is entry `i * 32 + wrap offset` of a second fixed table and the bias is 0x29 for a PS4 and
  0xd3 for a PS5. The console undoes it and so learns the nonce.
- **Starting vector** of message number *n*: the first sixteen bytes of HMAC-SHA256, keyed with a sixteen-byte
  constant of the console family, over the nonce followed by *n* as eight bytes, most significant first.
- **Encryption.** AES-128 in CFB mode (128-bit feedback) with that key and vector. Request and response are
  both message number 0.

The four tables and the two constants are fixed values that every client has to use; they are in
`src/proto/key_tables.inc`, generated from the reference by `tools/oracle/extract-key-tables.py`.

A client with the wrong PIN derives the wrong key, the console cannot read the text, and the registration is
refused.

## The answer

**[read]** (the status line and the body's fields are the reference's; our reading of them is tested against
answers we build ourselves, and the reference's client reads those too)

A refusal is a status other than 200, with the console's reason in hex:

```
HTTP/1.1 403 Forbidden
RP-Application-Reason: 80108b09
```

`80108b09` is the general "registration failed". An acceptance is status 200 with a `Content-Length` and a
body encrypted like the request text (message number 0). Decrypted, it is a block of `name: value` lines:

| Name | Value |
|---|---|
| `PS5-RegistKey` / `PS4-RegistKey` | the registration key, as hex of its characters (up to sixteen) |
| `RP-Key` | the shared secret, 32 hex digits |
| `RP-KeyType` | a number |
| `PS5-Mac` / `PS4-Mac` | the console's hardware address, 12 hex digits |
| `PS5-Nickname` / `PS4-Nickname` | the console's name |
| `AP-Ssid`, `AP-Bssid`, `AP-Key`, `AP-Name` | the console's own access point, for a direct connection |

**[seen]** A PS5 on system software 14.23 accepted a registration built exactly as above (2026-11-19). Its
answer's head was

```
HTTP/1.1 200 OK
Connection: close
RP-Feature: 3
Content-Length:        195
```

with the length padded by spaces to eleven characters after the colon, so a reader must skip the spaces
before that number. The decrypted body held, in this order, `AP-Bssid`, `AP-Name` (`PS5`), `PS5-Mac`,
`PS5-RegistKey` (eight characters), `PS5-Nickname`, `RP-KeyType` (`2`), `RP-Key` and `RP-SupportCmd` (six
digits that match the system software version); no `AP-Ssid` and no `AP-Key`. The PS4 answer is still
**[read]**.

The registration key, the secret and the hardware address must all be present; without any of them the answer
is not a registration. Text fields longer than the console's own limits are ignored.

## What the mock console assumes

The tests register against `tools/mock-console`, which plays the console's part as this page describes it:
it reads the two offsets from the payload, unwraps the nonce, derives the key from its PIN, and refuses when
the text does not open. Two things keep it honest: the reference's own client registers with it and reads back
the same keys, and that client's request is byte for byte the one ours sends. What it cannot show is how a real
console words its answers; that is confirmed the first time this client registers with one.

`src/proto/regist.h`
