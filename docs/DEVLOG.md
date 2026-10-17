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

## Status (2026-10-13)

Working and user-confirmed:
- Capture at source geometry/rate; no latency/lag (4×2 MB URBs)
- No freeze on OBS res/fps changes (`vb2_fop_release`)
- Honest enumeration (live size first; FPS menu = {native, native/2}, half only
  when the CVX8897 converter is enabled)
- Frame rate conversion (CVX8897), opt-in via `cvx8897=1`
- STREAMON no-data watchdog (soft-reload diagnosis)
- `measured_fps` no longer reports 61 for a 60 Hz source
- **ALSA capture PCM (TODO-1)** — verified working flawlessly on hardware (2026-10-16):
  S16LE stereo at 32/44.1/48 kHz, oversize packet ring-handling, TMDS unlock silence gating,
  and standalone pipeline refcounting (both standalone `arecord` and combined OBS capture).
- Continuous inter-frame audio demux (packets between C1 and C0).

Built, awaiting user install/test:
- ADV7604 status poll (TODO-2): 1 Hz while streaming, publishes `dev->sig`
  (TMDS lock / interlace / measured geometry / colorspace / RGB)
- Standby overlays (TODO-3, **restored 2026-10-16**): the four vendor
  `aver_custom_*.bmp` bitmaps (no-signal / out-of-range / HDCP /
  content-protection) are composited at the negotiated geometry and delivered
  through the normal frame path when the input is absent, unsupported or
  encrypted - and when a consumer negotiates a non-native geometry the FPGA
  cannot scale to (the vendor's "Signal Out of Range" case).  Stored
  run-length encoded by `tools/gen_bmp.py` (~7x smaller than the old header).
- Full-range quantization auto-detection (prevents crushed blacks in OBS when
  HDMI 0x53 reports full range)
- CVX8897 720p60 dedicated RGB half-rate table auto-selection via ADV7604 colorspace
- EP1 stack DMA bounce buffers (`CONFIG_VMAP_STACK=y` safety)
- Endpoint stall (-EPIPE) recovery via `usb_clear_halt`
- Auto-reset USB watchdog parameter (`auto_reset=1` for TODO-4 recovery)

---

## Parity matrix

| Capability | Official driver | avmcv710 kernel | Status |
| --- | --- | --- | --- |
| Video capture @ source rate | yes | yes | ✅ |
| Geometry resets passthrough | yes (Scaler off by default) | yes (never scales) | ✅ |
| Short/partial frame (re-lock) | kept, black-padded `0x80108010` | kept, black-padded (`AVM_PAD_WORD`) | ✅ (test pending) |
| Res/fps change without restart | yes | yes (V4L2_EVENT_SOURCE_CHANGE) | ✅ |
| "Video Input Frame Rate" 60/50/30/25 | CVX8897 | CVX8897, opt-in; menu = {native, native/2} only | ✅ |
| Audio S16LE stereo @ 32/44.1/48 kHz | yes (PCM) | ALSA capture PCM (`avmcv710-audio.c`), opt-in `avm_audio=1`, sink pinned to the source rate from the Audio InfoFrame; standalone capture supported via refcounted pipeline | ✅ (hardware verified) |
| No-signal overlay | yes (BMP splice) | 640x480 BMP (macOS rendition) composited at live geometry, RLE-stored (2026-10-16) | ✅ (test pending) |
| Out-of-range overlay | yes | receiver-driven **and** non-native S_FMT request (`avm_req_mismatch`), 2026-10-16 | ✅ (test pending) |
| Content-protection overlay | yes | compiled in (`aver_custom_content_protection.bmp`); trigger not wired (HDCP doc only) | 🟡 doc |
| Receiver status (lock/RGB/rate) | ~10 Hz poll | 1 Hz poll while streaming → `dev->sig` | ✅ (test pending) |
| Warm restart without replug | yes | watchdog diagnosis; opt-in `auto_reset=1` | 🟡 TODO-4 |
| Clean unload (rmmod without replug) | n/a | unbind drops the ALSA pin in disconnect; rmmod succeeds | ✅ (fixed 2026-10-12) |
| EDID upload / HPD handshake on start | yes (every open) | yes: 871-cmd trace, 256 B on bridge id 0x36, byte-identical to the vendor binaries' EDID | ✅ |
| Bootstrap sequence reverse-engineered | yes | 100% of opcodes mapped (Atmel AT88SC CryptoMemory auth, PCA9539, CVX8897 bus 1, ADV7604, EDID, Lattice ECP3); see `docs/BOOTSTRAP.md` & `docs/BOOTSTRAP_I2C.md` | ✅ |
| Standalone ALSA audio capture | yes | Hardware streaming refcounted across video and audio (`avm_pipeline_start`/`stop`); arecord captures without video running | ✅ (hardware verified) |
| EP1 wedge recovery | drain + stall service | drain + toggle reset + retry + bounce bufs | ✅ |

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

### 2026-10-12 (PM) — ALSA card oops: mandatory `ops->prepare` + opt-in gate

Symptom: after installing the TODO-1/2/3 module, "everything broken" —
OBS balked, videos stopped playing, user had to restart PipeWire.  Journal
evidence: probe was clean (`registered video0`, `ALSA capture PCM
registered`, `ready, 4 x 2080768 byte URBs`), then immediately:

```
BUG: kernel NULL pointer dereference, address: 0000000000000000
#PF: supervisor instruction fetch in kernel mode
RIP: 0010:0x0
... Comm: wireplumber ...
snd_pcm_do_prepare+0x72 ... snd_pcm_action_nonatomic+0x98 ...
```

~4 min later WirePlumber died in `stop-sigterm` (killed) — the audio stack
collapse behind the user-visible breakage.

Root cause: **`snd_pcm_do_prepare()` calls `substream->ops->prepare()`
unconditionally** in this kernel (no NULL check — confirmed against
`sound/core/pcm_native.c`).  `avm_audio_ops` had no `.prepare`, so
WirePlumber's open+prep of the new capture PCM (it prepares every device to
enumerate it) executed a NULL function pointer: supervisor instruction fetch
at 0x0.  The old NULL-checked variant of that helper is long gone, which is
why the audio TODO shipped a latent oops.

Fix (`avmcv710-audio.c`):
- Added `avm_audio_pcm_prepare()` (returns 0 — hw_params pins the format and
  the push path per-packet validates the runtime), wired as `.prepare`.
- Audited every other `ops->` call in `snd_pcm_do_*`: `snd_pcm_do_start`/
  `snd_pcm_do_stop` need `.trigger` (we have it); `snd_pcm_ops_ioctl` falls
  back to `snd_pcm_lib_ioctl`; `.mmap`/`.page` are optional — for
  `SNDRV_DMA_TYPE_VMALLOC` buffers `hw_support_mmap()` returns true and the
  data area is mapped by the core via memalloc' `snd_dma_vmalloc_ops` (no
  driver op needed, and the old `snd_pcm_lib_mmap_vmalloc` exports are gone
  from pcm_memory.c — do not import them).
- Registration is now **opt-in**: `avm_audio=1` module param, **default 0**
  (off).  With it off the module is exactly the previously stable video-only
  card, and nothing about the sink can affect userspace until it is
  deliberately enabled and verified (same policy as `cvx8897`).
  `avm_audio_push()` already no-ops on `dev->audio == NULL`; `fini()` on
  `NULL` is a guard early-return.  `avm_audio_index`/`avm_audio_id` are inert
  while disabled.

### 2026-10-12 (late) — full driver bug hunt: 8 confirmed bugs, fixes landed

Directive: "go hunt for all problems in the driver."  The recovery pass for
the ALSA oops surfaced a *second* unload defect, then the audit found six
more.  All eight fixed in this session (see module identity
`47D0A3D5A3B5820DE03EF7B`).

**Unload/pin bugs (the reason `rmmod` kept failing):**

