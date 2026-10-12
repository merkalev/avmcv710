# DEVLOG — avmcv710 parity with the official AVerMedia driver

Goal: make the kernel driver behave as close to the official ExtremeCap U3
(CV710) driver as possible, verified against the reverse-engineered vendor
code (Windows `libC877Driver.dylib`/`AVMU3.sys` trace, `libC877Driver.dylib`
i386 macOS i386 build) and the userspace port (`../cv710userspace/`), not
against guesses.

Workflow: every entry below is grounded in code in this repo or the userspace
repo.  The agent builds; **the user installs and tests** (sudo is not
available to the agent).  Kernel log: `journalctl -k -f`.

---

## Status (2026-10-12)

Working and user-confirmed:
- Capture at source geometry/rate; no latency/lag (4×2 MB URBs)
- No freeze on OBS res/fps changes (`vb2_fop_release`)
- Honest enumeration (one live size; measured interval, or the CVX8897 menu
  when enabled)
- Frame rate conversion (CVX8897), opt-in via `cvx8897=1`
- STREAMON no-data watchdog (soft-reload diagnosis)
- `measured_fps` no longer reports 61 for a 60 Hz source
- ALSA capture PCM (TODO-1): parser pushes 0x58 audio straight into the
  PCM DMA buffer (snd-usb-audio model), card id `AVMCV710`

Built, awaiting user install/test:
- ADV7604 status poll (TODO-2): 10 Hz while streaming, publishes `dev->sig`
  (TMDS lock / interlace / measured geometry)
- No-signal + out-of-range overlays (TODO-3): 640×480 bitmap composited into
  the current raster after an idle window, so no-signal is visible in OBS
  at the negotiated geometry

Not yet parity (see TODO):
- Soft-reload no-output root cause still open (watchdog confirms it happens;
  replug fixes)
- Converter RGB-vs-422 choice still assumes YCbCr 4:2:2 (needs AVI colorspace
  flag read)

> Handoff 2026-10-12 (end of session): agent stepped back until the user has
> installed + tested TODO-1/2/3 (J/K/L in the matrix) and run the TODO-4
> USB `authorized`-toggle experiment.  A new agent should read
> `docs/AGENTS.md` first — it is the full handoff (state, commands, RE facts,
> pitfalls, next steps).

---

## Parity matrix

| Capability | Official driver | avmcv710 kernel | Status |
| --- | --- | --- | --- |
| Video capture @ source rate | yes | yes | ✅ |
| Geometry resets passthrough | yes (Scaler off by default) | yes (never scales) | ✅ |
| Res/fps change without restart | yes | yes (V4L2_EVENT_SOURCE_CHANGE) | ✅ |
| "Video Input Frame Rate" 60/50/30/25 | CVX8897 | CVX8897, opt-in | ✅ |
| Audio S16LE stereo @ 48 kHz | yes (PCM) | ALSA capture PCM (`avmcv710-audio.c`) | ✅ (test pending) |
| No-signal overlay | yes (BMP splice) | 640×480 BMP composited at live geometry | ✅ (test pending) |
| Out-of-range overlay | yes | same | ✅ (test pending) |
| Content-protection overlay | yes | same (HDCP doc only) | 🟡 doc |
| Receiver status (lock/RGB/rate) | ~10 Hz poll | 10 Hz poll while streaming → `dev->sig` | ✅ (test pending) |
| Warm restart without replug | yes | **no (open bug)** | 🔴 TODO-4 |
| EDID upload / HPD handshake on start | yes (every open) | yes (in 871-cmd trace, 0x50 ×N) | ✅ |
| EP1 wedge recovery | drain + stall service | drain + toggle reset + retry | ✅ |

---

## RE findings (dated)

### 2026-10-11 — audio packet format (spec for Stage 3, TODO-1)

Source: `cv710userspace/src/liblgx/src/lgxdevice.cpp` (audio parse) +
`SdlAudioOutput.cpp` (sink format).

- Marker `0x58FFFF00` starts an audio frame; the next word is the length:
  `audioBytes = ((rawLen >> 16) & 0xFF) << 8 | ((rawLen >> 24) & 0xFF)`;
  valid range `512 .. 8192` bytes (4-byte aligned).
- Payload follows immediately (`audioWords = audioBytes / 4` words of raw
  bytes), possibly split across URB chunks; simple continuation state
  (`_inAudio`, `_remainingAudioWords`).
- After the payload: `0xAA5555AA` padding until the next marker.
- Sink: **S16LE, stereo, source sample rate (default 48000)** —
  `SDL_AUDIO_S16LE`, `channels = 2`, `freq = _currentSampleRate (48 kHz)`.
- Decision: audio packets were already removed while parsing (padding or to a
  contiguous capture buffer).  Kernel parser currently *skips* them for raster
  alignment; Stage 3 copies them into an ALSA S16LE stereo capture PCM.

### 2026-10-11 — ADV7604 status poll register set (spec for TODO-2)

Source: `UsbStream::queryVideoSignalStatus()` (verified against
`drivers/media/i2c/adv7604.c`).  This is read-only telemetry except where
noted.

