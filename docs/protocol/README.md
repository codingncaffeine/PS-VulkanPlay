# Protocol notes

The Remote Play protocol has no public specification. These pages describe it as this project understands it, in
our own words, one page per part. The code under `src/proto/` is written from these pages, not the other way
around: a behavior is described here first, then implemented, then checked.

## How sure each statement is

Every statement about the protocol carries one of these marks.

| Mark | Meaning |
|---|---|
| **[checked]** | Our implementation gives the same result as the reference implementation on the same inputs, proven by a differential test (`tools/oracle/run.sh`). |
| **[seen]** | Observed in a session between our own client and our own console. |
| **[read]** | Read in the reference implementation's source and not yet confirmed by a test or an observation. |

A statement moves up as evidence arrives; nothing is implemented from a guess.

## The reference

The reference implementation is [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), the continuation of
[Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki). The differential tests pin it to one commit, named in
`tools/oracle/run.sh`; when the pin moves, the tests run again and the marks here are reviewed.

## Pages

- [Encodings](encodings.md): how values are written as text
- [Discovery and wake-up](discovery.md): finding consoles, their state, waking them from rest mode
- [Registration](registration.md): the PIN exchange that makes a console accept this client
- [Signing in](sign-in.md): how the account ID a registration needs is learned from a PSN login
- [Starting a session](session.md): the session request, the session cipher and the control channel
- [The stream transport](transport.md): the UDP packets, the opening handshake and the messages that must arrive
- [The stream's messages](messages.md): the field coding and the messages carried in the transport
- [The network probe](probe.md): measuring the round trip and the largest packet before the stream
- [Setting a stream up](stream-setup.md): the key exchange, the launch specification, the stream keys, packet
  authentication and the stream information
- [Picture and sound](video.md): the picture and sound packets, frames and their repair units, what the client
  reports back
- [The controller's state](input.md): the input state packet with the sticks and the motion sensors, and the
  input history packet with the buttons, the triggers and the touchpad
