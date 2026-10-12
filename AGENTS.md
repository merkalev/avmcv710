# AGENTS.md — handoff for continuation of the avmcv710 kernel driver

> **Read this first.** This file is the complete handoff for a new agent
> (human or LLM) continuing the avmcv710 work.  It captures the state, the
> commands, the reverse-engineering facts, the pitfalls, and the open tickets
> exactly as they stood when the previous agent stepped back on
> **2026-10-12**.  `docs/DEVLOG.md` has the dated RE ledger and the parity
> matrix; this file is the "where do I start and what do I do" document.

---

## 0. One-paragraph situation

`avmcv710` is an out-of-tree Linux kernel driver (V4L2 video capture + ALSA
audio capture) for the AVerMedia ExtremeCap U3 (CV710, USB `07ca:0710`:
Cypress FX3 + Analog Devices ADV7604 + Lattice ECP3 FPGA).  The goal is to
mirror the official Windows/macOS driver's behavior in OBS, verified against
the extracted vendor driver and the community userspace port rather than
guesses.

**The driver is feature-complete for video/audio/hardware-conversion.**  The
last three tickets (TODO-1 audio, TODO-2 ADV7604 poll, TODO-3 overlays) are
written and committed.  The first install of TODO-2/3's module oopsed the
kernel through the never-hardware-tested ALSA card (TODO-1): the PCM had no
`ops->prepare`, this kernel calls it unconditionally, and WirePlumber's
prepare of the new card was a NULL instruction fetch that collapsed system
audio.  That is **fixed and committed** and audio is now opt-in
(`avm_audio=1`, default off = the previously stable video-only module); the
user still must verify TODO-1/2/3 on hardware (the agent cannot `sudo`).

Then a full bug hunt ("go hunt for all problems in the driver") found and
fixed **8 more bugs** on 2026-10-12 (devlog entry `### 2026-10-12 (late)`):
the ALSA pin not dropping on unbind (the `fix-driver.sh` FAIL), the
adaptive-mode exact-size gate that silently dropped every non-table source
(1366x768 etc.), a `frame_sequence` race, a cross-adapter shared scratch
mode, the URB fail-path 2 MB leak, a pre-init `status_work` cancel, the
bootstrap scratch guard, and the audio `hw_free` UAF barrier.
`fix-driver.sh` was reworked to kill OBS first, poll the module refcount,
and hand over a deliberate `rmmod -f` escape hatch when an old build's
stray ref survives.  TODO-4 (soft-reload auto-recovery) is waiting on a
user-run experiment.  The next agent's job is **coordinate the
verification, then implement TODO-4
based on the experiment result** — not to rewrite anything.

## 1. Where everything lives

```
/home/hp/Projects/Piecap/
├── avmcv710/                 # THIS driver (git repo, commits local, NOT pushed)
│   ├── avmcv710.h            # device struct, protocol constants, prototypes
│   ├── avmcv710-hw.c         # FX3 EP1/I2C bridge, bootstrap replay, FPGA/DMA cmds
│   ├── avmcv710-parser.c     # C0/C1 bulk-stream parser, frame end, audio demux
│   ├── avmcv710-video.c      # V4L2 node, vb2 ops, source-paced formats
│   ├── avmcv710-usb.c        # probe/disconnect/release, URB lifecycle, watchdog
│   ├── avmcv710-cvx8897.c    # CVX8897 frame-rate converter (opt-in)
│   ├── avmcv710-audio.c      # ALSA card "AVMCV710" capture PCM (TODO-1)
│   ├── avmcv710-status.c     # ADV7604 poll + no-signal overlay pump (TODO-2/3)
│   ├── avmcv710-bootstrap.h  # TRACKED: pre-parsed 871-command bootstrap table
│   ├── avmcv710-bmp.h        # GENERATED (gitignored): 4x 640x480 YUY2 overlays
│   ├── avmcv710-cvx8897.h    # GENERATED (gitignored): 251 regs x 14 modes
│   ├── docs/DEVLOG.md        # parity matrix, dated RE ledger, TODO with acceptance
│   ├── docs/AGENTS.md        # this handoff document
│   ├── tools/gen_bmp.py      # regenerates avmcv710-bmp.h
│   ├── tools/gen_bootstrap.py# regenerates avmcv710-bootstrap.h (from the trace)
│   └── tools/gen_cvx8897.py  # regenerates avmcv710-cvx8897.h
├── cv710userspace/           # official-behavior SOURCE OF TRUTH (userspace port)
│   └── src/liblgx/(src/lgxdevice.cpp, liblgx-common/src/usb/UsbStream.cpp, …)
└── driver_extracted/         # vendor Windows/macOS driver + .inf, traces
```

