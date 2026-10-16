# avmcv710

An independent, reverse-engineered V4L2 kernel driver for the AVerMedia
**ExtremeCap U3** (CV710, USB `07ca:0710`) on Linux.

> **Not affiliated with AVerMedia.** This is an independent interoperability
> project. It is not affiliated with, authorised by or endorsed by AVerMedia.
> "AVerMedia", "ExtremeCap U3" and "CV710" are trademarks of their respective
> owners, used only to identify the hardware. See [Legal](#legal).

The driver replays the vendor's FX3 / ADV7604 / ECP3 bring-up over the device's
control endpoint, parses the FPGA's C0/C1-framed DMA stream, and exposes the
result as a standard V4L2 `YUYV` capture node, with an opt-in ALSA capture PCM
for the embedded audio.

It is the kernel-space sibling of the userspace driver
[`cv710userspace`](../cv710userspace). That project, together with the
extracted vendor Windows/macOS drivers, is the behavioural source of truth for
everything here.

- **Hardware:** `07ca:0710` (AVerMedia ExtremeCap U3); binds interface 0 only.
- **Status:** video capture is feature-complete. Audio and hardware frame-rate
  conversion are opt-in.
- **Docs:** the parity matrix, the reverse-engineering ledger and the open
  tickets are in [`docs/DEVLOG.md`](docs/DEVLOG.md); [`AGENTS.md`](AGENTS.md)
  is the continuation handoff, and worth reading before changing behaviour.

## Contents

- [Features](#features)
- [Build and install](#build-and-install)
- [Test](#test)
- [Module parameters](#module-parameters)
- [File map](#file-map)
- [Kernel safety notes](#kernel-safety-notes)
- [Non-goals](#non-goals)
- [Legal](#legal)

## Features

- **Capture node.** `/dev/videoN`, `YUYV` (YUY2), single plane. Buffers are
  always sized `AVM_FRAME_BPF` (4,608,000 B), so a mid-stream mode change can
  never overflow them.
- **Passthrough geometry.** The FPGA does not scale. `S_FMT` reports the live
  geometry and every locked change raises `V4L2_EVENT_SOURCE_CHANGE` so a
  consumer re-queries instead of reading the new raster with the old stride.
  Before the first frame locks, 1920x1080 is reported.
- **Non-native requests** get the vendor's out-of-range overlay, and live
  frames are dropped while the mismatch holds.
- **Interlaced sources.** 1080i / 576i / 480i fields are woven into full
  frames, including the CV-28k re-classification for spuriously interlaced
  480p/576p.
- **Standby overlays.** No-signal and out-of-range, built from the vendor's
  640x480 bitmaps, RLE-compressed into the module, decoded per kind and scaled
  to fill the frame.
- **Receiver status.** A 1 Hz ADV7604 poll while streaming (TMDS lock,
  interlace, measured geometry).
- **Mode-switch hysteresis.** The geometry only changes after 4 consecutive
  frames of the new size.
- **Frame-rate conversion (opt-in).** `cvx8897=1` maps `S_PARM` onto the
  CVX8897; the advertised interval menu is `{native, native/2}`, because the
  converter can only halve the source rate.
- **Audio (opt-in).** `avm_audio=1` registers a capture PCM
  (`hw:CARD=AVMCV710`, S16LE stereo at 32 / 44.1 / 48 kHz).
- **Fast bootstrap.** 871 commands replayed over EP1 from a generated table,
  the same final register state as the full capture but roughly 8x faster.
  `tools/check_bootstrap.py` verifies it (capture parity and the 256-byte
  EDID) against the vendor binaries.

## Build and install

Out-of-tree against the running kernel. Kernels built with clang (CachyOS and
others) must use `LLVM=1`; gcc cannot compile against clang-only flags.

```sh
make KDIR=/lib/modules/$(uname -r)/build LLVM=1
sudo make KDIR=/lib/modules/$(uname -r)/build LLVM=1 install
sudo depmod -a && sudo modprobe avmcv710
```

Or, to free the device and reload cleanly in one step:

```sh
sudo ./fix-driver.sh                 # video-only module (audio off, the default)
sudo ./fix-driver.sh avm_audio=1     # with the ALSA capture card
```

On gcc-built kernels plain `make` works. `fix-driver.sh` installs the tree
build first whenever its `srcversion` differs from the loaded module, so "I
edited it but nothing changed" cannot silently happen.

`avmcv710-bmp.h` and `avmcv710-cvx8897.h` are generated and gitignored. After a
fresh clone run `python3 tools/gen_bmp.py` (and `python3 tools/gen_cvx8897.py`
if the build asks for it) before building.

## Test

```sh
# after plugging in the CV710 (the journal shows "avmcv710: ... ready")
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext
v4l2-ctl -d /dev/videoN --set-fmt-video=width=1920,height=1080,pixelformat=YUYV
v4l2-ctl -d /dev/videoN --stream-mmap --stream-count=30 --stream-to=out.yuv
```

`v4l2-ctl` prints dropped/received counts on exit; the internal parser counters
(valid/dropped/corrupt/padded/resync) print on `STREAMOFF`. Pull the HDMI cable
while streaming to see the no-signal overlay, and ask for a non-native size to
see the out-of-range one. Logs go to `journalctl -k -f` (`dmesg` is not
readable on modern kernels).

Geometry is auto-detected from the frame word count, so the driver delivers
whatever the source produces. If the picture looks wrong, check the source's
output mode first.

## Module parameters

| Parameter | Type / access | Default | Meaning |
| --- | --- | --- | --- |
| `cvx8897` | int, 0644 (runtime) | `0` | `1` = follow `S_PARM` through the CVX8897 frame-rate converter |
| `avm_audio` | bool, 0444 | `0` (off) | register the `AVMCV710` ALSA capture card at probe |
| `avm_audio_index` | int, 0444 | `-1` | ALSA card index (`-1` = auto) |
| `avm_audio_id` | charp, 0444 | `"AVMCV710"` | ALSA card id |
| `auto_reset` | bool, 0644 | `0` (off) | watchdog may `usb_reset_device()` a silent stream |

`MODULE_SOFTDEP("pre: snd-pcm")`, 4x2 MB URBs, `MODULE_LICENSE("GPL")`.

## File map

| File | Role |
| --- | --- |
| `avmcv710.h` | device/parser/buffer structs, constants, cross-file API |
| `avmcv710-hw.c` | bootstrap replay (EP1), I2C register access, FPGA work mode, DMA reset |
| `avmcv710-parser.c` | C0/C1/audio framing parser, mode detect, reconcile, weaving, delivery |
| `avmcv710-usb.c` | probe/disconnect, URB pipeline, parse workqueue, teardown, watchdog |
| `avmcv710-video.c` | V4L2 + vb2 queue, format negotiation, streaming lifecycle |
| `avmcv710-cvx8897.c` | CVX8897 frame-rate converter (opt-in) |
| `avmcv710-audio.c` | ALSA capture PCM (opt-in, S16LE stereo 32/44.1/48 kHz) |
| `avmcv710-status.c` | ADV7604 status poll (1 Hz) and standby overlays |
| `avmcv710-bootstrap.h` | tracked, pre-parsed 871-command bootstrap table |
| `avmcv710-bmp.h` | generated (gitignored) RLE standby overlays |
| `avmcv710-cvx8897.h` | generated (gitignored) CVX8897 register tables |
| `fix-driver.sh` | clean reload helper (closes OBS, checks `srcversion`, installs the tree build) |
| `tools/gen_bootstrap.py` | generates `avmcv710-bootstrap.h` |
| `tools/check_bootstrap.py` | verifies the bootstrap against the vendor binaries |
| `tools/gen_bmp.py` | generates `avmcv710-bmp.h` from the vendor overlays |
| `tools/gen_cvx8897.py` | generates `avmcv710-cvx8897.h` |

Protocol details (markers, checksums, chunk size) are in the userspace repo's
[`PROTOCOL.md`](../cv710userspace/PROTOCOL.md); known hardware limitations are
in [`ISSUES.md`](../cv710userspace/ISSUES.md).

## Kernel safety notes

The userspace driver this derives from shipped kernel-crashing bugs. This port
is written so that no userspace-visible condition can panic the kernel:

- **Bounds-checked parser.** Every marker lookup holds the chunk and re-parses
  it with the next chunk's start (CV-01), so no read underflows or runs past
  the buffer. Word counts are compared, never subtracted into a copy length.
- **Frames are reconciled to the mode.** A count outside the active mode's
  detection window is corruption and is dropped. A count inside it is
  reconciled: short frames are black-padded with `AVM_PAD_WORD` (`0x80108010`,
  studio black), long ones truncated, matching the vendor driver. Reconcile
  never writes past `AVM_MAX_FRAME_WORDS`.
- **Interlaced weaving is bounded.** Fields are woven only after the
  full-frame word count is checked, and every row is written inside `p->weave`.
- **URB teardown is ordered.** URBs are anchored at submit and killed before
  anything is freed; `stop_streaming()` kills, flushes the workqueue, kills
  again (a worker can re-anchor in between) and then returns buffers. The
  workqueue and the URBs are freed only in the release callback, after the last
  file handle closes.
- **Parsing runs only on a workqueue.** The completion callback chains the
  chunk and kicks the worker. No locking, allocation or V4L2 calls in atomic
  context.
- **Every stop path returns vb2 buffers** (`STREAMOFF`, queue cancellation, a
  failed `STREAMON`), and a disconnect flags the queue errored so a blocked
  reader wakes with `-EIO` instead of hanging.

## Non-goals

- **EDID patching / VID_STD auto-switch.** The receiver's cross-checks are
  read-only telemetry.
- **FPGA scaler emulation.** A non-native `S_FMT` is answered the way the
  official driver does, with the out-of-range overlay instead of scaling.
- **HDCP.** No implementation, no interrogation, no gating. See
  [Legal](#legal).
- **Auto-recovery from the soft-reload "silent pipe".** The watchdog is only a
  tripwire; see [`AGENTS.md`](AGENTS.md) section 4.

## Legal

*Not legal advice. If any of this matters to you, talk to a lawyer.*

- **No affiliation.** Independent interoperability project, not affiliated
  with, authorised by or endorsed by AVerMedia. "AVerMedia", "ExtremeCap U3"
  and "CV710" are trademarks of their respective owners, used only to identify
  the hardware this driver targets.
- **Reverse-engineered for interoperability.** Written by observing the
  device's own USB traffic and the vendor's shipped binaries. It contains no
  vendor source code.
- **No HDCP, no circumvention.** The driver implements no HDCP: no
  authentication, no key material, no cipher, no key derivation, and no
  attempt to defeat, avoid, remove or impair any technological protection
  measure. The bootstrap sequence is a byte-for-byte replay of the vendor
  driver's own hardware initialisation. Whatever the hardware does with a
  protected input, it does with or without this driver.
- **Your responsibility.** Whether you may capture, record or redistribute any
  particular signal is governed by laws such as the DMCA (17 U.S.C. 1201) and
  the EU Information Society Directive (Art. 6) and by the terms of the
  content's licence. That is between you and the rights holders.
- **Vendor data in the tree.** The bootstrap table is a recording of the
  device's own USB traffic, and the EDID blob is the device's own identity.
  The standby overlay bitmaps under `cv710userspace/assets/`, and the RLE
  tables generated from them, are AVerMedia's artwork, reproduced so the Linux
  presentation matches the official driver. Copyright remains with AVerMedia;
  remove them if that is a problem for your distribution.
- **No warranty.** Provided "as is", without warranty of any kind. Loading an
  out-of-tree kernel module can crash your kernel.

## Licence

GPL-2.0 (`MODULE_LICENSE("GPL")`), as required of a Linux kernel module.
Structure and protocol knowledge are derived from the MIT-licensed
[`cv710userspace`](../cv710userspace). See [Legal](#legal) for the overlay
assets.
