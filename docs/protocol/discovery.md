# Discovery and wake-up

A client finds consoles, learns whether they are on, and wakes them from rest mode with small text datagrams
over UDP. No registration is needed to find a console; waking one needs the key from a registration with it.

## Ports and versions

| | PS4 | PS5 |
|---|---|---|
| The console listens on UDP port | 987 | 9302 |
| Version text in every datagram | `00020020` | `00030010` |

- **[checked]** The client sends from a local UDP port in the range 9303 to 9319 when one is free. **[read]**
  Whether a console cares about the source port is not known; we use the range as other clients do.
- A request goes to one console's address, or to a broadcast address to reach every console on the network.

## Asking consoles to identify themselves

**[checked]** The request is this text, followed by one NUL byte:

```
SRCH * HTTP/1.1
device-discovery-protocol-version:<version>
```

Lines end with a line feed only. A console answers a request that carries its own family's version.

## The answer

**[checked]** (as far as reading it goes) The answer is a status line and `name:value` lines:

```
HTTP/1.1 200 Ok
host-id:<twelve hex digits>
host-type:PS5
host-name:<the name its owner gave it>
host-request-port:997
device-discovery-protocol-version:00030010
system-version:<eight digits>
running-app-titleid:<...>
running-app-name:<...>
```

| Status | Meaning |
|---|---|
| 200 | the console is on |
| 620 | the console is in rest mode |
| anything else | unknown; shown as such |

- The family is told from the version line: `00030010` is a PS5, anything else a PS4.
- `host-id` is the console's hardware address in uppercase hex; it is how a discovered console is matched to a
  registration.
- `system-version` tells whether a PS4 can be talked to at all: this client implements the protocol of system
  software 8.00 and later (`08000000` and up) and says so for anything older.
- The two `running-app` lines are present only while something runs.
- **[seen: not yet]** No answer from a real console has been recorded by this project yet; the list of fields
  is the reference's.

### How the text is read

`src/proto/text_message.h` reads this text here and everywhere else in the protocol.

- **[checked]** Lines end with a line feed, with or without a carriage return before it. Reading stops at a NUL
  byte. One space after the colon is not part of the value. Names are case-sensitive; if a name occurs twice the
  first occurrence counts.
- **[checked]** A last line without a line ending is not read: a datagram that was cut off must not yield half a
  value. (The reference behaves the same way; the differential test showed it.)
- A line with no colon, or with nothing before the colon, makes the whole text invalid.

Where we differ from the reference, on purpose:

| Text | Reference | Ours |
|---|---|---|
| a header with an empty value (`running-app-name:`) | refuses the whole answer | keeps it, with an empty value |
| a status line with nothing after it | refuses | reads the status |
| `host-request-port` that is not a number from 0 to 65535 | keeps the low sixteen bits of whatever number it starts with | no port |

A console that sent an empty value would be invisible to the reference; nothing is gained by refusing it.

## Waking a console

**[checked]** The wake-up is this text, followed by one NUL byte, sent to the same port as a search:

```
WAKEUP * HTTP/1.1
client-type:vr
auth-type:R
model:w
app-type:r
user-credential:<number>
device-discovery-protocol-version:<version>
```

- **[checked]** The credential is the registration key (up to sixteen hex digits, handed out by the console
  when the client registered, see [Registration](registration.md)) read as a hexadecimal number and written
  in decimal.
- **[read]** A console wakes only for a credential of a client registered with it, and only if its rest mode
  keeps it connected to the network.
- A console takes a while to come up. This client repeats the wake-up once a second and asks for the state in
  between, until the console answers with status 200 or the wait runs out.

`src/proto/discovery.h`