Git notes: all commits since `09a144b` are **local only, nothing pushed**.
`avmcv710-bmp.h` and `avmcv710-cvx8897.h` are gitignored (regenerate with
`python3 tools/gen_bmp.py` / `tools/gen_cvx8897.py` only if the build asks
for them); `avmcv710-bootstrap.h` **is** tracked.

## 2. Build / install / test (agent cannot sudo — user does install)

```bash
# agent-side build (from avmcv710/):
make KDIR=/lib/modules/$(uname -r)/build LLVM=1          # clang build

# user-side install (needs sudo — run this block yourself, user runs it):
sudo make KDIR=/lib/modules/$(uname -r)/build LLVM=1 install
# free the device first: close OBS, stop anything holding /dev/video0
# and anything with hw:AVMCV710 open (audio card pins the module).
sudo rmmod avmcv710 && sudo modprobe avmcv710

# logs are readable WITHOUT sudo:
journalctl -k -f          # kernel ring buffer (dmesg is NOT readable)
```

Kernel: `7.2.6-1-cachyos` (CachyOS), full build tree at
`/lib/modules/$(uname -r)/build`.  Module params: `cvx8897` (int, writable
runtime, 0=off/1=follow s_parm), `avm_audio` (bool, 0=off **default**,
1=register the ALSA card at probe — opt-in until hardware-verified),
`avm_audio_index` (int, -1=auto), `avm_audio_id` (charp, "AVMCV710").
`softdep: pre: snd-pcm`.

## 3. Pending user verification (do this FIRST, it gates everything)

Three features are built and committed but **none verified on hardware**:
TODO-1's first install hit the audio oops (now fixed, opt-in); TODO-2/3 were
never exercised in the broken session.

1. **TODO-1 ALSA audio** (`aa16708` + regression fix) — **opt-in: reload with
   `avm_audio=1`** or the card is not registered (default).  The first install
   oopsed the kernel (missing `ops->prepare`; see devlog) and collapsed the
   audio stack — the fix is committed.  Normal (non-opt-in) use must
   **not** be affected: `/proc/asound/cards` lists PCH (and AVMCV710 only
   with `avm_audio=1`), PipeWire/audio keep working.
   ```bash
   sudo modprobe -r avmcv710 && sudo modprobe avmcv710 avm_audio=1
   arecord -D hw:CARD=AVMCV710 -f S16_LE -r 48000 -c 2 -d 10 /tmp/a.wav
   cat /proc/asound/cards   # must list AVMCV710
   # then: OBS records video+audio; /tmp/a.wav plays back in sync;
   # the rest of the system keeps producing audio (no WirePlumber collapse)
   ```
2. **TODO-2 ADV7604 poll + TODO-3 overlays** (`482fed3` + `7ebb4b6`) — start
   OBS capture and watch `journalctl -k -f`:
   - `avmcv710: STREAMON: ...` then ~100–200 ms later
     `signal: TMDS locked 1920x1080` (or whatever the source is) —
     **proves the poll works**.
   - Pull the HDMI cable while recording → ≤250 ms later OBS shows the
     AVerMedia-style "no signal" splash and the journal logs
     `no video: showing no-signal overlay 1920x1080` (or the out-of-range
     variant).  Replug → `video resumed; overlay cleared` + a fresh
     `signal: TMDS locked`.
   - **Probing failure symptoms to watch for:** a stream of
     `i2c read .. failed` lines would mean EP1 wedged again; report them.