- Core (discard snapshot on failure, keep last good):
  - IO 0x20/0x6A — TMDS lock: `(v & 0xE0) == 0xE0 && (v & 0x10)` 
  - IO 0x20/0x12 — STDI, interlaced = bit 4
  - HDMI 0x34/0x07 — 12-bit active width (mask 0x0FFF across 2 bytes)
  - HDMI 0x34/0x09 — 12-bit active height
- Optional (a failure degrades one field, not the snapshot):
  - HDMI 0x34/0x20,0x22,0x24,0x2A,0x2E,0x32 — porches (timings)
  - HDMI 0x34/0x05 — HDMI mode; 0x06/0x3B — pixel clock; 0x0B — deep color
  - HDMI 0x34/0x53 — colorspace byte
  - HDMI 0x34/0x04 — audio status (bit 0 PLL lock); 0x34/0x18 — audio freq
  - IO 0x20/0x6F — cable detect; 0x20/0x01 — VID_STD; 0x20/0x60 — infoframe
    presence (bit 1 = audio infoframe)
  - CP 0x22/0xB1 — STDI blanking length; 0x22/0xB3 — line count (read-only
    cross-check vs the parser's word-count mode table)
- Sample rate: HDMI 0x34/0x18 bit 0 + infoframe payload byte 1 bits 4:2
  (registers are stale after reconnect; parse the infoframe when present).
- Poll cadence: wake 50 ms, poll ~10 Hz.  **Only while streaming, after
  setFpgaWork** — polling from probe wedged EP1 (that exact foot-gun is what
  the WIP status debugged around).

### 2026-10-12 — status worker rebuilt (TODO-2 + TODO-3)

Recovered the dropped worker (`git show a7faa76:avmcv710-status.c`) and
reconciled it with the current tree (audio + cvx live, watchdog live, URBs
4×2 MB):

- Worker lifecycle moved from "armed in init" (the EP1-wedging bug) to
  streaming-only: `avm_status_start()` is called at the end of
  `start_streaming()` (**after** `avm_reset_stream_dma` +
  `avm_fpga_work_mode(true)`, mirroring `setFpgaWork`); `avm_status_stop()`
  (`cancel_delayed_work_sync`) runs at the top of `stop_streaming()` and in
  `disconnect()`.  The work checks `streaming` at entry and re-arms itself
  only while it stays true; `cancel_delayed_work_sync` neutralizes that
  requeue (WORK_OFFQ_DISABLE during cancel, verified in
  `kernel/workqueue.c:__cancel_work_sync`).
- Overlay delivery: the raw 640×480 BMP is *not* handed to the queue as-is
  (it would shear in a 1080p consumer).  It is composited centred on a
  limited-range black raster at the live geometry
  (`avm_live_geometry()`, shared with `avm_pixfmt()`), cached by
  (kind, w, h), then delivered through the parser's normal
  `avm_deliver_frame()` path (payload/timestamp/sequence).  1×4.6 MB
  `splash_frame` scratch allocated at probe, freed at release.
- The vendor draws its splash host-side in the renderer; a bare V4L2 node
  has no renderer, so synthesizing the frame is the closest faithful
  behaviour (documented divergence).
- `dev->sig` is now live (was invalid since Stage 0): the parser fallback
  geometry is fed from the polled measurement before the first frame locks
  (`avm_live_geometry`), and the converter already consumes
  `measured_fps`/`active_interlaced`.  The RGB-vs-422 table choice remains
  the one poll-driven follow-up.

### 2026-10-11 — `.inf` "device parameter" keys ruled out

`avmu3c877_x64.inf` service params (`ENABLE_SCALER=1`, `SCALER_OP_MODE=2`,
`VIDEO_IN_QUEUE=0x30`, `AUDIO_IN_QUEUE=0x19`, `LATENCY=0x0053EC60`) are Windows
registry knobs for the vendor's user-mode stack (queue depths, latency
tuning) — **not FX3/EP1 commands**.  Nothing to port to the kernel.

### 2026-10-11 — warm-start semantics (soft-reload bug, TODO-4)

- The userspace replays the **full bootstrap on every open** ("re-uploads the
  256-byte EDID and drives the HPD handshake"; skipping it on warm start
  left stale mode lists until a physical replug).  Our probe replays the same
  871-command trace (slave 0x50 EDID writes present, ×22), so that part is
  covered.
- Sound: `setVideoInput(HDMI)` writes IO 0x20 {0x00→0x08, 0x01→0x06,
  0x02→0xFC, 0x05→0x2C} (power-on / HDMI auto-detect / eq) once at startup.
- Stream start order — ours matches exactly:
  `queueFrameRead (queue URBs) → sendResetStreamDma → setFpgaWork (0x10/0x05
  bit1)`.
- Stream end order: stop control poll → `setFpgaIdle → sendResetStreamDma →
  cancel transfers` (we kill URBs, set idle, reset DMA).
