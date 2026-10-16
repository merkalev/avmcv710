# avmcv710 — V4L2 kernel capture driver for the AVerMedia ExtremeCap U3 (CV710)

Out-of-tree Linux kernel driver for the AVerMedia **ExtremeCap U3** (CV710,
USB `07ca:0710`).  It replays the vendor's FX3/ADV7604/ECP3 bring-up over the
device's control endpoint, parses the FPGA's C0/C1-framed DMA stream and
exposes it as a standard V4L2 `YUYV` capture node — plus an opt-in ALSA
capture PCM for the embedded audio.

It is the kernel-space sibling (and third-generation port) of the
reverse-engineered userspace driver [`cv710userspace`](../cv710userspace)
(`liblgx` → `UsbStream.cpp` / `lgxdevice.cpp`).  That project, together with
the extracted vendor Windows/macOS drivers, is the behavioural source of
truth for everything in this driver.

- **Hardware:** `07ca:0710` (AVerMedia ExtremeCap U3); binds interface 0 only.
- **Status:** video capture is feature-complete; audio and hardware
  frame-rate conversion are opt-in.  The official-driver parity matrix, the
  dated reverse-engineering ledger and the open tickets live in
  [`docs/DEVLOG.md`](docs/DEVLOG.md).  The continuation handoff is
  [`AGENTS.md`](AGENTS.md) — read it before changing driver behaviour.

## Safety first

The userspace driver this is derived from shipped kernel-crashing bugs; this
port is written so that no userspace-visible condition can panic the kernel:

- **Bounds-checked parser.** Every marker lookup holds the chunk and re-parses
  it with the start of the next chunk (CV-01), so no read can underflow or
  run past the buffer.  The parser never subtracts a user-controlled count
  from a frame budget (`CV710_1080P_FRAME_WORDS - curWords`-style underflow is
  not reproduced); word counts are compared, never subtracted into a copy
  length.
- **Frames are reconciled to the mode (vendor parity).** A frame whose word
  count lands *outside* the active mode's detection window is corruption and
  is **dropped**.  A count *inside* the window is reconciled to the mode
  geometry: a short frame is black-padded with `AVM_PAD_WORD` (`0x80108010`,
  studio black) and a long one truncated — which is what the vendor driver
  does (`docs/DEVLOG.md`, `PARSER-PAD`, 2026-10-16).  This replaced the
  userspace CV-23 "exact size or drop" gate, which is stricter than the
  hardware actually is.  Reconcile never writes past `AVM_MAX_FRAME_WORDS`.
- **Woven-frame size bound.** Interlaced fields are woven only after the
  full-frame word count is checked against `AVM_MAX_FRAME_WORDS`, and every
  woven row is written inside `p->weave`; an exotic adaptive interlaced
  geometry is dropped, never overrun.
- **Resynchronisation.** Invalid C0/C1 headers, FPGA clock-recovering
  trailers and mode-transition frames are counted and discarded; the parser
  re-anchors on the next valid C0.
- **Safe URB teardown.** All URBs are anchored at submit and killed with
  `usb_kill_anchored_urbs()` before anything is freed; `stop_streaming()`
  kills → `flush_workqueue()` → kills again (a worker can re-anchor in
  between) → returns buffers.  The final `destroy_workqueue()` / URB free
  happens only in the video-device release callback, i.e. after every open
  file handle is gone.
- **Error-resilient streaming.** The URB completion callback chains every
  transfer (errors included); the worker parses only clean chunks and
  resubmits a transiently failed URB, retiring one only after a retry cap so
  a persistently failing endpoint cannot spin the worker.  On disconnect the
  vb2 queue is flagged errored (`vb2_queue_error()`), so a blocked
  reader/poller wakes with `-EIO`/`POLLERR` instead of hanging.
- **Parsing only in a workqueue.** The completion callback only chains the
  chunk and kicks the worker.  No locking, allocation or V4L2 calls in atomic
  context.
- **vb2 buffers returned on every stop path.** `STREAMOFF`, queue
  cancellation and failed `STREAMON` all route queued buffers back with
  `VB2_BUF_STATE_ERROR`.

## Features

- **V4L2 capture node:** `/dev/videoN`, `YUYV` (YUY2), one plane.  Planes are
  always sized `AVM_FRAME_BPF` (4,608,000 B) so a mid-stream mode change can
  never overflow the buffer.
- **Resolution is a passthrough; the FPGA does not scale.**  Format
  negotiation is source-paced: `S_FMT` reports the live geometry, and every
  locked change raises `V4L2_EVENT_SOURCE_CHANGE` so a consumer re-queries
  `g_fmt` instead of reading the new raster with the old stride.  Before the
  first frame locks, the most common sink geometry (1920x1080) is reported.
- **Non-native resolution → the vendor's out-of-range overlay.**  If a
  consumer negotiates a geometry the source cannot provide, the official
  driver paints `aver_custom_out_of_range.bmp`; this driver does the same
  (`avm_req_mismatch`) and drops live frames while the mismatch holds.
