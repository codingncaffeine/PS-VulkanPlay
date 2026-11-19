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