3. **Converter** (older, already tested once but re-verify after the new
   module): `echo 1 | sudo tee /sys/module/avmcv710/parameters/cvx8897`,
   then `v4l2-ctl -d /dev/video0 --set-parm=30 --stream-mmap
   --stream-count=60` → journal shows `cvx8897: halving ...`.

Triage rule: **accept a ticket only on the user's test report**, then move its
`✅ (test pending)` to `✅` in `docs/DEVLOG.md`.

## 4. TODO-4 — the one open ticket, and the experiment that decides it

**Bug:** `rmmod`/`modprobe` (warm start, no replug) → OBS gets frozen/black
frames.  A physical replug reboots the FX3 firmware and fixes it.  A module
reload does not reset the USB device, and the FX3 stays in a state where
EP 0x83 never produces data (soft-reload, "silent pipe").  The watchdog
(`avmcv710-usb.c: avm_watchdog`) currently only *flags* it
(`STREAMON with no EP 0x83 data` after ~5 s) — it does not recover.

**The decision experiment (user must run it — agent cannot sudo):**

```bash
# reproduce: free device, rmmod/modprobe, start OBS capture, no replug
#   -> frozen frames + watchdog warning

# find the CV710 and toggle USB authorised state (re-enumeration, no replug):
lsusb | grep 0710
ls /sys/bus/usb/devices/          # the 1-x / 3-x.2 entry for the CV710
echo 0 | sudo tee /sys/bus/usb/devices/<port>/authorized
sleep 2
echo 1 | sudo tee /sys/bus/usb/devices/<port>/authorized
# expected: port re-enumerates, avmcv710 re-binds, full bootstrap replays
# then: start OBS capture again and check for frames
```

Decision tree (implement based ONLY on the result):
- **Works** (frames return) → implement watchdog-triggered auto-recovery:
  `usb_reset_device()` from the watchdog work on the *silent* transition,
  behind a module param (default off), with a rate limit and a
  "recovered"/"giving up" journal line.  `usb_reset_device()` in *probe* is
  known-risky (probe recursion) — keep it out of probe.
- **Doesn't work** → document the divergence; leave watchdog-as-tripwire and
  suggest a udev/hotplug workaround.  Do not write risky code on a guess.

## 5. Kernel-safety contract (non-negotiable)

- Every parser index is bounds-checked against the transfer word count; use
  `guarded >= / min()` clamps — a `words - cur` underflow family of bugs was
  the original userspace crash (CV-23 rules).  Never assume a marker/header is
  fully in the current chunk.
- Exact-size frame gate: a C1 whose word count does not match the active
  mode's `target_words` is a corruption — **drop it, never repair**.
- Parsing happens ONLY in the workqueue (process context), never in the URB
  completion callback.  The completion just parks chunks on `drain_list`.
- vb2 buffers are ERROR-returned on every stop path; a *failed*
  `start_streaming()` returns them as **QUEUED** (else vb2 WARNs on
  `q->done_list`).
- URB teardown: anchor URBs, `usb_kill_anchored_urbs` then
  `flush_workqueue` then **kill again** (a worker can re-anchor+resubmit
  between the first kill and the flush).
- All EP1/I2C traffic serialized by `dev->ctrl_lock` (read/write are
  command+response pairs; interleaving desyncs and wedges the endpoint).
- `cancel_delayed_work_sync` on a self-requeuing work is safe
  (`WORK_OFFQ_DISABLE` during cancel, verified in
  `kernel/workqueue.c:__cancel_work_sync`) — still gate requeue on a
  "still running" flag where practical.