- **Interlaced** 1080i / 576i / 480i fields detected from word counts and
  woven into full frames.  The CV-28k re-classification is ported: when the
  FPGA's C0 interlace bit is set spuriously for progressive 480p/576p, and the
  ADV7604 measures a matching progressive raster whose word count is not a
  genuine field count, the receiver wins and the frame is presented at full
  height instead of being woven to double height.
- **Standby overlays** (no-signal / out-of-range).  The vendor's four 640x480
  bitmaps are RLE-compressed into the module, decoded once per kind and
  **scaled to fill the frame** ("contain", black letterbox, bilinear luma),
  then delivered through the normal frame path once the stream has been idle
  for more than 250 ms.  Generated from the macOS rendition of the vendor
  assets (`tools/gen_bmp.py`).
- **Receiver status poll.**  While streaming, a ~30 Hz work ticks; every
  30th tick (1 Hz) it reads the ADV7604 TMDS-lock, interlace and
  measured-geometry registers and publishes `dev->sig` for the overlay /
  geometry logic.
- **Mode-switch hysteresis.**  The active geometry changes only after 4
  consecutive frames of the new size; every transitional frame is dropped.
- **Hardware frame-rate conversion (opt-in, `cvx8897=1`).**
  `enum_frameintervals()` advertises only `{native, native/2}`, because the
  CVX8897 can only halve the source rate.  Geometry is never scaled.
- **Audio (opt-in, `avm_audio=1`).**  Audio packets (marker `0x58`) are
  demuxed into an ALSA capture PCM (`hw:CARD=AVMCV710`), S16LE stereo at
  32 / 44.1 / 48 kHz, using the snd-usb-audio push model.  Off by default.
- **Fast bootstrap.**  871 commands (841 writes / 30 reads) replayed over EP1
  from a generated table (`tools/gen_bootstrap.py` →
  `avmcv710-bootstrap.h`): the same final register state as the full capture,
  ~8x faster to apply.  `tools/check_bootstrap.py` proves it against the
  vendor binaries — capture parity, plus the 256-byte EDID is byte-identical
  to the copy embedded in `avmu3_x64.sys` and `libC877Driver.dylib`.

## Build and install

Out-of-tree against the running kernel.  Kernels built with clang (CachyOS and
others) must use `LLVM=1`; gcc cannot compile against clang-only flags:

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

On gcc-built kernels plain `make` works.  `fix-driver.sh` closes OBS, waits
out the module refcount, and installs the tree build first when its
`srcversion` differs from the loaded module — so "I edited it but nothing
changed" cannot silently happen.

`avmcv710-bmp.h` and `avmcv710-cvx8897.h` are generated and gitignored; after a
fresh clone run `python3 tools/gen_bmp.py` (and
`python3 tools/gen_cvx8897.py` if the build asks) before building.

## Test

```sh
# after plugging in the CV710 (the journal should show "avmcv710: ... ready")
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext
v4l2-ctl -d /dev/videoN --set-fmt-video=width=1920,height=1080,pixelformat=YUYV
v4l2-ctl -d /dev/videoN --stream-mmap --stream-count=30 --stream-to=out.yuv
```

`v4l2-ctl` prints dropped/received counts on exit; the internal parser
counters (valid/dropped/corrupt/padded/resync) are printed on `STREAMOFF`.
Pull the HDMI cable while streaming to see the no-signal overlay, and ask for
a non-native size to see the out-of-range one.  Logs: `journalctl -k -f`
(`dmesg` is not readable on modern kernels).

The frame word count / geometry auto-detection means the driver delivers
whatever the HDMI source produces; if the picture is wrong, check the source's
output mode first.

### Frame rate (CVX8897 converter)

Hardware frame-rate conversion is opt-in, because deciding "half the source
rate" needs the *input* rate; the converter infers it from the delivered rate
while it is in native (non-converted) mode:

```sh
# 0 = off (default), 1 = follow S_PARM (halve when asked for half the source)
sudo modprobe avmcv710 cvx8897=1
echo 1 | sudo tee /sys/module/avmcv710/parameters/cvx8897   # runtime, no reload
```

With the converter enabled, `enum_frameintervals()` offers exactly
`{native, native/2}` for the detected source and `S_PARM` selects the matching
half-rate table; with it off, the source paces the cadence and `S_PARM` cannot
retime it.  Earlier builds advertised the vendor's full 60/50/30/25/24/15 menu;
that was dishonest — the converter cannot triple or quadruple a rate.  The
converter never scales geometry.

## Module parameters

| Parameter | Type / access | Default | Meaning |
| --- | --- | --- | --- |
| `cvx8897` | int, 0644 (writable at runtime) | `0` | `1` = follow `S_PARM` through the CVX8897 frame-rate converter |
| `avm_audio` | bool, 0444 | `0` (off) | register the `AVMCV710` ALSA capture card at probe |
| `avm_audio_index` | int, 0444 | `-1` | ALSA card index (`-1` = auto) |
| `avm_audio_id` | charp, 0444 | `"AVMCV710"` | ALSA card id |
| `auto_reset` | bool, 0644 | `0` (off) | watchdog may `usb_reset_device()` a silent stream (TODO-4) |