- **Open question**: a physical replug reboots the FX3 firmware; a module
  reload does not, and a soft reload can leave the pipe silent (watchdog
  confirmed: URBs submitted, zero completions, no endpoint error).  On
  Windows a driver reload is accompanied by a USB-level re-enumeration; on
  Linux `rmmod/modprobe` re-runs probe on the *already enumerated* device —
  no USB reset.  Candidate: issue `usb_reset_device()` on probe when the
  previous session left the device in a bad state (or make the retry path
  escalate EP1 drain → toggle reset → USB reset).  Needs a reproduction +
  log before/after.

---

## TODO

### P0 — official-driver behavior OBS users actually hit

- [x] **TODO-1 ALSA capture PCM (audio)** — Stage 3.  Done in
  `avmcv710-audio.c` (+ parser demux + Makefile): snd-usb-audio style sink —
  the parser copies 0x58 payload bytes straight into the PCM DMA buffer and
  `pointer()` reports the write cursor, so mmap and RW both work with no
  `copy_user` op; `snd_pcm_period_elapsed` is fired from the parser worker
  (PCM marked non-atomic).  Card id `AVMCV710`, S16LE stereo 48 kHz, 1 MB max
  buffer, `index`/`id` module params, `MODULE_SOFTDEP("pre: snd-pcm")`.
  Audio init is non-fatal (video-only fallback on failure).
  Test: `arecord -D hw:CARD=AVMCV710 -f S16_LE -r 48000 -c 2 -d 10 /tmp/a.wav`
  while OBS records; `/proc/asound/cards` should list AVMCV710.
  Open follow-up: sample rate is fixed 48 kHz; parse the Audio Infoframe
  (0x34/0x18 bit 0 + infoframe byte 1 bits 4:2) to honour 32/44.1 kHz sources.
- [ ] **TODO-4 soft-reload no output** — close the loop with the USB-reset
  hypothesis; keep the watchdog as the tripwire.  Acceptance: rmmod/modprobe
  then start OBS capture without replugging — frames arrive.

### P1 — correctness of what we already have

- [x] **TODO-2 ADV7604 status poll (Stage 2)** — `avmcv710-status.c` (recovered
  and rebuilt from git history; the dropped `a7faa76` worker).  The 10 Hz
  poll runs *only while streaming*: the delayed work is armed by
  `avm_status_start()` at the end of `start_streaming()` (after FPGA work
  mode) and cancelled by `avm_status_stop()` in `stop_streaming()` /
  `disconnect()` — polling an idle FX3 was what wedged EP1 in the earlier
  adaptive-geometry attempt.  Publishes `dev->sig` (TMDS lock / interlace /
  measured 12-bit geometry) under `sig_lock`; `dev->sig` already feeds the
  parser fallback geometry, and interlace/`measured_fps` feed the converter.
  Acceptance: `journalctl -k` shows the ADV7604 lock, and pulling the HDMI
  cable flips the overlay (below).
- [x] **TODO-3 overlays** — same file: when streaming and no real frame has
  arrived for a 250 ms idle window, the work composites the 640×480
  no-signal / out-of-range bitmap (generated `avmcv710-bmp.h`) centred on a
  limited-range black raster at the *live* geometry (`avm_live_geometry()`,
  shared with `avm_pixfmt()` so consumers see the size they negotiated) and
  delivers it via `avm_deliver_frame()` — paced at the ~30 Hz tick, cached by
  (kind, w, h), cleared the moment real frames resume.  Acceptance: pulling
  the cable during OBS capture shows the AVerMedia-style "no signal" splash
  instead of a frozen frame.
- [ ] Converter: read the ADV7604 AVI colorspace flag so a 720p60 RGB source
  gets the vendor's dedicated RGB table instead of the YCbCr 4:2:2 table
  (interlace + input rate are already polled/measured).
- [ ] Audio: parse the HDMI Audio Infoframe (0x34/0x18 bit 0 + infoframe
  payload byte 1 bits 4:2) and drive the ALSA PCM clock for 32/44.1 kHz
  sources (PCM is currently fixed at 48 kHz).

### P2 — polish

- [ ] Overlay pump (needs TODO-2): splice 640×480 BMP (generated
  `avmcv710-bmp.h` already tracked locally) into the stream after
  idle-window, mirroring standby behavior.
- [ ] Wire `SNDRV_PCM_INFO_RESUME`-safe suspend/resume for audio.
- [ ] HDCP: keep documentation-only, but log the ADV7604 HDCP state read.

### Never (documented divergence)

- EDID patching / VID_STD auto-switch (receiver is input-capability passthrough).
- FPGA scaler enable (vendor `ENABLE_SCALER=1` resizes; we keep the raster).

---

## Perf / measurement notes

- `61 fps` in OBS was the G_PARM counter counting `fps_cnt` frames over the
  window when the first counted frame only started the clock
  (`f4c24ad`): window ~2.5 s, divide by `fps_cnt - 1`, round to nearest,
  ignore ±1 flaps.  Delivery was always exactly 1 frame/C1.
- Latency: 4 URBs ≈ 1 frame of pre-buffer slack (~33 ms); ≥6 restored latency.