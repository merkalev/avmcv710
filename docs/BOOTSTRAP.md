# What the bootstrap blob actually is

`avmcv710-bootstrap.h` replays 871 EP1 commands (841 writes, 4495 bytes). It is
a recording of the vendor driver's traffic, so "where did these numbers come
from" is a fair review question. This note breaks it down so each part can be
judged separately. Counts come from decoding the generated table; nothing here
is guessed.

## Composition (841 writes)

| Leading byte | Count | What it is |
| --- | --- | --- |
| `0x01 <bridge> 01 <reg> <val>` | 696 | single-register I2C write through the FX3 I2C bridge |
| `0x05 / 0x06 / 0x07 / 0x09 / 0x03 / 0x08 / 0x0e` | 145 | FX3 / FPGA command framing (see below) |

The 696 I2C writes by bridge id (7-bit I2C address):

| Bridge | Writes | Device (per ADV7604 map layout in Linux `adv7604.c`) |
| --- | --- | --- |
| `0x20` | 521 | ADV7604 IO map (375 of them in one contiguous run) |
| `0x34` | 28 | ADV7604 HDMI map |
| `0x22` | 43 | ADV7604 CP map (content-protection map slot in the vendor naming) |
| `0x74` | 42 | PCA9539 GPIO expander |
| `0x10` | 29 | FPGA work-mode / control registers (`0x10/0x05` bit 1 = work) |
| `0x32`, `0x26`, `0x18` | 14, 7, 2 | further ADV7604 maps, CVX8897 (`0x18`) |
| `0x36` | 10 | the 256-byte EDID, 8 x 32-byte writes |

## Parts that are ordinary and decodable

The I2C register programming (696 writes) is plain register init for parts with
public Linux drivers or datasheet-derived maps. Nothing in it needs to be
opaque; what is missing is a *named* source, i.e. each write mapped to a
register name and a reason. That is a documentation job, not a reverse
engineering job, and it is the main way to make the blob reviewable. The EDID is
the device's own identity and is byte-identical to the vendor binaries.

## The part that is still opaque

The leading ~127 commands (types `0x05 0x06 0x07 0x09`) are FX3/FPGA framing,
not I2C. Their `0x07` payloads (268 bytes in total) are high entropy
(about 5.3 bits/byte over that sample) rather than looking like register
addresses or small constants. This repository does **not** know what they are.
They could be an FPGA configuration/calibration table; they could be something
else. We have no evidence either way, so this is recorded as unknown and is not
characterised further here.

Consequences, stated plainly:

- The blob cannot be reduced to a clean-room, from-the-datasheet sequence
  until this head section is understood.
- Any upstream submission would be asked about exactly this section.

## How to close it

1. In `libC877Driver.dylib` (symbols intact) find the caller that emits these
   framing commands and read how the payload is produced: constant table, or
   computed at run time. A constant table can be named; a computed one needs the
   algorithm.
2. Replace the 696 I2C writes with a documented table (`bridge, reg, value,
   name, comment`) generated from the existing capture, keeping
   `tools/check_bootstrap.py` as the regression gate (it already proves
   state-equivalence with the full capture).
3. Only then decide whether the head section can be re-derived or has to stay a
   recorded constant with a stated origin.