`MODULE_SOFTDEP("pre: snd-pcm")`; 4×2 MB URBs; `MODULE_LICENSE("GPL")`.

## Driver internals / file map

The full reverse-engineering ledger, the parity matrix and the prioritized
TODO list are in [`docs/DEVLOG.md`](docs/DEVLOG.md).

| File | Role |
| --- | --- |
| `avmcv710.h` | device/parser/buffer structs, constants, cross-file API |
| `avmcv710-hw.c` | bootstrap replay (EP1), I2C register access, FPGA work mode, DMA reset |
| `avmcv710-parser.c` | C0/C1/audio framing parser, mode detect, reconcile, weaving, delivery |
| `avmcv710-usb.c` | probe/disconnect, URB pipeline, parse workqueue, teardown, watchdog |
| `avmcv710-video.c` | V4L2 + vb2 queue, format negotiation, streaming lifecycle |
| `avmcv710-cvx8897.c` | CVX8897 frame-rate converter (opt-in) |
| `avmcv710-audio.c` | ALSA capture PCM (opt-in `avm_audio=1`, S16LE stereo 32/44.1/48 kHz) |
| `avmcv710-status.c` | ADV7604 status poll (1 Hz while streaming) + standby overlays |
| `avmcv710-bootstrap.h` | tracked, pre-parsed 871-command bootstrap table |
| `avmcv710-bmp.h` | generated (gitignored) RLE standby overlays (macOS rendition) |
| `avmcv710-cvx8897.h` | generated (gitignored) CVX8897 register tables |
| `fix-driver.sh` | clean reload helper (closes OBS, checks `srcversion`, installs the tree build) |
| `tools/gen_bootstrap.py` | generates `avmcv710-bootstrap.h` from the original trace |
| `tools/check_bootstrap.py` | verifies the generated bootstrap (capture parity + EDID vs the vendor binaries) |
| `tools/gen_bmp.py` | generates `avmcv710-bmp.h` from the vendor overlays (default source: `cv710userspace/assets/`, the macOS rendition) |
| `tools/gen_cvx8897.py` | generates `avmcv710-cvx8897.h` from the userspace tables |

Protocol details (markers, checksums, chunk size) are documented in the
userspace repo's [`PROTOCOL.md`](../cv710userspace/PROTOCOL.md); known
hardware limitations are in
[`ISSUES.md`](../cv710userspace/ISSUES.md).

## Non-goals (by design)

- **EDID patching / VID_STD auto-switch** — the receiver's cross-checks are
  read-only telemetry.
- **FPGA scaler emulation** — a non-native `S_FMT` is answered the way the
  official driver does, with the out-of-range overlay, not with scaling.
- **HDCP** — no implementation, no interrogation, no gating; see
  [Legal](#legal) below.
- **Auto-recovery from the soft-reload "silent pipe"** (TODO-4; the watchdog
  is the tripwire — see [`AGENTS.md`](AGENTS.md) §4).

## Legal

*Not legal advice.  If any of this matters to you, talk to a lawyer.*

- **No affiliation.**  This is an independent interoperability project.  It
  is not affiliated with, authorised by, or endorsed by AVerMedia.
  "AVerMedia", "ExtremeCap U3" and "CV710" are trademarks of their respective
  owners, used here only to identify the hardware this driver targets.
- **Reverse-engineered for interoperability.**  The driver was written by
  observing the device's own USB traffic and the vendor's shipped binaries; it
  contains no vendor source code.
- **No HDCP, no circumvention.**  The driver implements no HDCP: no
  authentication, no key material, no cipher, no key derivation, and no
  attempt to defeat, avoid, remove or impair any technological protection
  measure.  The bootstrap sequence is a byte-for-byte replay of the vendor
  driver's own hardware initialisation; whatever the hardware does with a
  protected input, it does with or without this driver.
- **Your responsibility.**  Whether you may capture, record or redistribute
  any particular signal is governed by laws such as the DMCA (17 U.S.C.
  §1201) and the EU Information Society Directive (Art. 6), and by the terms
  of the content's licence.  That is between you and the rights holders.
- **Vendor data in the tree.**  The bootstrap table is a recording of the
  device's own USB traffic, and the EDID blob is the device's own identity.
  The standby overlay bitmaps under `cv710userspace/assets/` (and the RLE
  tables generated from them) are AVerMedia's artwork, reproduced so the Linux
  presentation matches the official driver; copyright remains with AVerMedia.
  Remove them if that is a problem for your distribution.
- **No warranty.**  Provided "as is", without warranty of any kind.  Loading
  an out-of-tree kernel module can crash your kernel.

## Licence

GPL-2.0 (`MODULE_LICENSE("GPL")`), as required of a Linux kernel module.
Structure and protocol knowledge are derived from the MIT-licensed
[`cv710userspace`](../cv710userspace); see [Legal](#legal) for the overlay
assets.
