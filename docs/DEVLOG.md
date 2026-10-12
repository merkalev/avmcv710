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

## Status (2026-10-11)

Working and user-confirmed:
- Capture at source geometry/rate; no latency/lag (4×2 MB URBs)
- No freeze on OBS res/fps changes (`vb2_fop_release`)
- Honest enumeration (one live size; measured interval, or the CVX8897 menu
  when enabled)
- Frame rate conversion (CVX8897), opt-in via `cvx8897=1`
- STREAMON no-data watchdog (soft-reload diagnosis)
- `measured_fps` no longer reports 61 for a 60 Hz source

Not yet parity (see TODO):
- No ADV7604 status poll / overlays (WIP exists at
  `/tmp/opencode/avmcv710-status-wip.c`, uncommitted)
- Soft-reload no-output root cause still open (watchdog confirms it happens;
  replug fixes)

---

## Parity matrix

| Capability | Official driver | avmcv710 kernel | Status |
| --- | --- | --- | --- |
| Video capture @ source rate | yes | yes | ✅ |
| Geometry resets passthrough | yes (Scaler off by default) | yes (never scales) | ✅ |
| Res/fps change without restart | yes | yes (V4L2_EVENT_SOURCE_CHANGE) | ✅ |
| "Video Input Frame Rate" 60/50/30/25 | CVX8897 | CVX8897, opt-in | ✅ |
| Audio S16LE stereo @ 48 kHz | yes (PCM) | ALSA capture PCM (`avmcv710-audio.c`) | ✅ (test pending) |
| No-signal overlay | yes (BMP splice) | bmp header generated, unused | 🔴 TODO-3 |
| Out-of-range overlay | yes | same | 🔴 TODO-3 |
| Content-protection overlay | yes | same (HDCP doc only) | 🟡 doc |
| Receiver status (lock/RGB/rate) | ~10 Hz poll | none (word-count fallback) | 🔴 TODO-2 |
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
  Open follow-up: sample rate is fixed 48 kHz; parse the Audio Infoframe in
  TODO-2 to honour 32/44.1 kHz sources.
- [ ] **TODO-4 soft-reload no output** — close the loop with the USB-reset
  hypothesis; keep the watchdog as the tripwire.  Acceptance: rmmod/modprobe
  then start OBS capture without replugging — frames arrive.

### P1 — correctness of what we already have

- [ ] **TODO-2 ADV7604 status poll (Stage 2)** — port the WIP status worker
  (`/tmp/opencode/avmcv710-status-wip.c`): 10 Hz poll while streaming,
  publish `dev->sig`, feed the converter's input rate/RGB/interlace decision,
  and drive no-signal/out-of-range splash.  Acceptance: `cat
  /sys/…/format`-equivalent telemetry + overlays appear when the cable is
  pulled; converter picks the RGB table for a 720p60 RGB source.
- [ ] Converter: use polled RGB/interlace instead of the 422 assumption.

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