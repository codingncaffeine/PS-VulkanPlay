# Encodings

## Base64

Binary values that travel as text (the account ID, and several values in the session headers) are written in
base64 with the standard alphabet (`A`-`Z`, `a`-`z`, `0`-`9`, `+`, `/`) and `=` padding, as in RFC 4648
section 4. Text is never wrapped into lines.

- **[checked]** Encoding: for any bytes, our text is identical to the reference's, character for character.
- **[checked]** Decoding: text produced by either side decodes to the same bytes on the other.
- **[checked]** Wherever our decoder accepts a text, the reference reads the same bytes from it.

Our decoder is deliberately stricter than the reference. It refuses text that an encoder cannot have produced;
the reference accepts some of it:

| Text | Reference | Ours |
|---|---|---|
| `=` before the end (`Zm=v`) | stops reading at the first `=` | refused |
| unused bits before the padding not zero (`Zh==`) | ignores the bits | refused |

These two cases are counted by the differential test on every run. If a console is ever seen sending such text,
this page and the decoder change together.

The reference's decoder looks characters up in a table without checking for bytes above `0x7f`; the tests never
hand it any.

`src/proto/base64.h`