- Do not call `dev_info`/printk while holding a raw spinlock (log after
  dropping the lock or use `dev_*`.

## 6. Architecture cheat-sheet (don't re-derive this)

- **Wire protocol:** one bulk IN EP0x83 stream of 2,080,768-byte chunks
  (`AVM_CHUNK_BYTES`), framing markers `C0`/`C1` words; C1 = frame/field with
  a word count, audio interleaved with marker `0x58FFFF00`.  This is the same
  C0/C1 protocol as the userspace port — `cv710userspace` is the reference.
- **Bootstrap:** probe replays the vendor's 871-command trace over EP1
  (I2C writes to slaves 0x20/0x34/0x22/0x50 etc., incl. the 22× 256-byte EDID
  writes to 0x50).  First EP1 read after warm start can return a stale byte:
  drain EP1 before trusting the sequence.
- **Frame path:** FPGA passes the raster through (no scaling — resolution is
  NOT scalable).  Parser locks a mode via word-count (1080p 1036800, 1080i
  518400, 720p 460800, 576p 207360, 576i 103680, 480p 172800, 480i 86400) with
  4-frame hysteresis, weaves interlaced fields (`p->weave`), stamps
  timestamp+sequence (driver owns them), and calls `avm_deliver_frame()`.
- **Status poll (TODO-2):** while streaming, every 3rd ~33 ms tick reads
  IO 0x20/0x6A (TMDS lock `(v&0xE0)==0xE0 && v&0x10`), IO 0x20/0x12 (interlace
  bit 4), HDMI 0x34/0x07+0x09 (12-bit W/H), publishes `dev->sig` under
  `sig_lock`.  `avm_live_geometry()` (shared by `avm_pixfmt`) falls back to
  `dev->sig` before the first frame locks, then 1920x1080.
- **Overlays (TODO-3):** after 250 ms without a real frame the status work
  composites the 640x480 no-signal/out-of-range bitmap centered on a
  limited-range black raster at the live geometry and delivers it through the
  normal vb2 path.  (The vendor draws its splash host-side; a bare V4L2 node
  synthesizes it instead — documented divergence.)
- **Audio (TODO-1), opt-in `avm_audio=1`:** snd-usb-audio push model — parser
  copies 0x58 payload into the PCM DMA buffer, `pointer()` reports the write
  cursor, PCM flagged non-atomic, `snd_pcm_period_elapsed` fired from the
  parser worker.  Fixed S16LE stereo 48 kHz.  **Registration is off by
  default** (the card's missing `ops->prepare` oopsed this kernel — it calls
  prepare unconditionally — and collapsed the user's audio stack on first
  install; now provided).  The core maps the vmalloc PCM buffer itself, so
  no `.mmap`/`.page` ops and no vmalloc-helper imports.  Audio Infoframe
  rate parse is an open follow-up.
- **Converter (CVX8897):** opt-in `cvx8897=1`.  `avm_cvx8897_half_rate()` maps
  (w,h,fps) to a table; 1080p→60:30/50:25, 720p→same, other geometries
  60→30 only; canned for interlaced.  Programs I2C 7-bit 0x18 + PCA9539
  (0x74) reset pulse, sleeps ~3 s on its own workqueue.  RGB-vs-422 is
  assumed 422 (open follow-up: read AVI colorspace flag).
- **Watchdog:** 5 s delayed work flags STREAMON-with-no-EP0x83-data;
  baseline reset on every start_streaming.

## 7. Pitfalls learned the hard way (check these before touching related code)

- **Polling ADV7604 while the FPGA is idle wedges EP1.**  The old status
  worker was dropped for exactly this.  The rewritten one is armed only from
  `start_streaming()` (after `fpga_work_mode(true)`) and cancelled from
  `stop_streaming()`/`disconnect()`.
- **`vb2_fop_release`** on the open/close callbacks is the OBS freeze fix —
  do not switch back to `video_ioctl2`-style release without re-testing
  resolution switches mid-session.
- **URB kill must be done twice** around `flush_workqueue` (see §5).
- **`measured_fps`** must divide by `(cnt-1)` over a ~2.5 s window and round
  to nearest, or a 60 Hz source reports 61.
- **`V4L2_CAP_TIMEPERFRAME` aliases `V4L2_CAP_VIDEO_CAPTURE_MPLANE`**
  (both 0x1000) — never advertise it.
- **OBS renegotiates on `V4L2_EVENT_SOURCE_CHANGE`; a *failed*
  start_streaming must return buffers QUEUED** or the next STREAMON trips
  vb2 warnings.
- **Audio** init failure must stay non-fatal (video-only fallback), and
  `MODULE_SOFTDEP("pre: snd-pcm")` is required.

## 8. Verification workflow (how prior tickets were done)

1. Confirm the claim in `cv710userspace/` source (e.g. `UsbStream.cpp`,
   `lgxdevice.cpp`) — that, plus `driver_extracted/`, is the source of truth.
2. Write the change + a rationale paragraph in `docs/DEVLOG.md` under a dated
   `### YYYY-MM-DD` heading, update the parity matrix row and the TODO
   checkbox with its acceptance criterion.
3. Build (`make ... LLVM=1`), fix warnings; commit **one ticket per commit**.
4. Hand the install/test command block to the user (agent cannot sudo).
5. Only after the user's positive report: flip the matrix cell to ✅.

## 9. Open TODO list (see docs/DEVLOG.md for full acceptance text)

| # | Ticket | State |
|---|--------|-------|
| TODO-1 | ALSA capture PCM | built + committed `aa16708` + oops fix (opt-in `avm_audio=1`) — **user test pending** |
| TODO-2 | ADV7604 status poll | built + committed `482fed3`,`7ebb4b6` — **user test pending** |
| TODO-3 | No-signal/out-of-range overlays | built + committed `482fed3` — **user test pending** |
| TODO-4 | Soft-reload no output | **open** — needs user `authorized`-toggle experiment, then implement decision per §4 |
| — | 2026-10-12 bug-hunt fixes (8 bugs: unload pin, adaptive-mode dead delivery, sequence race, adapter shared scratch mode, URB fail-path leak, status-work pre-init cancel, bootstrap scratch guard, audio hw_free barrier) | fixed + committed; see `### 2026-10-12 (late)` devlog entry |
| — | Converter RGB-vs-422 (AVI colorspace flag) | open (P1 follow-up) |
| — | Audio Infoframe sample-rate parse (32/44.1 kHz) | open (P1 follow-up) |
| — | HDCP state log (doc-only) | open (P2, docs only) |
| — | Suspend/resume safe audio | open (P2) |

Current module identity (for regression diffing): `srcversion
47D0A3D5A3B5820DE03EF7B` (bug-hunt fixes, 2026-10-12 — see
`docs/DEVLOG.md` "### 2026-10-12 (late)"), parms `cvx8897`, `avm_audio`
(bool, default off), `avm_audio_index`, `avm_audio_id`,
`softdep: pre: snd-pcm`, 4×2 MB URBs.  `fix-driver.sh` expects this
srcversion and prints the `rmmod -f` escape hatch if an old build's stray
ref survives unbind.

## 10. Do / don't

- DO re-read `docs/DEVLOG.md` (dated RE entries) before touching hw/parser.
- DO keep every claim grounded in `cv710userspace/` or `driver_extracted/`.
- DO keep commits local (repo has not been pushed; ask the user before
  pushing).
- DON'T re-do completed RE (ADV7604 regs, CVX8897 tables, audio packet
  format, bounce/bootstrap ordering — all in the devlog).
- DON'T add EDID patching / VID_STD auto-switch / scaler emulation (all
  documented divergences).
- DON'T make the driver re-probe-in-probe (probe recursion) to "fix"
  TODO-4.