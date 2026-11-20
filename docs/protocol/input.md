# The controller's state

What the player does reaches the console in two kinds of packets: the *input state* (kind 6) with the sticks
and the motion sensors, and the *input history* (kind 1) with the buttons. This page covers the input state;
the history comes with the controllers.

Like picture and sound packets, input packets may be lost and are never sent again. Each is sealed
([stream-setup.md](stream-setup.md)): for both kinds the code is at offset 8 and the key position at offset 4.

## The packet

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | 6 |
| 1 | 2 | sequence number, counting up from 0, most significant byte first |
| 3 | 1 | 0 |
| 4 | 4 | key position |
| 8 | 4 | authentication code |
| 12 | 25 or 28 | the state, encrypted |

**[checked]** The state is encrypted with the client's key stream, beginning at the packet's key position
plus 16; the packet counts for the state's size plus 16 (the mock console decrypts what the reference's own
client sends this way, and reads a resting controller's state in every packet).

**[read]** A client sends a state whenever the controller's changes, and every 200 ms when nothing does.

## The state

25 bytes to a PS4, 28 to a PS5.

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | `a0` |
| 1 | 6 | rate of turn about x, y and z: three 16-bit numbers, least significant byte first |
| 7 | 6 | acceleration along x, y and z, the same way |
| 13 | 4 | the controller's orientation, packed (below), least significant byte first |
| 17 | 8 | left stick x and y, right stick x and y: four signed 16-bit numbers, most significant byte first |
| 25 | 3 | PS5 only: `00 00 01` |

**[checked]** A rate of turn travels as 65535 x (value + 30) / 60, an acceleration as 65535 x (value + 5) / 10,
both rounded down, computed in single precision. This client holds values beyond those ranges at the ends.

**[checked]** The orientation is a rotation of four parts (x, y, z, w) whose squares add up to 1. Its packed
form, from the lowest bit: one bit that is set when the part of the largest size is negative; two bits that
say which part that is; then the three other parts in their order, nine bits each: (part + 0.7071...) x 511 /
1.4142..., rounded down, a part beyond +-0.7071... held there first. Of parts of equal size the first counts
as the largest.

**[read]** A controller that nothing is done with: no turning, an acceleration of 1 along y, the rotation
(0, 0, 0, 1), both sticks at 0. Its state is `a0 ff7f ff7f ff7f ff7f 9999 ff7f fef7ef1f` and eight zero bytes.

Until controllers are implemented this client sends that state every 200 ms.

## What has been confirmed

- **[checked]** by differential test against the reference (`tools/oracle/run.sh`): the state for both
  families, byte for byte, on 6,000 random controllers.
- **[checked]** by the reference's own client against the mock console (`tests/scenario/phase2d.sh`): the mock
  decrypts every input state it sends and reads a resting controller's, the same bytes this client sends.
- **[seen]** A PS5 that was sent a resting controller's state every 200 ms kept its stream up, and sent one
  data chunk of kind 9 (information for the controller). That it read the states is not shown by this.

`src/proto/input.h`
