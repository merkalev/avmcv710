# avmcv710 — kernel capture driver for the AVerMedia ExtremeCap U3 (CV710)

Third-generation port of the reverse-engineered userspace CV710 driver
(`cv710userspace` → `liblgx` → `UsbStream.cpp` / `lgxdevice.cpp`).  Runs the
device's full FX3/ADV7604/ECP3 bring-up, parses the FPGA's C0/C1-framed DMA
stream and exposes it as a standard V4L2 YUY2 capture node.  Buildable as an
out-of-tree module against the running kernel.

Supported hardware: `07ca:0710` (AVerMedia ExtremeCap U3).  The driver binds
only to interface 0.

## Safety first

The upstream userspace driver has had kernel-crashing bugs shipped in it; this
port is written so that no userspace-visible condition can panic the kernel:

- **Bounds-checked parser.** Every marker lookup holds the chunk and re-parses
  it with the start of the next chunk (CV-01), so no read can underflow or
  run past the buffer.  The parser never subtracts a user-controlled count
  from a frame budget (`CV710_1080P_FRAME_WORDS - curWords` style underflow is
  not reproduced); word counts are compared, never subtracted into a copy
  length.
- **Exact-size frame gate.** A frame is delivered only if its word count
  exactly matches the locked mode geometry (CV-23).  A mismatched frame is
  dropped, never padded, repaired or partially copied.  For interlaced modes
  the gate compares against the *field* word count (the incoming C1 count),
  exactly as the userspace driver does.
- **Woven-frame size bound.** Interlaced fields are woven only after the
  full-frame word count is checked against `AVM_MAX_FRAME_WORDS`, and every
  woven row is written inside `p->weave`; an exotic adaptive interlaced
  geometry is dropped, never overrun.
- **Drop, never repair.** Invalid C0/C1 headers, FPGA clock-recovering
  trailers, and mode-transition frames are counted and discarded; the parser
  resynchronises on the next valid C0.
- **Safe URB teardown.** All URBs are anchored at submit and killed with
  `usb_kill_anchored_urbs()` before anything is freed; `stop_streaming()`
  kills → `flush_workqueue()` → returns buffers, in that order.  The final
  `destroy_workqueue()` / URB free only happens in the video-device release
  callback, i.e. after every open file handle is gone.
- **Error-resilient streaming.** The URB completion callback chains every
  transfer (errors included); the worker parses only clean chunks and
  resubmits a transiently failed URB, retiring one only after a retry cap so
  a persistently failing endpoint cannot spin the worker.  On disconnect the
  vb2 queue is flagged errored (`vb2_queue_error()`), so a blocked
  reader/poller wakes with `-EIO`/`POLLERR` instead of hanging and queued
  buffers are returned on close.
- **Parsing only in a workqueue.** The URB completion callback only chains
  the chunk and kicks the worker.  No locking, allocation or v4l2 calls in
  atomic context.
- **vb2 buffers returned on every stop path.** `STREAMOFF`, queue
  cancellation and failed `STREAMON` all route queued buffers back with
  `VB2_BUF_STATE_ERROR`.

## Feature scope (phase 1)

- V4L2 capture node: `/dev/videoN`, `YUYV` (YUY2), one plane.
- Resolution auto-detected from the frame word count; the most common sink
  geometry (1920x1080) is reported before the first frame.  Planes are always
  sized `AVM_FRAME_BPF` (4,608,000 B) so a mid-stream mode change can never
  overflow the buffer.  Format negotiation is source-paced (`S_FMT` reports
  the live geometry) and every locked change raises
  `V4L2_EVENT_SOURCE_CHANGE` so a consumer re-queries `g_fmt` instead of
  reading the new raster with the old stride.
- Interlaced 1080i / 576i / 480i fields detected from word counts and woven
  into full frames (device parity flag used once observed to alternate).
  The CV-28k re-classification is ported: when the FPGA's C0 interlace bit is
  set spuriously for progressive 480p/576p (and flaps), and the ADV7604
  measures a matching progressive raster whose word count is not a genuine
  field count, the receiver wins and the frame is presented at full height
  instead of being woven to double height.
- Mode-switch hysteresis: the active geometry only changes after 4
  consecutive frames of the new size; every transitional frame is dropped.
- Fast bootstrap: 871 commands (841 writes / 30 reads) replayed over EP1 bulk
  from a generated table (`tools/gen_bootstrap.py` →
  `avmcv710-bootstrap.h`).  Same final register state as the full capture
  bootstrap, ~8x faster to apply.