1. **`avm_usb_disconnect()` never freed the ALSA card.**  `avm_audio_fini()`
   only ran from `avm_vdev_release()` (fired on the *last video-node fd
   close*), so an unbind that merely unregistered the video node left the
   registered snd card pinning the module — `rmmod` failed with no way out.
   The old build freed the card in disconnect (why the card vanished on the
   user's unbind while the module stayed pinned by a stray ref).  Fix:
   `avm_audio_fini()` in disconnect, after the parser flush (no in-flight
   0x58 push can race `a->dying`); a still-open arecord keeps the card (and
   module) alive via `snd_card_free_when_closed` — the correct contract.
2. **`usb_set_intfdata(intf, dev)` before the fallible steps.**  A probe
   failure after that left intfdata pointing at a freed `dev`; the affected
   error path (or a later unbind) would dereference it.  Fix: set intfdata
   only after every fallible step succeeded (nothing before it reads it).
3. **`avm_init_urbs` fail loop leaked the failing index's 2 MB `parse_buf`.**
   `while (i--)` freed indices `i-1..0`, skipping `i` — the entry whose
   `kmalloc`/`usb_alloc_urb` just failed (parse_buf allocated, urb NULL).
   Fix: `for (; i >= 0; i--)`, inclusive.
4. **`avm_status_fini()` cancelled a zeroed `status_work` on pre-init probe
   failures** (bootstrap/CVX failing before `avm_status_init`).  With
   `CONFIG_DEBUG_OBJECTS_WORK` that is a WARN on a kzalloc'd work struct.
   Fix: `dev->status_inited` set in init, early-return in fini.

**Data-path races (kernel-safety contract):**

5. **Adaptive-mode frames were always dropped.**  `avm_detect_mode()`'s two
   tolerant branches accepted a frame within ±2% of the measurement but set
   `m->target = expected`; the CV-23 exact-size gate then compared
   `frame_words != mode->target` against the observed count, which almost
   never equals `expected` for a non-table timing → every 1366x768 /
   1280x1024 / VGA-style source hit `corrupt+drop` and was never delivered.
   Fix: the tolerant branches set `m->target = words` (the exact-derivation
   interlaced branch is already `target == words` by construction).  Frame
   accumulation is clamped to `AVM_MAX_FRAME_WORDS`, so an oversized
   adaptive geometry degrades to a drop, never an overrun.
6. **`p->frame_sequence++` data race.**  Parser worker and status (splash)
   worker both deliver through `dev->vb_bufs`; the increment lived outside
   the `urbs_lock` critical section that pops the buffer → two concurrent
   delivers could stamp duplicate sequence numbers on DQBUF frames.  Fix:
   advance inside the `if (buf)` lock region.
7. **Global `static struct avm_mode avm_meas_mode` raced across adapters.**
   Two CV710s on one host shared one scratch mode, so a concurrent frame on
   the other adapter could corrupt a mode before `avm_handle_frame_end()`
   gated on it.  Fix: per-device `dev->parser.meas`.
8. **Plain writes to `active_w/h/interlaced/mode_serial` vs READ_ONCE
   readers.**  `avm_live_geometry()` (ioctl) and the cvx8897 worker read
   these with READ_ONCE (runs on other CPUs); the parser published them with
   plain stores.  Fix: WRITE_ONCE on the publishes (and READ_ONCE on the
   hysteresis comparison).

**Hardening (latent, no current trigger):**

9. `avm_bootstrap()` fed the 2048-byte scratch with `c->len` unbounded; a
   regenerated trace with an oversized write/read entry would overrun it
   (today's table max is 512).  Fix: WARN+`-EINVAL` guard before dispatch.
10. **Audio UAF window:** `avm_audio_push()` copies into `rt->dma_area`
    under `a->lock`, but nothing serialized against the core freeing that
    buffer on app close (`snd_pcm_hw_free` runs outside our lock).  Fix:
    `avm_audio_pcm_hw_free()` taking `a->lock` as a barrier — the buffer
    cannot be freed while a push is mid-memcpy.

**Left alone (reported, not changed):** the C1 clock-recovery branch consumes
2 of its 3 trailer words without a parity check against the userspace code;
the `frame_words >= 40000` arbitrary floor; the adaptive-interlaced
`> stride*field_lines` weave drop.  All are conservative (drop-only) and
the userspace port has the same shape.

**Recovery tooling:** `fix-driver.sh` now kills OBS *before* unbinding
(OBS retries its source and can grab /dev/video0 mid-flight, deferring the
vdev release), prints `/sys/module/avmcv710/refcnt` while retrying rmmod,
and — when the old build's invisible stray ref survives — prints the
deliberate `sudo rmmod -f avmcv710 && sudo modprobe avmcv710` escape hatch
instead of failing silently.

### 2026-10-16 (night) — audio push fixes; bootstrap blob analysed

**Audio (`avmcv710-audio.c`, build only, not hardware-tested).**
- A packet larger than the negotiated ring (packets go up to 8192 B,
  `period_bytes_min` is 1920) was dropped whole, so a small-buffer client
  heard nothing.  The newest `size` bytes are now kept and the write pointer
  still advances by the full amount.
- While the receiver positively reports TMDS unlocked, the 0x58 payload is
  replaced by silence instead of being copied.  The userspace driver gates
  the same bus noise (`cv710userspace/ISSUES.md` section 4).  Silence, not a
  drop, so the PCM pointer keeps its timing.  Before the first poll
  (`sig.valid` false) audio passes.
- Known gap, not changed: audio is only produced while a video capture holds
  the stream open (URBs and FPGA work mode are started from
  `avm_start_streaming()`), so a standalone `arecord` hears nothing.
  Fixing it means starting the pipeline from PCM open/trigger and sharing it
  with video via a refcount; that is a larger change and wants hardware
  testing first.

### 2026-10-16 (midnight) — reverse-engineering the bootstrap 'goop' & standalone ALSA audio streaming

**1. Bootstrap 'goop' fully reverse-engineered (`docs/BOOTSTRAP.md` and `docs/BOOTSTRAP_I2C.md`):**
- Disassembled `libC877Driver.dylib` (32-bit i386 macOS binary with full symbols intact).
- Identified the entire Cypress FX3 bulk EP1 opcode command set (`Fx3Firmware::sendCommand`):
  - `0x01` / `0x02`: I2C Bus 0 Write / Read (ADV7604, EDID, PCA9539, FPGA)
  - `0x03` / `0x04`: I2C Bus 1 Write / Read (CVX8897 exclusively)
  - `0x05` / `0x06` / `0x07`: Atmel AT88 CryptoMemory SendCmd, RevData, SendData
  - `0x08` / `0x09`: Atmel AT88 PowerOn, WaitClock
  - `0x0D` / `0x0E`: FX3 SPI Flash Write / Read
  - `0x0F` / `0x10`: FX3 SPI Flash Sector / Chip Erase
  - `0x14`: FX3 resetStreamDMA
- The mysterious ~268 bytes of high-entropy "goop" payload: it is the hardware copy-protection authentication challenge-response and crypto-fuse initialization exchanged between the FX3 microcontroller and an on-board Atmel AT88SC CryptoAuthentication chip (`CryptoAT88::CustomizeHWInit` / `Device::CryptoCheckSecureID`).
- Documented all 7 execution phases of the 871-command sequence and created a full register reference (`docs/BOOTSTRAP_I2C.md`) for all 696 I2C writes.

**2. Standalone ALSA audio streaming (`avmcv710-audio.c`, `avmcv710-video.c`, `avmcv710-usb.c`, `avmcv710.h`):**
- Replaced monolithic `avm_start_streaming` / `avm_stop_streaming` with refcounted `avm_pipeline_start(dev, is_video)` and `avm_pipeline_stop(dev, is_video)` under `dev->stream_lock`.
- `snd_pcm` is configured with `pcm->nonatomic = 1`, so ALSA ops (`.trigger`, `.close`) run in sleepable process context and safely acquire `mutex_lock(&dev->stream_lock)`.
- When `arecord` runs standalone (`SNDRV_PCM_TRIGGER_START`), it starts the hardware pipeline (URBs, FX3 GPIF DMA reset, FPGA work mode, status worker). The parser demuxes 0x58 audio packets and delivers them into the ALSA DMA ring buffer.
- When both video and audio are streaming, the hardware pipeline stays alive until *both* consumers have stopped.
- Splash overlay worker (`avm_status_pump_splash`) gated on `dev->video_streaming` so it skips compositing/logging overlays during audio-only capture.
- Module compiled with `srcversion 72336FB6B7466E80CF2FEAA`.

### 2026-10-16 (late) — deep bug scan (4 passes), DKMS, VIDIOC_LOG_STATUS, soft_unbind

Audit across `avmcv710-parser.c`, `-status.c`, `-video.c`, `-cvx8897.c`,
`-audio.c`, `-usb.c` (`srcversion 337EE7D6B3550756C3CFB71`).  Descriptions
below were checked against the commit diffs.

**Parser (`avmcv710-parser.c`)**

1. **`fields_woven` not cleared on non-woven exits.**  It was reset only on a
   clean trailer.  A dropped/corrupt/mismatched field, a mode change or a bad
   C1 left a stale half-frame count, so the next field could pair with a field
   of a different frame.  Now cleared on every drop path, on mode lock, on a
   bad-trailer C1 and after delivery.
2. **Fallback mode table keyed on width.**  `switch (m->w)` collided: 720 is
   the width of both 576 and 480 lines, so 480p/480i were unreachable.  The
   table is now keyed on height.  The interlaced receiver-derived fallback now
   computes field words from the field height (`sig.h` is already field height
   when `<= 700`).
3. **Adaptive `m->target` was `words`, not the nominal count.**  That made
   `avm_reconcile_frame()` a no-op for non-table timings, so short/long frames
   skipped pad/truncate.  Now `(u32)expected`.
4. **Receiver-width cross-check** in the fallback table: a locked receiver
   whose width contradicts the candidate mode rejects it (partial frames of a
   resolution switch could match another mode's word window).
5. **False `0x58FFFF00` marker inside video.**  When the following length
   word failed audio validation, the word was *counted* (`frame_words++`) but
   never copied, shifting the rest of the raster by one word.  It is now
   stored as pixel data.
6. **`avm_parser_reset()`**: `mode_serial` stays monotonic (a new stream can
   no longer alias `cvx_prog_serial`), and `clock_recovering`,
   `active_interlaced`, `target_words` are cleared.

**Status / video**

7. **Status worker backoff** no longer stretches the 33 ms tick (which also
   pumps overlays) after repeated poll failures; only the ADV7604 poll backs
   off.
8. **`avm_signal_ok()` / `avm_live_geometry()` consistency.**  Both reject odd
   widths, and both bound the *woven* frame (`h*2` for interlaced `h <= 700`)
   against `AVM_MAX_FRAME_WORDS` (pass 4 fixed `avm_signal_ok()`, which
   previously used the un-doubled height).  Oversized input reports 1920x1080
   plus the out-of-range overlay.
9. **`ENUM_FRAMESIZES`** skips table entries equal to the live size instead of
   emitting a duplicate; **`ENUM_FRAMEINTERVALS`** offers native/2 only when a
   CVX8897 table exists for that size/interlace.
10. `G_FMT` now records the reported format in `dev->fmt`.
11. `VIDIOC_LOG_STATUS` (`avm_log_status`) dumps stream, ADV7604, CVX8897 and
    parser counters to the kernel log (`v4l2-ctl --log-status`).

**Teardown / concurrency**

12. **Watchdog `auto_reset`** now uses `usb_queue_reset_device(dev->intf)`
    instead of calling `usb_reset_device()` from a work item.
13. **`cvx_work` vs `STREAMOFF`:** `avm_stop_streaming()` cancels it and the
    251-register upload checks `streaming` between writes.
14. **`avm_audio_pcm_open()`** refuses with `-ENODEV` once `a->dying`.
15. **`avm_usb_disconnect()`** parks the FPGA / resets stream DMA only if a
    stream was active and the device is still attached (an idle or unplugged
    device has nothing to park; the commands would just time out), and the
    driver sets `.soft_unbind = 1`.

**Build**

16. `Makefile` auto-detects a clang-built kernel (`LLVM=1`), `dkms.conf` plus
    `make dkms-install` / `dkms-remove`, `fix-driver.sh` no longer forces
    `LLVM=1`.

### 2026-10-16: documentation, READMEs renovated, legal posture recorded

Not a driver change.  Both READMEs were rewritten, and the HDCP ticket was
closed as a policy decision.

**READMEs.**  `avmcv710/README.md` had drifted from HEAD: it still described
the userspace CV-23 "exact size or drop" gate (PARSER-PAD replaced that),
advertised the vendor's 60/50/30/25/24/15 frame-interval menu (FPS-MENU cut it
to `{native, native/2}`), and called audio on-by-default 48 kHz (it is opt-in
and 32/44.1/48 kHz).  Missing entirely: the restored overlays, the non-native
out-of-range path and the 1 Hz receiver poll.  The licence note also called
`cv710userspace` GPL; it is MIT.  Both READMEs now lead with a
not-affiliated-with-AVerMedia notice, carry a contents list, and drop the old
bloat; the kernel one gained a module-parameter table, and the userspace
README's `--convert` wall of prose became a short CVX8897 section.

**Legal posture**, stated in both READMEs: no AVerMedia affiliation;
reverse-engineered for interoperability, no vendor source; no HDCP (no
authentication, no key material, no cipher, no key derivation) and no
circumvention; compliance with capture/recording law is the user's; the
overlay bitmaps are AVerMedia artwork; no warranty.

**HDCP ticket closed.**  The P2 "keep documentation-only, but log the ADV7604
HDCP state read" is now won't-do: reading the HDCP map is exactly the
interrogation the project declines (see the README *Legal* section and
`cv710userspace/ISSUES.md` section 6).  The `AVM_SPLASH_HDCP` /
content-protection bitmaps stay compiled in but unreachable.

### 2026-10-16 — overlay presentation: scale to the frame, macOS artwork

Two corrections to the restored overlays, both from user feedback ("had to
close OBS for YouTube to play" is unrelated; "windows driver is higher res
but no branding, macOS has branding" is the one that matters here).

**1. The overlay is a full-frame design and must be scaled.**  Both vendor
bitmaps are exactly 640x480x24 (neither is "higher resolution" than the
other — the Windows *design* just draws a bigger panel inside the same
canvas).  Crucially, neither binary embeds or even names the `.bmp` files:
they are host-application assets, and the macOS rendition puts the AVerMedia
logo in the *corner of the 640x480 canvas*, which only makes sense if the
whole canvas is presented scaled.  The first cut blitted it 1:1 centred, so
at 1080p the card covered a ninth of the frame and the logo floated
mid-screen.  `avm_splash_compose()` now scales uniformly to fit ("contain",
black letterbox, bilinear luma, nearest-macropixel chroma) — at 1080p that is
a 1440x1080 card centred with 240px bars, and the logo lands at the corner of
the card.  This is cached by geometry, so it is computed once, not per frame.

**2. Source = the macOS rendition** (`cv710userspace/assets/`), because it is
the branded presentation and lives in-tree.  The Windows copy
(`driver_extracted/win/`) is still one argument away; it is also dirtier (a
dithered gradient vs a flat panel), so it RLEs to 92 174 runs / 1.41 MB versus
43 316 / 0.66 MB.  `srcversion` tracks the kernel sources, not the generated
header, so the artwork swap alone did not move it — the scaler change did
(now `27DFE0F25007224D8224D56`).

### 2026-10-16 — BOOTSTRAP-PARITY: the 871-command replay vs the vendor init

Ticket: "Diff 871-cmd replay vs `CRXADV7604`/`CLFE3` init in `avmu3_x64.sys`."
The replay in `avmcv710-hw.c` is not hand-derived — `tools/gen_bootstrap.py`
emits it verbatim from the community port's fast-start capture, which is
itself a recording of the official driver's real USB EP1 traffic.  So the
honest question is whether that recording still *is* the vendor's init and
whether the fast trim kept the end state.  Both are now checked mechanically
by `tools/check_bootstrap.py` (exit 0 = parity):

1. **Header == reference capture.**  871 commands, 4495 write bytes,
   **0 differences** against `commanddata_cv710_fast.cpp`.  (The read counts
   are decimal: `UsbStream.cpp` parses them with `std::stoi` base 10, so
   `<512` is 512 bytes, not `0x512`.)
2. **The trim is state-preserving.**  All 841 fast writes appear, **in
   order**, among the 4301 writes of the 7108-command full capture
   (`commanddata_cv710.cpp`), and the final single-register I2C state is
   identical (132 `(bridge-id, reg)` entries, none only-in-full, none
   only-in-fast, none differing).  The trim removes reads and collapsed poll
   loops only.
3. **The EDID is the vendor's EDID.**  The replay uploads 256 bytes to EP1
   bridge id 0x36; that byte string is present *identical* in
   `driver_extracted/win/avmu3_x64.sys` (file offset 0xd9fb0) and in
   `driver_extracted/mac/.../libC877Driver.dylib` (0x4669c), and absent from
   the sibling `libC875Driver.dylib`.  (Earlier notes said "22× 256-byte EDID
   writes to 0x50"; the decode is 8× 32-byte writes to bridge id 0x36 —
   corrected in the matrix and §6.)

Decode of the write stream (841 writes): leading-byte histogram
`0x01:696 0x03:16 0x05:56 0x06:29 0x07:27 0x08:1 0x09:15 0x0e:1`; the `0x01`
writes are `01 <bridge-id> <reglen> <reg..> <data..>` and cover the ADV7604
IO map (0x20, 521), HDMI map (0x34, 28), content-protection map (0x22, 32),
plus 0x10 (29), 0x18 (2), 0x26 (7), 0x32 (11), 0x36 (EDID), and the PCA9539
GPIO expander (0x74, 42).  The `0x03/0x05/0x06/0x07/0x09` writes are the
CLFE3/FPGA bring-up (address + payload + commit bursts) and `0x0e` is the
leading FX3 setup word.

What cannot be done from the binaries: the vendor's ADV7604/CLFE3 init is
emitted by library calls (`CRXADV7604::Costomize7604Init` is a few method
calls; the ADI driver synthesises the I2C writes at run time), so no flat
register list exists to diff value-by-value.  The capture — which we replay
byte-for-byte and have now pinned to the vendor EDID — *is* the artifact.
Residual (hardware-only) risk: the captured session could predate a vendor
init change.

### 2026-10-16 — standby overlays restored (no-signal / out-of-range)

TODO-3's splash subsystem was deleted on 2026-10-15 as "bloat" (a 14.8 MB
generated header plus a 4.6 MB per-device heap buffer).  The user asked for
"as close to the official macOS/Windows driver as possible", and the vendor
does keep four 640x480 standby bitmaps in the installer
(`driver_extracted/win/aver_custom_{no_signal,out_of_range,hdcp_protection,
content_protection}.bmp`), so the overlays come back - without the bloat.

Data (`tools/gen_bmp.py`):
- **Source = the macOS rendition**: `cv710userspace/assets/aver_custom_*.bmp`
  (the macOS RECentral presentation — the AVerMedia logo is composited into a
  tighter dialog), not the Windows installer's `driver_extracted/win/` copy.
  Both are 640x480x24 and vendor-produced; the macOS one is the branded
  presentation and it lives in-tree, so the build no longer depends on the
  extracted installer.  The Windows rendition is still one argument away
  (`python3 tools/gen_bmp.py /path/to/driver_extracted/win`).  It is also
  *cleaner*: a flat panel instead of a dithered gradient, so it RLEs to
  43 316 runs / 663 KB versus 92 174 runs / 1.41 MB for the Windows copy.
- Converts each BMP to the FPGA's YUY2 raster as before, but stores it
  **run-length encoded** as `(u32 word, u16 count)` pairs.  Versus the raw
  arrays removed 2026-10-15 (14.8 MB header, 2.4 MB `.rodata`) the RLE header
  is 0.66 MB and the `.ko` grew by only ~0.55 MB (3.23 MB total, down from
  3.62 MB with the Windows data).  (Note: `srcversion` is derived from the
  kernel sources, not the generated header, so a bitmap-only change does not
  perturb it — verify visually.)
- `avm_splash_expand()` decodes the RLE into a per-device 640x480 sprite once
  per kind; `avm_splash_compose()` **scales it to fill the frame** (uniform
  "contain", letterboxed with black, bilinear in luma, chroma from the nearest
  source macropixel) and caches by (kind, geometry).  The overlay is a 640x480
  *full-frame* design — the macOS rendition puts the AVerMedia logo in the
  corner of that canvas — and the vendor's host app paints it scaled into its
  window, so blitting 1:1 (the first cut) left the card a ninth of a 1080p
  frame with the logo stranded mid-screen; at 1080p this now renders a
  1440x1080 card centred in the frame.  The scaling runs once per geometry
  change, never per frame (the composite is cached).

Delivery (`avmcv710-status.c`):
- The status work now ticks at ~30 Hz but keeps the gentle **1 Hz** ADV7604
  poll (the EP1-wedge protection), and re-delivers a splash every 3rd tick
  (~10 Hz) - enough to keep a static image flowing without memcpy-ing 4.6 MB
  at 30 Hz.
- An overlay is shown only after the stream has been idle for 250 ms
  (`p->last_real_kt`, stamped by the parser on every recognizable frame), so
  a mode re-lock or USB hiccup does not flash the splash; real video clears
  it immediately.
- Kind selection: TDMS unlocked -> `no-signal`; locked but geometry outside
  the deliverable range -> `out-of-range`; locked but dark -> `no-signal`.
- Delivery goes through the ordinary `avm_deliver_frame()`, so timestamp and
  sequence are stamped the same way as live frames.

Non-native geometry (`avm_req_mismatch`, `avmcv710-video.c`/`-parser.c`):
- The FPGA cannot scale.  When a consumer negotiates a geometry other than
  the source raster (the user's report: "any resolution other than 1080p60
  says res not supported, there is a bmp for that"), `S_FMT` records the
  un-honoured request and the driver shows the `out-of-range` overlay instead
  of live frames - matching the official driver, which paints
  `aver_custom_out_of_range.bmp` ("Signal Out of Range - Please adjust your
  resolution to supported range") rather than scaling.
- A `S_FMT` that was already the reported geometry is *not* a forced pick and
  clears the mismatch, so a consumer that simply accepts the device format
  (OBS's normal path, `ENUM_FRAMESIZES` index 0 = live) always sees live
  video, including across source-geometry changes.
- While the mismatch holds, `avm_handle_frame_end()` drops live frames (they
  cannot be delivered at the negotiated size), so the overlay and live video
  never interleave.

Acceptance (user test):
- Pull the HDMI cable during capture -> the `no-signal` bitmap appears at the
  negotiated geometry within ~0.25-1 s; replug -> live video returns.
- Set OBS to a resolution other than the source's -> "Signal Out of Range"
  appears; set it back to the source resolution -> live video returns.
- Normal 1080p60 capture is unchanged (no overlay, no extra log spam).

### 2026-10-16 — frame-interval menu now matches what the hardware can do

Observed behavior: changing the frame rate (or resolution) in OBS "does
nothing".  Root cause for the rate half: `avm_enum_frameintervals()` advertised
a fixed 7-entry menu (60/59.94/50/30/29.97/25/24) **regardless of state**, but
the only rates this device can actually deliver are the source's native rate
and - with the CVX8897 converter enabled (`cvx8897=1`) - its **half**.  So
OBS offered 24 / 59.94 / 29.97, and `S_PARM` silently turned them into
passthrough.  (The resolution half is the vendor overlay behaviour; see the
`out_of_range` note below.)

Change (`avmcv710-video.c`):
- `avm_native_fps()`: the input cadence = `measured_fps`, doubled when the
  converter is currently halving (measured is the delivered rate).
- `enum_frameintervals()` now offers exactly index 0 = native, and index 1 =
  native/2 **only when `avm_cvx8897_enabled()`** and the native rate is even.
  This is the vendor's discrete "Video Input Frame Rate" 60p/30p/50p/25p set,
  not a fictional 7-way menu.
- `g_parm` still reports the delivered rate; `s_parm` is unchanged (records
  the request for the converter worker).

Verified against the vendor control surface: the CVX8897 tables
(`avm_cvx8897_half_rate`) only ever halve, so any other advertised rate was a
lie.  `measured_fps`-based, so before a first measurement the menu shows
{60[, 30]} (the driver's 1080p60 nominal).

Acceptance (user test): with `cvx8897=0` OBS offers only the source rate; with
`cvx8897=1` it offers the source rate and its half, and picking the half makes
`journalctl -k` show `cvx8897: halving ...`.

### 2026-10-16 — PARSER-PAD: short frames black-padded, not dropped (vendor parity)

Fixes the P0 `PARSER-PAD` item from the vendor RE below.

**RE provenance.**  `driver_extracted/mac/…/Resources/libC877Driver.dylib` is
the CV710 macOS capture driver and, unlike `avmu3_x64.sys` (stripped, CodeView
PDB not shipped), it **retains its symbol table** (`ParseAVerData`,
`VideoParseBuffer`, `Device`, …).  Disassembling `__text` (extract the section,
`objdump -D -b binary -m i386`) gives the real framing:

- `ParseAVerData::updateVideoStartInfo` (C0) validates exactly like ours:
  `(b0+b1+b2-0x40)&0xFF == b3` **and** `b1 == 0x01`.
- `ParseAVerData::updateVideoEndInfo` (C1) validates
  `(b0+b1+b2+b3+0xC1)&0xFF == b4` (i.e. `-0x3F`) **and** `b0 == last_seq`.
  Note it does *not* separately test `b1==0x02`/`b3==0x38`; `0x38` is folded
  into the checksum.  Our extra `tb1==0x02` test is stricter but harmless.
- `Device::initDevice` and `Device::InitialOverlayScreen` **pre-fill the frame
  buffer with `0x80108010`** (legal YUY2 studio black: Y=16,U=128,Y=16,V=128)
  and the parser copies the received raster over the top.  A frame that lost
  lines therefore comes out **black-padded — the vendor does not drop it**.
- `PROTOCOL.md` §3 states the same rule (short frames ≥ `MINIMUM_VIDEO_FRAME_WORDS`
  padded with `0x80108010`; below the minimum dropped).

**Change** (`avmcv710-parser.c`, `avmcv710.h`, `avmcv710-video.c`):
- New `AVM_PAD_WORD 0x80108010u` and `avm_reconcile_frame()`: a C1 whose word
  count lands in the locked mode's detection window is presented at exactly
  `mode->target` words — a short frame gets an `AVM_PAD_WORD` tail, a long one
  is truncated.  Word counts outside every mode window are still corruption and
  are dropped (CV-23 bounds are untouched).
- The interlaced path reconciles the **field** to the field target *before*
  weaving, which also removes the stale-row weave that a short field could
  have produced (the previous exact-size gate hid it).
- Removed the invented `frame_words >= 40000` floor: the mode detector's
  per-mode windows are the real gate.  (`40000` / `0x9C40` occurs **nowhere**
  in either vendor binary.)
- New `padded_frames` counter, reported in the stop `dev_dbg` and via a
  ratelimited `short frame: N/M words, black-padded` log.

**Caveat.**  The vendor's exact `MINIMUM_VIDEO_FRAME_WORDS` was not recoverable
as a single constant in the disassembly, so the gate is `avm_detect_mode()`'s
per-mode window (±~2.5 % / 64 words) — at least as conservative.  A count that
does not land in any window is still dropped.

**Acceptance (user test).**  With a marginal HDMI source (or a mid-recording
re-lock) OBS shows a black-padded frame instead of a frozen one; `journalctl -k`
shows at most a ratelimited `short frame: … black-padded`.  No regression on a
clean 1080p60 source: every frame is exact, `padded_frames` stays 0.

### 2026-10-16 — audio: source-paced sample rate from the Audio InfoFrame

**Gap.**  The capture PCM was hard-wired to 48 kHz.  The FPGA forwards the HDMI
audio clock untouched and nothing in the path resamples, so a 44.1 kHz source
played ~8.8 % fast (audible pitch shift).  The userspace port already reads the
source rate and retunes its SDL stream (`SdlAudioOutput::setAudioSampleRate`);
the kernel had no equivalent.

**Spec.**  From `UsbStream::queryVideoSignalStatus` (cv710userspace) plus
CEA-861:

- Audio InfoFrame *present* flag: IO map 0x60 (bridge 0x20) bit 1.
- Audio PLL locked: HDMI map 0x04 bit 0; "audio sample packet detected":
  HDMI 0x18 bit 0.  Both must be set.  HDMI 0x18's low nibble is **not** a
  rate; decoding one from it latched random values and detuned playback
  (`cv710userspace/ISSUES.md` §12).
- The Audio InfoFrame itself lives on the **INFOFRAME page** (I2C slave 0x3e):
  header at 0xE3 (type 0x84, version 0x01, length 0x0A) and payload byte 1 at
  0x1D, sample-frequency code in bits 6:2 (1..7 =
  32k/44.1k/48k/88.2k/96k/176.4k/192k; 0 = refer to the stream header).  Same
  layout as Linux `hdmi_audio_infoframe_unpack`.

**Change** (`avmcv710-status.c`, `avmcv710.h`, `avmcv710-audio.c`):

- The 1 Hz status tick now also reads audio lock/detect and the InfoFrame and
  publishes `struct avm_signal.audio_rate` / `audio_locked`.  Every one of
  those reads is optional: a failure degrades to "audio unknown" and never
  fails the geometry poll (CV-13).  The rate is sanitised against the last
  known-good value, defaulting to 48000.
- A locked-and-detected stream logs
  `[ADV7604] HDMI audio <rate> Hz (0x04=.., 0x18=.., AIF=..)` on each
  lock/rate change.
- `avm_audio_pcm_open()` pins the sink to the source rate with
  `snd_pcm_hw_constraint_single` once the poll has seen one, so an application
  cannot open at the wrong pitch; before the first poll the full 32/44.1/48 set
  stays available (the previous behaviour).

**Divergence kept.**  Infoframe codes above 48 kHz (88.2k..192k) are reported
but not adopted: the sink advertises up to 48 kHz and the audio packet window
caps at 8192 bytes.  A `dev_warn_once` names the source rate.

**Acceptance (user test).**  With `avm_audio=1` and a 44.1 kHz HDMI source,
`arecord -D hw:CARD=AVMCV710 -f S16_LE -c 2 -d 10 /tmp/a.wav` records at
44100 Hz (journal logs 44100); a 48 kHz source keeps recording at 48 kHz,
unpitched.

### 2026-10-15 — streamlined rewrite: bloat elimination, disconnect park fix, clean 1Hz telemetry

Session: clean-slate redesign based on lessons from `cv710userspace` and hardware analysis.

1. **Root cause of soft-reload probe timeout `-110` resolved (`avmcv710-usb.c`):**
   - **Mechanism:** In `avm_usb_disconnect()`, unbinding the driver killed the EP 0x83
     video URBs *without* parking the FPGA (`avm_fpga_work_mode(dev, false)`) or
     resetting stream DMA (`avm_reset_stream_dma(dev)`). This left the Cypress FX3
     GPIF II controller stuck mid-DMA trying to push pixels with no host buffer, wedging
     the FX3 ARM9 firmware and locking up EP1. When the driver was reloaded moments later,
     `avm_bootstrap()` timed out across all retries after 35 seconds with `-110`.
   - **Fix:** `avm_usb_disconnect()` now symmetrically parks the FPGA and resets FX3 stream
     DMA *before* killing URBs, exactly matching `avm_stop_streaming()` and `UsbStream::shutdownStream`.
2. **In-kernel splash bloat elimination (`avmcv710-status.c`, `avmcv710.h`):**
   - Deleted the generated 14.8 MB `avmcv710-bmp.h` header containing raw BMP arrays.
   - Removed 4.6 MB per-device `splash_frame` kernel heap allocation.
   - Replaced 33 Hz timer overlay pump with a clean, low-overhead 1 Hz receiver telemetry
     poller (`avmcv710-status.c`) that monitors TMDS lock, geometry, and colorspace without
     interfering with video streaming or wedging the EP1 control pipe.
   - Module `.ko` size dropped from 5.4 MB down to 2.8 MB (compressed 632 KiB).
3. **Running kernel 7.2.9 native compilation:**
   - Compiled cleanly against `/lib/modules/7.2.9-2-cachyos/build` with Clang/LLVM (`LLVM=1`).
   - Module `srcversion DD67527A5AD86CE6D3F925C`, `vermagic: 7.2.9-2-cachyos SMP preempt mod_unload`.
   - Updated `fix-driver.sh` with the new expected srcversion.

### 2026-10-13 — audio framing, DMA bounce buffers, colorspace detection, auto_reset watchdog


Session: finishing work on the kernel driver, cross-verifying with `cv710userspace` and hardware specifications.

1. **DMA bounce buffers for EP1 (`avmcv710-hw.c`):**
   `usb_bulk_msg()` requires DMA-capable memory. Callers in bootstrap, status
   poll, and drain passed stack buffers, which violate DMA safety under
   `CONFIG_VMAP_STACK=y`. `avm_ep1_write_t()`, `avm_ep1_read_t()`, and
   `avm_ep1_drain()` now bounce through `kmemdup`/`kmalloc` heap buffers.
2. **Audio packet preservation between frames (`avmcv710-parser.c`):**
   The parser previously guarded the initial hunt with `if (!p->in_video &&
   p->frame_words == 0)`, which mistakenly discarded all non-C0 data at the
   start of every chunk arriving between a frame's C1 trailer and the next C0
   header. Audio packets (marker `0x58FFFF00`) arrive continuously in this gap.
   Changed guard to `if (!p->stream_locked)` so audio packets are parsed
   continuously once the initial stream lock is established.
3. **Audio sample alignment check (`avmcv710-parser.c`):**
   Added `(audio_bytes & 3) == 0` validation to ensure every audio payload
   conforms to 4-byte (S16 stereo sample) alignment.
4. **Clock-recovery C1 trailer consumption (`avmcv710-parser.c`):**
   Fixed the C1 clock recovery skip to consume the full 3 trailer words
   (`i += 3;`) matching standard frame end handling.
5. **ADV7604 AVI colorspace auto-detection (`avmcv710-status.c`, `avmcv710-cvx8897.c`, `avmcv710-video.c`):**
   - In `avm_poll_signal()`, read HDMI 0x34 register 0x53 to inspect the
     colorspace low nibble. Set `sig.colorspace` and `sig.rgb = (sig.colorspace <= 0x01)`.
   - In `avm_cvx_work()`, pass `sig.rgb` into `avm_cvx8897_half_rate()` to
     automatically select the vendor's dedicated 720p60 RGB half-rate table
     instead of falling back to YCbCr 4:2:2.
   - In `avm_pixfmt()`, check `sig.colorspace` and set `pix->quantization =
     V4L2_QUANTIZATION_FULL_RANGE` when source is full-range (HDMI 0x53 value
     1, 6, or 7), preventing crushed blacks and clipped whites in OBS.
6. **ALSA sample rate expansion (`avmcv710-audio.c`):**
   Expanded ALSA PCM hardware constraints and hw_params check to support
   32000 Hz, 44100 Hz, and 48000 Hz. In `avm_audio_pcm_hw_free()`, set
   `a->running = false;` under lock barrier.
7. **`VB2_READ` flag added (`avmcv710-video.c`):**
   Added `VB2_READ` to `dev->q.io_modes` to back `V4L2_CAP_READWRITE` for
   user applications that use `read()` instead of mmap streaming.
8. **Endpoint stall recovery (`avmcv710-usb.c`):**
   Added `usb_clear_halt()` on `-EPIPE` in `avm_parse_work()` for
   `AVM_EP_VIDEO_IN` with resubmission.
9. **Teardown race prevention (`avmcv710-usb.c`, `avmcv710-cvx8897.c`):**
   - Reordered `avm_usb_disconnect()` to flush the workqueue before cancelling
     `dev->cvx_work`.
   - In `avm_cvx8897_set_mode()` and `avm_cvx_work()`, gated sleeps and
     execution on `READ_ONCE(dev->streaming)`.
10. **Watchdog auto-recovery module parameter (`avmcv710-usb.c`):**
    Added `auto_reset` module parameter (default 0=off). If enabled, when the
    5-second watchdog flags a silent stream under STREAMON, it attempts
    `usb_reset_device(dev->udev)` to kick the FX3 out of its wedged state.
11. **ALSA cyclic buffer alignment, pointer bounds & pause/resume (`avmcv710-audio.c`):**
    - Sized cyclic write buffer in `avm_audio_push()` using negotiated
      `frames_to_bytes(rt, rt->buffer_size)` rather than the full preallocated
      `rt->dma_bytes` (1 MB), preventing audio packets from being written outside
      the active mmap'd ring buffer.
    - Bound `avm_audio_pcm_pointer()` to strictly return `< rt->buffer_size` frames.
    - Added `snd_pcm_hw_constraint_integer(subs->runtime, SNDRV_PCM_HW_PARAM_PERIODS)`
      to ensure period boundaries align with cyclic buffer wraps.
    - Added `SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME` flags and handled
      `TRIGGER_PAUSE_RELEASE` and `TRIGGER_RESUME`.
12. **EP1 control pipe stability & I2C error handling (`avmcv710-hw.c`, `avmcv710-status.c`):**
    - Removed `avm_ep1_drain()` from live I2C read/write failure paths. Draining
      mid-stream submits 512-byte bulk IN requests with 20 ms timeouts; on xHCI
      this aborts/cancels active TRBs and wedges the FX3 endpoint state machine.
    - Gated `avm_poll_signal()` to abort immediately on the first core read
      failure (`0x20/0x6A`) instead of issuing 4 more requests into a busy FX3.
    - Delayed initial status work in `avm_status_start()` by 1000 ms after
      `start_streaming()` so the FPGA/FX3 video DMA stabilizes before I2C
      transactions begin.
    - Added failure backoff: if `poll_failures >= 3`, back off polling interval
      to 1000 ms (1 Hz) to prevent bus hammering.
13. **Full standard resolutions and frame rates enumeration (`avmcv710-video.c`):**
    - Enumerate standard discrete frame sizes: 1920x1080, 1280x720, 720x576,
      720x480, 640x480.
    - Enumerate standard frame rates: 60, 59.94, 50, 30, 29.97, 25, 24 fps
      unconditionally in `avm_enum_frameintervals()`.
    - Default nominal interval to 60 fps (1080p60) instead of 30 fps, enabling
      OBS to offer full 1080p60 capture.
14. **Soft-reload probe -110 timeout (`avm_usb_probe` / `avm_bootstrap`):**
    - **Observed log (2026-10-13 23:08):**
      ```
      Thg 10 13 23:08:14 ProDesk kernel: usbcore: deregistering interface driver avmcv710
      Thg 10 13 23:08:49 ProDesk kernel: avmcv710 2-1:1.0: probe with driver avmcv710 failed with error -110
      Thg 10 13 23:08:49 ProDesk kernel: usbcore: registered new interface driver avmcv710
      ```
    - **Analysis:**
      When `avmcv710` was unloaded (`23:08:14`) and reloaded, probe failed with error `-110` (`-ETIMEDOUT`) after exactly 35 seconds (`23:08:49`). This matches the 3 `AVM_BOOT_ATTEMPTS` in `avm_usb_probe()`, which issues bulk transfers over EP1 during `avm_bootstrap()`. Prior to the reload, the earlier test run had wedged EP1 during an unmitigated I2C timeout cascade (`i2c read 34/07 len 2 failed: w=-110 r=-110`). Because a module reload does not reset USB bus power to the Cypress FX3 microcontroller, EP1 remained unresponsive.
    - **Action plan for tomorrow:**
      1. Reset device hardware state: physical replug or sysfs port toggle (`echo 0 > /sys/bus/usb/devices/<port>/authorized; sleep 2; echo 1 > ...`).
      2. Re-test driver: run `sudo ./fix-driver.sh` and verify clean probe without `-110`.
      3. Verify 1080p60 capture in OBS and test EP1 stability with the newly committed delay/backoff guards (`af23399`).
      4. Driver-level probe recovery: evaluate whether issuing `usb_reset_device(udev)` on bootstrap timeout or during probe can safely unwedge the FX3 without triggering probe recursion.


### 2026-10-11 — `.inf` "device parameter" keys ruled out

`avmu3c877_x64.inf` service params (`ENABLE_SCALER=1`, `SCALER_OP_MODE=2`,
`VIDEO_IN_QUEUE=0x30`, `AUDIO_IN_QUEUE=0x19`, `LATENCY=0x0053EC60`) are Windows
registry knobs for the vendor's user-mode stack (queue depths, latency
tuning) — **not FX3/EP1 commands**.  Nothing to port to the kernel.

### 2026-10-11 — warm-start semantics (soft-reload bug, TODO-4)

- The userspace replays the **full bootstrap on every open** ("re-uploads the
  256-byte EDID and drives the HPD handshake"; skipping it on warm start
  left stale mode lists until a physical replug).  Our probe replays the same
  871-command trace, including the EDID upload (256 B to bridge id 0x36,
  byte-identical to the vendor binaries — see the BOOTSTRAP-PARITY entry), so
  that part is covered.
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
  buffer, `index`/`id`/`avm_audio` module params,
  `MODULE_SOFTDEP("pre: snd-pcm")`.
  Audio init is non-fatal (video-only fallback on failure).
  **2026-10-12 regression:** first hardware install oopsed (`ops->prepare`
  NULL, see dated entry) and collapsed system audio.  Fixed + the card is now
  **opt-in via `avm_audio=1`, default off** — a reload without the param is
  the known-stable video-only module.  Acceptance: with `avm_audio=1`,
  `arecord -D hw:CARD=AVMCV710 -f S16_LE -r 48000 -c 2 -d 10 /tmp/a.wav`
  while OBS records (`/proc/asound/cards` lists AVMCV710), **and** the rest of
  the system keeps producing sound (no WirePlumber collapse).  Source rate
  is now read from the Audio InfoFrame (2026-10-16): the sink pins itself to
  the incoming 32/44.1/48 kHz instead of always playing at 48 kHz.
- [ ] **TODO-4 soft-reload no output & probe -110 timeout** —
  Observed log (2026-10-13 23:08):
  ```text
  Thg 10 13 23:08:14 ProDesk kernel: usbcore: deregistering interface driver avmcv710
  Thg 10 13 23:08:49 ProDesk kernel: avmcv710 2-1:1.0: probe with driver avmcv710 failed with error -110
  Thg 10 13 23:08:49 ProDesk kernel: usbcore: registered new interface driver avmcv710
  ```
  When FX3 EP1 is left wedged by a prior failed session or unhandled I2C cascade, a `rmmod`/`modprobe` soft-reload attempts `avm_bootstrap()` on the still-wedged endpoint without USB power cycling, timing out after 35 s with `-110`.
  Acceptance:
  1. Re-test with fresh hardware state (replug / sysfs authorized toggle) using `fix-driver.sh` to confirm probe succeeds and OBS captures 1080p60 cleanly.
  2. Test `auto_reset=1` or investigate safe probe-time recovery (e.g. `usb_reset_device` fallback or udev helper) so rmmod/modprobe recovers even if EP1 was wedged.


### P0 — vendor-parity gaps from the 2026-10-15 RE of `avmu3_x64.sys`

Context: quick disassembly of `driver_extracted/win/avmu3_x64.sys`
(PE32+ x64, ~710 KB `.text`, **no COFF symbols** — only an RSDS CodeView path
to a non-shipped PDB; names recovered from RTTI/format-string xrefs) **and**
`driver_extracted/mac/…/Resources/libC877Driver.dylib` (Mach-O i386, but with
its **symbol table intact** — `ParseAVerData`, `VideoParseBuffer`, `Device`;
disassemble the `__text` section as raw i386).  The readable mac driver is the
better framing source of truth.  (Both `libC875Driver` and `libC877Driver`
ship; C877 is the CV710.)

- [x] **PARSER-PAD — short-frame pad-and-keep (vendor) vs drop (ours).**
  **DONE 2026-10-16** (see the `### 2026-10-16 — PARSER-PAD` entry).  The
  vendor (`libC877Driver.dylib::Device::initDevice`) pre-fills the 4,608,000-byte
  (`0x465000` = 1920×1200×2 = `AVM_FRAME_BPF`) frame buffer with `0x80108010`
  (studio black) and copies the raster over it, so a short frame is delivered
  black-padded.  Our parser now does the same via `avm_reconcile_frame()`:
  a C1 inside the locked mode's window is presented at `mode->target` words
  with an `AVM_PAD_WORD` tail (long frames truncated); counts outside every
  window are still dropped (CV-23).  The invented `frame_words >= 40000` floor
  (`40000`/`0x9C40` occurs nowhere in the vendor binaries) is gone.  Pending
  user hardware test.
- [x] **FRAMING-LAYER — where does the vendor parse C0/C1?**  **RESOLVED
  2026-10-16.**  The vendor *does* frame the stream in the driver, but it
  dispatches on the **marker's leading byte** (`ParseAVerData::determineNextState`
  compares against `0xC0` / `0xC1` / `0x58` as single bytes), so the 32-bit
  constants `C0FFFF00`/`C1FFFF00`/`58FFFF00` never appear — that is why the
  earlier scan came up empty.  Both `libC877Driver.dylib` (readable symbols)
  and `avmu3_x64.sys` (many `add $0xc0` C0-checksum sites) parse C0/C1 in the
  kernel driver.  Our in-kernel state machine is the same layer as the vendor;
  the earlier "marker constants absent" note was a false lead.
- [x] **RES-OVERLAY — non-native resolution → vendor's out-of-range splash.**
  **DONE 2026-10-16.**  Reporting from the user: on Windows, selecting any
  resolution other than the source's (e.g. 640×480 while the source is
  1080p60) shows `aver_custom_out_of_range.bmp` ("Signal Out of Range — Please
  adjust your resolution to supported range"), i.e. the vendor *refuses* rather
  than scales.  `S_FMT` now records a request it could not honour and
  `avm_req_mismatch()` drives the same overlay at the negotiated geometry (the
  parser drops live frames while the mismatch holds); a `S_FMT` matching the
  reported geometry clears it, so OBS's normal "accept the device format" path
  is unaffected.  Pending user hardware test.
- [x] **BOOTSTRAP-PARITY — 871-command replay vs `CRXADV7604`/`CLFE3` init.**
  **RESOLVED 2026-10-16** (see the dated entry above and
  `tools/check_bootstrap.py`).  The replay is not a hand-written sequence: it
  is the recorded EP1 traffic of the official driver, so parity holds by
  construction, and the three things that could have drifted are now checked
  mechanically:
  1. our `avmcv710-bootstrap.h` == the reference fast-start capture
     (`commanddata_cv710_fast.cpp`), 871 commands / 4495 write bytes, 0 diffs;
  2. every one of the 841 fast writes occurs, in order, in the 7108-command
     full capture (`commanddata_cv710.cpp`), and the two reach an identical
     final I2C register map (132 `(bridge-id, reg)` entries) — the trim drops
     only reads/poll loops;
  3. the 256-byte EDID the replay uploads is byte-identical to the EDID
     embedded in `avmu3_x64.sys` (0xd9fb0) and `libC877Driver.dylib`
     (0x4669c) — and absent from the sibling `libC875Driver.dylib`.
  The vendor emits its ADV7604/CLFE3 init through library calls
  (`CRXADV7604::Costomize7604Init` is a handful of method calls, not a
  register table; the ADI driver builds the I2C writes at run time), so no
  flat register list can be lifted from the binaries to diff value-by-value —
  the capture is the authoritative artifact and it is what we replay.  The
  remaining risk is unfalsifiable without hardware: that the captured session
  predates a vendor init change.  (`CLFE3::SetInterPattern` and `SetPWMMode`
  still have no kernel counterpart; the replay covers the reachable state.)

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
  cable logs the TMDS UNLOCKED line within ~1 s.
- [x] **TODO-3 overlays — RESTORED 2026-10-16.**  The overlay/splash subsystem
  (640×480 BITMAP composite delivered through `avm_deliver_frame()`, the
  `avm_splash_bmp()` accessor, the `avm_splash` enum and the generated
  `avmcv710-bmp.h`) was deleted 2026-10-15 as "bloat" and brought back
  compactly: `tools/gen_bmp.py` now stores the bitmaps **run-length encoded**
  (14.8 MB header → 1.4 MB; `.rodata` 2.4 MB → 0.74 MB), the status worker
  keeps its 1 Hz ADV7604 poll but pumps the composite at ~10 Hz once the
  stream has been idle > 250 ms, and a non-native `S_FMT` (`avm_req_mismatch`)
  shows the vendor's "Signal Out of Range" bitmap exactly like the official
  driver.  Acceptance: cable pull → no-signal overlay; non-native OBS
  resolution → out-of-range overlay; normal capture unchanged.
- [x] Converter: read the ADV7604 AVI colorspace flag (HDMI 0x34 reg 0x53) so a 720p60 RGB source
  gets the vendor's dedicated RGB table instead of the YCbCr 4:2:2 table
  (interlace + input rate + colorspace are now polled/measured).
- [x] Audio: expanded ALSA capture PCM constraints to support 32/44.1/48 kHz
  sample rates, and the status poll now reads the source's real rate from the
  Audio InfoFrame (2026-10-16) so the sink is pinned to it instead of 48 kHz.
- [x] Watchdog auto-recovery: `auto_reset` module parameter (default 0=off)
  triggers `usb_reset_device(dev->udev)` when silent stream is flagged.

### P2 — polish

- [x] Overlay pump — **restored 2026-10-16** (see TODO-3 above); RLE-stored
  bitmaps, ~10 Hz pump, `avm_req_mismatch` for the non-native case.
  `driver_extracted/win/aver_custom_*.bmp` are the vendor bitmaps.
- [x] Wire `SNDRV_PCM_INFO_RESUME`-safe suspend/resume for audio.
- [x] HDCP: **won't do** — no implementation, no interrogation, no gating
  (see the README's *Legal* section and `cv710userspace/ISSUES.md` §6).
  `AVM_SPLASH_HDCP` / `AVM_SPLASH_CONTENT_PROTECTION` stay compiled in but
  unreachable: nothing reads the HDCP map, so there is no state to key them on.
  (An earlier note here proposed logging the ADV7604 HDCP state read; that is
  exactly the interrogation the project declines.)

### Never (documented divergence)

- EDID patching / VID_STD auto-switch (receiver is input-capability passthrough).
- FPGA scaler enable (vendor `ENABLE_SCALER=1` resizes; we keep the raster).
  A non-native `S_FMT` is answered the way the official driver does — with the
  out-of-range overlay, not with scaling (2026-10-16).

---

## Perf / measurement notes

- `61 fps` in OBS was the G_PARM counter counting `fps_cnt` frames over the
  window when the first counted frame only started the clock
  (`f4c24ad`): window ~2.5 s, divide by `fps_cnt - 1`, round to nearest,
  ignore ±1 flaps.  Delivery was always exactly 1 frame/C1.
- Latency: 4 URBs ≈ 1 frame of pre-buffer slack (~33 ms); ≥6 restored latency.

---

### 2026-10-16 (evening) — OBS Parity, 1:1 Centered Overlays, Anti-Popping, Anti-Drift

1. **Standby BMP Presentation (1:1 Centered, Not Enlarged):**
   - Replaced bilinear "contain" scaling in `avm_splash_compose()` with 1:1 pixel-perfect centered blitting.
   - The 640×480 standby sprite is composited into the center of the limited-range black canvas (`AVM_YUY2_BLACK`), preserving original pixel aspect without stretching or blur.
   - Target geometry in `avm_splash_tick()` uses `req_w x req_h` when set by OBS, ensuring payload matches the buffer size OBS allocated for non-native modes.

2. **Overlay Second-Time Loss Fix:**
   - Root cause: when the HDMI source was powered off after a streaming session, the ADV7604/FPGA continued generating free-run frames. `avm_detect_mode()` accepted these word counts and updated `p->last_real_kt = ktime_get()`, so the stream was never marked idle and the standby splash never fired on subsequent disconnects.
   - Fix: `avm_handle_frame_end()` now explicitly gates on `dev->sig.valid && dev->sig.tmds_locked`. When TMDS is unlocked, free-run/noise frames are dropped and `last_real_kt` is not refreshed, allowing the standby overlay to cleanly display every time.

3. **Framerate Variety & 24 FPS Drop (Windows Driver Parity):**
   - `avm_enum_frameintervals()` now enumerates standard discrete frame rates: `60, 59.94, 50, 30, 29.97, 25, 24, 23.98, 15` fps.
   - `cvx8897` converter parameter defaulted to 1 (on).
   - `avm_s_parm()` snaps requested `timeperframe` to the closest standard discrete interval, requests rate conversion from CVX8897, and echoes back the accepted interval.
   - Matching Windows driver behavior: when 24 fps is requested from a >30 fps source (e.g. 1080p60), `avm_handle_frame_end()` drops the frames to a black screen while refreshing `last_real_kt` (preventing "No Signal" overlay). Live video instantly resumes upon increasing FPS.

4. **Resolution Selection & Out of Range Overlay:**
   - `avm_try_fmt()` now accepts standard resolutions (`1920x1080, 1280x720, 720x576, 720x480, 640x480`) as well as the native detected geometry.
   - In `avm_s_fmt()`, if the negotiated resolution differs from native, `req_w` and `req_h` are latched; the parser drops mismatched frames, and `avm_splash_tick()` renders the official AVerMedia "Signal Out of Range" overlay at that negotiated geometry.
   - Selecting "Device Default / Leave Unchanged" in OBS clears `req_w/req_h` back to 0, seamlessly tracking native source resolution.

5. **Audio Popping on Disconnect Elimination:**
   - Root cause: `avm_audio_muted()` previously only checked `dev->sig.valid && !dev->sig.tmds_locked`, which is only polled every 1000 ms. When HDMI power was lost, floating bus noise poured into the ALSA buffer for up to a full second before the poll detected unlock.
   - Fix: `avm_audio_muted()` now mutes immediately when:
     - Stream parser loses initial lock (`!p->stream_locked`).
     - FPGA indicates clock/PLL recovery (`p->clock_recovering`).
     - Fewer than 2 consecutive valid video frames have arrived (`p->consecutive_valid < 2`).
     - No valid video frame has arrived for > 150 ms (`idle > 150 ms`).
   - Muted state writes clean silence (`memset 0`) while maintaining ALSA ring timing.

6. **Audio Latency Drift Elimination:**
   - Added `AVM_AUDIO_BUFFER_MIN = 32768` constraint and increased `AVM_AUDIO_PERIOD_MIN = 4096`.
   - Ring buffer is now guaranteed to be >= 4x the maximum FX3 audio packet size (8192 bytes), preventing `bytes > size` packet truncation and sample-drop latency drift.
   - `avm_audio_pcm_prepare()` resets `a->base = 0` so capture always begins frame-aligned.

### 2026-10-16 (night) — Investigation of Official Drivers & Resolution Mismatch Gating

1. **Official Driver Architecture Analysis (`driver_extracted/`):**
   - **Lattice ECP3 Hardware Scaler:** `avmu3c877_x64.inf` specifies `ENABLE_SCALER = 1`, `SCALER_OP_MODE = 2`, `UserEnableScaler = 0`. By default, hardware FPGA scaling is disabled (`UserEnableScaler = 0`).
   - **EP1 Scaler Control:** In `libC877Driver.dylib`, `CLFE3::setEnableFPGAScaler(enable)` sends EP1 vendor command `[0x22, enable]`. When bypassed, the FPGA passes the raw ADV7604 raster straight to the FX3 USB FIFO without scaling.
   - **Buffer Size Enforcement:** In `VideoParseBuffer::copyData()` (`libC877Driver.dylib`), the vendor driver explicitly compares bytes received against the pin's allocated buffer size. Incomplete or mismatching frames are dropped (`Copy Data Drop Incomplete Frame buffersize %ld total %ld diff = %ld`), and the driver paints `aver_custom_out_of_range.bmp` 1:1 centered on the pin's buffer canvas.
   - **Pin Definitions:** Both Windows (`avmu3_x64.sys`) and macOS (`libC877Driver.dylib` table `_g_AV877SupportResolution`) define discrete formats for 1080p, 720p, 576p, 480p, and 640x480. On Windows, DirectShow pins negotiate a fixed format at connect time; a mismatching input generates "Signal Out of Range" at the pin's geometry.

2. **Fix: False "Signal Out of Range" When No Signal:**
   - **Root Cause:** In `avmcv710-status.c`, `avm_splash_tick()` checked `if (avm_req_mismatch(dev, live_w, live_h))` before checking signal validity and TMDS lock. When no signal was connected, `live_w` defaulted to 1920x1080. If OBS was configured for 720p or 480p, `req_w != live_w` (1280 != 1920) immediately triggered `AVM_SPLASH_OUT_OF_RANGE`.
   - **Fix:** Inverted priority in `avm_splash_tick()`. `if (!sig.valid || !sig.tmds_locked)` is now evaluated first: a disconnected cable or unlocked TMDS unconditionally displays `AVM_SPLASH_NO_SIGNAL` at the negotiated OBS buffer geometry, regardless of what resolution OBS requested.

3. **Fix: Sub-1080p Source Switching Glitch ("Leave Unchanged" / Device Default):**
   - **Root Cause:** In `avm_s_fmt()`, `dev->req_w` and `dev->req_h` were previously cleared to 0 when `f->fmt.pix.width == live_w`. When OBS started on a 1080p source, OBS allocated 1920x1080 buffers (4,147,200 bytes, stride 3840). When the HDMI source switched to 576p (720x576) or 480p (720x480), `avm_req_mismatch()` returned false (due to `rw == 0`). The parser shoved raw 576p frames (829,440 bytes, stride 1440) directly into the 1920x1080 buffer, causing severe stride/pitch shearing and diagonal tearing in OBS. Additionally, `avm_splash_tick()` fell back to composing a 720x576 splash into the 1920x1080 buffer, causing the capture pipeline to choke.
   - **Fix:**
     - In `avm_s_fmt()`, `dev->req_w` and `dev->req_h` are now strictly locked to `f->fmt.pix.width` and `f->fmt.pix.height`.
     - In `avm_video_register()`, `dev->req_w` and `dev->req_h` are initialized to 1920x1080 matching `dev->fmt`.
     - In `avmcv710-parser.c`, incoming frames that do not match the negotiated buffer geometry are dropped safely, preventing buffer memory corruption.
     - In `avmcv710-status.c`, target geometry always aligns with `dev->req_w x dev->req_h` or `dev->fmt`, compositing a pixel-perfect 1:1 centered "Signal Out of Range" overlay at the consumer's buffer size.
     - In `avm_g_fmt()`, when `vb2_is_busy()` (streaming is active), `dev->fmt` is returned without mutation, preventing in-stream geometry desync.
     - When the user selects 720x576 in OBS, `s_fmt` reallocates 720x576 buffers and 576p streams natively and flawlessly.
### 2026-10-17 — regressions after the fps/res rework: honest interval menu, S_PARM passthrough, leave-unchanged format follow, qfmt delivery gate

User reports on hardware after `7f6d871` + `ad0bd48`: 1080p@50 and @24 broken
both as OBS framerate picks and as real input modes, and the diagonal-shear
"lines" regression back after a resolution switch and while exercising the fps
menu. Three root causes survived in that rework:

1. **Fake interval menu + snapped S_PARM.** `avm_enum_frameintervals()`
   advertised nine rates (60, 59.94, 50, 30, 29.97, 25, 24, 23.98, 15) that the
   hardware cannot produce, and `avm_s_parm()` snapped every request onto that
   menu and forced 60 fps when the consumer left the rate unset. The converter
   only ever converts 60->30 and 50->25 (progressive), so a picked 50 on a
   60 Hz input delivers a mislabeled native cadence and a picked 24 hit a
   hard-coded frame-drop hack: `cvx_req_fps == 24 && measured_fps > 30` dropped
   *every* frame, black screen until the user turned the rate back up (windows
   vendor does the same, but the menu should simply never offer it).
   - `avmcv710-video.c`: the menu is now exactly what the pipeline can deliver
     for the live geometry: entry 0 is the native input rate
     (`avm_native_fps()`, measured, or the 60 fps nominal before first lock),
     entry 1 is that rate halved and offered only when
     `avm_cvx8897_half_rate()` has a table for the mode (and `cvx8897=1`).
     `avm_s_parm()` passes the request through unclamped; the converter logic
     (`avm_cvx_work`) alone decides engagement: about-half -> convert, the
     input rate itself or nothing -> native passthrough, and g_parm then
     reports the true measured delivered rate. The 24 fps drop-hack is deleted
     (a real 24p/50p source still captures natively because entry 0 follows
     the source).
2. **The shear gate was keyed to the wrong snapshot.** `ad0bd48` gated live
   delivery on `avm_req_mismatch()` (req_w/req_h, the last S_FMT). But req and
   dev->fmt are two different snapshots at one moment: after a "leave
   unchanged" negotiation that accepted the live raster, the dropping became
   possible again only by pinning every S_FMT, which killed the
   V4L2_EVENT_SOURCE_CHANGE renegotiation (OBS stuck on the old buffers,
   out-of-range until manual re-pick) and reintroduced the shear whenever a
   frame slipped through before renegotiation.
   - `avmcv710-video.c`: `start_streaming()` snapshots the allocation geometry
     into the new `qfmt_w`/`qfmt_h` (`avm_video_register()` inits both to
     1920x1080).
   - `avmcv710-parser.c`: the delivery gate now compares the incoming mode
     against the `qfmt` snapshot, never against `dev->fmt`. A raster whose
     stride differs from the queued buffers physically cannot reach the
     consumer, in any state, following or pinned.
3. **"Leave unchanged" had no resolution detection.** The user ask:
     its best to leave unchanged option in obs and probably add detection
     for proper res detection if we use unchanged options
   - `avmcv710-video.c`: `avm_s_fmt()` records `follow_source` when the
     consumer accepted the live geometry (OBS "leave unchanged"/Device
     default maps to this); a user-picked other geometry pins (vendor
     DirectShow pin behavior, out-of-range stands).
   - `avmcv710-parser.c`: when the 4-frame hysteresis adopts a new mode and
     `follow_source` holds, `avm_publish_geometry()` writes the new mode into
     `dev->fmt` *before* `avm_source_change()` fires, so the consumer's
     renegotiation lands on the right stride. The `qfmt` gate keeps the
     transitional frames out until OBS reallocates; the out-of-range splash
     (`avm_req_mismatch` keyed at req, still the buffer geometry) covers the
     renegotiation window, then live video returns at the new raster.

**Parity audit of the new fps/res behavior:**

| scenario | vendor | ours now |
|---|---|---|
| OBS menus | pin-defined discrete formats and intervals | resolution menu from the DirectShow geometry set; fps menu from the live source: {native, native/2 when a converter table exists} |
| picked 50 on a 60 Hz input | delivered as the pin said, no hardware path (cadence mislabel) | not offered; picks snap to {60,30} |
| picked 24 with no converter path | black screen, recovery = raise the rate | not offered; a real 24p input still captures natively at 24 |
| "leave unchanged" + source res change mid-stream | DirectShow pin cannot resize: out-of-range until a matching pin | dev->fmt follows the source, SOURCE_CHANGE renegotiates OBS buffers, out-of-range splash covers the ~1 s window |
| explicit 720x576 with a 576p source | native capture | native capture (qfmt gate passes) |
| source res changes under a pinned geometry | out-of-range card at the pin geometry | out-of-range card at the pinned geometry, frames dropped, no shear |

**Hardware verification (user, after `sudo ./fix-driver.sh`):**
- OBS fps menu on a 1080p60 input offers exactly 60 and 30 (30 engages the
  converter, `cvx8897: halving ...` in the journal; 60 native).
- Real 1080p50 input: capture runs natively at ~50 (g_parm `1/50`, no
  converter kick); configuring 25 in OBS halves it.
- Real 1080p24 input: capture runs natively at 24, no black screen.
- "Leave unchanged" + console switch 1080p -> 576p: brief out-of-range card,
  then OBS renegotiates and 576p keeps streaming without manual resolution
  pick; no diagonal shear at any point.
- Explicit resolution picks still match the vendor: a 720x576 capture on a
  1080p source shows the out-of-range card until the source matches.

### 2026-10-17 (later) — audio: latency-drift diagnosis, full rate adoption, cadence telemetry

User: "massive latency and delay issue rn; the latency can sHift or drift
depending on times". Root cause candidate established from the code:

**Mechanism.** The sink previously advertised and pinned only 32/44.1/48 kHz
(documented divergence: "sink advertises up to 48 kHz"). But the FPGA forwards
the HDMI audio clock UNRESAMPLED: the producer streams payload bytes at the
source's real rate while the consumer drains the PCM at the negotiated rate.
Any source above 48 kHz (88.2/96/176.4/192 kHz) - or any app negotiation that
picked 48 kHz before the Audio InfoFrame was read - makes the producer faster
than the consumer: the 1 MB cyclic ring overruns, the memcpy silently
overwrites unread samples (capture latency steps forward), and the A/V offset
drifts forever. Also historically invisible: nothing measured the real
production cadence, so a wrong rate ran silently.

**Changes (`avmcv710-audio.c`):**
1. The sink advertises the full HDMI receiver set (32/44.1/48/88.2/96/176.4/192
   kHz, rate_max 192000) and `avm_audio_source_rate()` adopts every rate the
   InfoFrame can report, so a >48 kHz source is pinned at its true rate
   (PipeWire resamples to the graph rate; raw ALSA clients must ask for the
   real rate). This removes the "up to 48 kHz" divergence.
2. Production-cadence telemetry: `avm_audio_push()` measures the actual
   payload byte rate once per second (`measured_bps`), the ground truth about
   the source audio clock, independent of the InfoFrame.
3. Drift guard: if the negotiated capture rate differs from the source rate,
   `audio: capture negotiated N Hz but the source sends M Hz ... capture
   latency will drift (close and reopen the capture to resync)` fires
   (rate-limited, once per rate pair). An application that negotiated before
   the InfoFrame arrived cannot be retuned mid-stream; reopening resyncs.
4. `VIDIOC_LOG_STATUS` now prints an Audio telemetry line
   (running, app Hz, source Hz, measured cadence B/s and its Hz equivalent,
   produced/missed/overruns counts) and the Stream line prints
   `qfmt` and `follow` (the renegotiation state, so resolution-change log
   captures are self-contained). Run `v4l2-ctl -d /dev/video0 --log-status`
   while OBS streams and capture the journal lines.

**Acceptance:** with OBS streaming, the cadence in the Audio line must equal
(app Hz x 4); `overruns` must stay flat and `missed` must not grow during a
long capture. Drift with those conditions healthy isolates the consumer side
(OBS buffering), not the driver.
