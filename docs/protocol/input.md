# The controller's state

What the player does reaches the console in two kinds of packets: the *input state* (kind 6) with the sticks
and the motion sensors, and the *input history* (kind 1) with the buttons, the triggers and the touchpad.

Like picture and sound packets, input packets may be lost and are never sent again. Each is sealed
([stream-setup.md](stream-setup.md)): for both kinds the code is at offset 8 and the key position at offset 4.

## The input state packet

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
The reference names 8 ms as the least time between two states and does not keep to it yet; this client does:
a state that changes all the time goes out every 8 ms, and the last change is never left unsent.

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

## The input history packet

The same twelve header bytes with kind 1. **[checked]** Its sequence number is the number of its newest event,
events counting from 0; its fourth byte is the number of events in it less one. The events follow, newest
first, encrypted like a state: from the packet's key position plus 16 on, the packet counting for their size
plus 16.

### Events

**[checked]** A button is `80`, a code, and for most buttons a third byte: `ff` pressed, `00` released.

| Button | Code | | Button | Code |
|---|---|---|---|---|
| up | `80` | | cross | `88` |
| down | `81` | | circle | `89` |
| left | `82` | | square | `8a` |
| right | `83` | | triangle | `8b` |
| L1 | `84` | | L2 | `86`, third byte: how far it is pulled, 0 to 255 |
| R1 | `85` | | R2 | `87`, the same |

Six buttons have no third byte; their code says released, and 0x20 more says pressed:

| Button | Released | Pressed |
|---|---|---|
| options | `8c` | `ac` |
| share (create) | `8d` | `ad` |
| PS | `8e` | `ae` |
| L3 | `8f` | `af` |
| R3 | `90` | `b0` |
| touchpad click | `91` | `b1` |

**[checked]** A finger on the touchpad is five bytes: `d0` when it is down or has moved, `c0` when it is
lifted; its number (0 to 127); then x and y in twelve bits each, x first. A lifted finger is told with the
place it had.

### Which events, in which order

**[checked]** When a controller's state changes, its events are, in this order: buttons released; touches;
buttons pressed; L2; R2. (Releases come before touches so that a finger does not lift while the click it made
still counts as held.) A finger that another one replaces in the same change is told as lifted; the new one
follows with its own next change.

### Repeats

Nothing acknowledges an input packet. **[checked]** So every history packet carries earlier events again:
there is one packet per event, holding that event and up to eight before it.

**[read]** And the last packet is sent again, with its sequence number, 20, 50, 100 and 200 ms after it was
first sent, unless a newer one has gone out by then: a lost release would otherwise leave its button held.
This client does the same; each such repeat is sealed anew, at its own key position.

A console can tell the events it has seen by their numbers: the packet's sequence number is its first
event's, and each following event's is one less.

## What has been confirmed

- **[checked]** by differential test against the reference (`tools/oracle/run.sh`): the state for both
  families, byte for byte, on 6,000 random controllers; every button, trigger and touch event; and the packets
  wanted after each of 4,000 random changes of a controller.
- **[checked]** by the reference's own client against the mock console (`tests/scenario/phase2d.sh`,
  `phase2e.sh`): the mock decrypts every input state it sends and reads a resting controller's, the same bytes
  this client sends; and when it presses buttons, the mock reads from its history packets the changes, with
  the numbers, that it reads from this client's.
- **[seen]** A PS5 that was sent a resting controller's state every 200 ms kept its stream up for twenty
  seconds, and sent one data chunk of kind 9 (information for the controller, seventeen bytes) at the
  stream's start. That it read the states is not shown by this; a button press on a real console has not been
  seen yet.

`src/proto/input.h`