- Audio packets (marker `0x58`) are demuxed into an ALSA capture PCM
  (`hw:CARD=AVMCV710`, S16LE stereo, 48 kHz, snd-usb-audio push model).

## Build

Out-of-tree against the running kernel (CachyOS and other clang-built kernels
must use `LLVM=1`; gcc cannot compile against clang-only flags):

```sh
make KDIR=/lib/modules/$(uname -r)/build LLVM=1
sudo make KDIR=/lib/modules/$(uname -r)/build LLVM=1 install
sudo modprobe avmcv710      # after depmod
```

Other distros: `make` (uses `KDIR` default) works if the kernel was built with
gcc.

## Test

```sh
# after plugging in the CV710 (dmesg should show "avmcv710: ... ready")
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext
v4l2-ctl -d /dev/videoN --set-fmt-video=width=1920,height=1080,pixelformat=YUYV
v4l2-ctl -d /dev/videoN --stream-mmap --stream-count=30 --stream-to=out.yuv

# check for loss: v4l2-ctl prints dropped/received counts on exit;
# internal parser counters (valid/dropped/corrupt/resync) are printed on
# STREAMOFF at debug level (dmesg -n 7 while streaming).
```

The frame word count / geometry auto-detection means the driver delivers
whatever the HDMI source produces; if the picture is wrong, check the source's
output mode first.

### Frame rate (CVX8897 converter)

Hardware frame-rate conversion is opt-in, because deciding "half the source
rate" needs the *input* rate; the converter infers it from the delivered
rate while it is in native (non-converted) mode:

```
# 0 = off (default), 1 = follow S_PARM (halve when asked for half the source)
sudo modprobe avmcv710 cvx8897=1
echo 1 | sudo tee /sys/module/avmcv710/parameters/cvx8897   # runtime, no reload
```

With the converter enabled, `enum_frameintervals()` offers the real
60/50/30/25/24/15 menu (S_PARM selects the matching half-rate table for
60 Hz / 50 Hz progressive sources); with it off, the source paces the cadence
and S_PARM cannot retime it. The converter never scales geometry.

## Driver internals / file map

A running development log with the official-driver parity matrix, the
reverse-engineering ledger and the prioritized TODO list lives in
[`docs/DEVLOG.md`](docs/DEVLOG.md).  The handoff document for continuing the
work (state, commands, RE facts, pitfalls, open tickets) is
[`AGENTS.md`](AGENTS.md) — read it before changing driver behavior.

| File | Role |
| --- | --- |
| `avmcv710.h` | device/parser/buffer structs, constants, cross-file API |
| `avmcv710-hw.c` | bootstrap replay (EP1), I2C register access, FPGA work mode, DMA reset |
| `avmcv710-parser.c` | C0/C1/audio framing parser, mode detect, weaving, delivery |
| `avmcv710-usb.c` | probe/disconnect, URB pipeline, parse workqueue, teardown, watchdog |
| `avmcv710-video.c` | V4L2 + vb2 queue, format negotiation, streaming lifecycle |
| `avmcv710-cvx8897.c` | CVX8897 frame-rate converter (opt-in 60→30 / 50→25) |
| `avmcv710-audio.c` | ALSA capture PCM (`hw:CARD=AVMCV710`, S16LE stereo 48 kHz) |
| `avmcv710-status.c` | ADV7604 status poll (10 Hz while streaming) + no-signal/out-of-range overlays |
| `avmcv710-bootstrap.h` | pre-parsed 871-command bootstrap table (tracked) |
| `avmcv710-bmp.h` | generated (gitignored) 640×480 YUY2 overlay bitmaps |
| `avmcv710-cvx8897.h` | generated (gitignored) CVX8897 register tables |
| `tools/gen_bootstrap.py` | generates `avmcv710-bootstrap.h` from the original trace |
| `tools/gen_bmp.py` | generates `avmcv710-bmp.h` |
| `tools/gen_cvx8897.py` | generates `avmcv710-cvx8897.h` from the userspace tables |

Protocol details (markers, checksums, chunk size, HDCP notes) are documented
in the userspace repo's `PROTOCOL.md`.

## Not implemented (by design)

- EDID patching, VID_STD auto-switch (receiver cross-checks are read-only
  telemetry), HDCP gating — single-vendor module, no copy-protection games.
- Auto-recovery from the soft-reload "silent pipe" (TODO-4, watchdog is the
  tripwire; open — see [`AGENTS.md`](AGENTS.md) §4).

## Licence

GPL-2.0 (kernel module).  Structure and bootstrap protocol derived from the
reverse-engineered `cv710userspace` (GPL).