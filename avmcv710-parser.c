// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-parser.c - framing parser for the CV710 bulk stream.
 *
 * Port of Device::onFrameData from the reverse-engineered userspace driver
 * (see cv710userspace PROTOCOL.md), hardened for kernel context:
 *
 *   - every marker is validated (C0 checksum + b1, C1 trailer checksum +
 *     tb1 + sequence, audio header length window) before being consumed
 *   - all "remaining space" arithmetic is clamped with min()/>= so a
 *     capacity minus-used can never underflow (the underflow family of
 *     bugs caused the userspace memcpy fault; here it would be a panic)
 *   - frame word counts are reconciled against the locked mode geometry:
 *     a count outside the mode's window is dropped (CV-23), but a short
 *     frame inside the window is black-padded with AVM_PAD_WORD and a long
 *     one truncated, matching the vendor's pre-filled frame buffer
 *   - audio packets are demuxed so the video raster stays aligned and their
 *     payload is handed to the ALSA capture PCM (avmcv710-audio.c); with no
 *     capture client open the bytes are simply dropped
 *   - markers that straddle a transfer boundary are held back and prepended
 *     to the next chunk (max 3 words), preserving cross-chunk correctness
 */
#include <linux/slab.h>

#include "avmcv710.h"

/*
 * Word counts for the standard modes (1 word = 1 YUY2 macropixel = 2 px).
 * Keeps the interlaced modes (field word counts) distinct from every
 * progressive mode so scan mode can be picked without a status read.
 */
static const struct avm_mode avm_modes[] = {
	{ 1920, 1080, 1036800, "1080p", false },
	{ 1920, 1080,  518400, "1080i", true  },   /* field words */
	{ 1280,  720,  460800, "720p ", false },
	{  720,  576,  207360, "576p ", false },
	{  720,  576,  103680, "576i ", true  },   /* field words */
	{  720,  480,  172800, "480p ", false },
	{  720,  480,   86400, "480i ", true  },   /* field words */
};

/*
 * The distinctive per-field word counts of the standard interlaced modes:
 * 1080i ~518,400, 576i ~103,680, 480i ~86,400 words.  Used both as a
 * tie-breaker when the scan-mode flags are unclear and to protect a genuine
 * field from the CV-28k progressive re-classification below.
 */
static bool avm_looks_interlaced_words(u32 n)
{
	return (n >= 510000 && n <= 524000) ||
	       (n >=  98000 && n <= 110000) ||
	       (n >=  80000 && n <=  92000);
}

/*
 * Map an observed frame/field word count to a mode.
 *
 * 1. Adaptive path first: when the ADV7604 status worker has locked TMDS
 *    and measured a geometry, accept any frame that matches it (within the
 *    same tolerances as the userspace driver, Device::detectVideoMode).
 *    This is what makes non-table sources work: 1366x768, 1280x1024,
 *    VGA-style timings from cheap TV boxes, etc. - the FPGA passes the
 *    receiver's raster straight through, so the word count alone is only a
 *    hint and the measured geometry is authoritative.
 *
 * 2. Fallback table: the standard modes, verbatim from the userspace
 *    driver (including the two 720p bands; the 1080i field band and the
 *    526k-545k 720p band only touch near their edges and the userspace
 *    driver accepted that overlap for years).
 */
static const struct avm_mode *avm_detect_mode(struct avm_device *dev,
					      u32 words,
					      bool hw_interlaced)
{
	struct avm_signal sig;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dev->sig_lock, flags);
	sig = dev->sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	if (sig.valid && sig.tmds_locked && sig.w >= 320 && sig.w <= 4095 &&
	    sig.h >= 120 && sig.h <= 2048 && (sig.w & 1) == 0) {
		/* Per-device scratch mode: the adaptive entries adopt the
		 * observed word count, so a tolerant match locks exactly on
		 * the size that actually arrived (AVM_MAX_FRAME_WORDS bounds
		 * it on the accumulation path).  No shared static: two
		 * adapters on one host must not race their measurement. */
		struct avm_mode *m = &dev->parser.meas;
		bool interlaced = hw_interlaced || sig.interlaced;

		/* Tie-breaker: if both scan-mode sources are clear or stale,
		 * the distinctive per-field word counts of the standard
		 * interlaced modes still identify a field stream - otherwise
		 * a 1080i field would be misclassified as a 1920x540
		 * progressive frame (same guard as the userspace driver). */
		if (!interlaced && avm_looks_interlaced_words(words)) {
			interlaced = true;
		} else if (interlaced && !sig.interlaced &&
			   !avm_looks_interlaced_words(words) &&
			   sig.w > 0 && sig.h > 0) {
			/* CV-28k: some SD firmware sets the C0 interlace bit
			 * spuriously for progressive 480p/576p (and flaps
			 * it), which would otherwise weave them into
			 * double-height 720x960 / 720x1152 pictures.  If the
			 * receiver is locked and measures a progressive
			 * raster whose word count matches exactly what the
			 * FPGA is sending, trust the receiver.  The guard
			 * above keeps genuine 1080i/576i/480i fields
			 * interlaced. */
			u64 expected = (u64)sig.w * sig.h / 2;
			u64 tol = expected / 50 + 64;

			if (words + tol >= expected && words <= expected + tol)
				interlaced = false;
		}

		if (interlaced) {
			u32 field_h = (words * 2) / sig.w;

			/* field word count must be an exact w*fieldH/2 */
			if (field_h >= 100 && field_h <= 700 &&
			    (u64)sig.w * field_h == (u64)words * 2) {
				m->w = sig.w;
				m->h = field_h * 2;
				/* target is the FIELD word count: it must
				 * match the incoming C1 count and the
				 * standard-mode table entries, and it is
				 * what avm_handle_frame_end() gates on.
				 * The full woven frame is built separately
				 * by avm_weave_field(). */
				m->target = sig.w * field_h / 2;
				m->name = "meas-i";
				m->interlaced = true;
				return m;
			}

			/* Derivation failed (non-exact timing): trust the
			 * receiver's geometry, converted to field words -
			 * same fallback as the userspace driver. */
			if (sig.h > 0) {
				u32 frame_h = (sig.h <= 700) ? sig.h * 2
							     : sig.h;
				u64 expected = (u64)sig.w * frame_h / 4;
				u64 tol = expected / 50 + 64;

				if (words + tol >= expected &&
				    words <= expected + tol) {
					m->w = sig.w;
					m->h = frame_h;
					/* target = words: the tolerance window
					 * accepted this C1, so gate exactly on
					 * what arrived - expected would never
					 * match a non-table timing. */
					m->target = words;
					m->name = "meas-i";
					m->interlaced = true;
					return m;
				}
			}
		} else {
			u64 expected = (u64)sig.w * sig.h / 2;
			u64 tol = expected / 50 + 64;

			if (words + tol >= expected && words <= expected + tol) {
				m->w = sig.w;
				m->h = sig.h;
				/* target = words: same rationale as the
				 * interlaced fallback - expected only matches
				 * when the FPGA raster happens to agree with
				 * the receiver measurement bit-for-bit. */
				m->target = words;
				m->name = "meas-p";
				m->interlaced = false;
				return m;
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(avm_modes); i++) {
		const struct avm_mode *m = &avm_modes[i];
		u32 lo, hi;

		switch (m->w) {
		case 1920:
			if (m->interlaced) { lo = 510000; hi = 524000; }
			else               { lo = 1000000; hi = 1050000; }
			break;
		case 1280:
			if (words >= 440000 && words <= 480000)
				return m;
			if (words >= 526000 && words <= 545000)
				return m;
			continue;
		case 720:
			if (m->interlaced) { lo =  98000; hi = 110000; }
			else               { lo = 195000; hi = 220000; }
			break;
		default: /* 480 */
			if (m->interlaced) { lo =  80000; hi =  92000; }
			else               { lo = 160000; hi = 185000; }
			break;
		}
		if (words >= lo && words <= hi)
			return m;
	}
	return NULL;
}

static u8 avm_next_seq(u8 seq)
{
	return seq == 0xFF ? 0x01 : seq + 1;
}

/* Hold trailing words (<= 3) for the next chunk. */
static void avm_hold(struct avm_parser *p, const u32 *d, u32 i, u32 count)
{
	p->hold_n = min_t(u32, count - i, ARRAY_SIZE(p->hold));
	memcpy(p->hold, d + i, p->hold_n * sizeof(u32));
}

void avm_parser_reset(struct avm_device *dev)
{
	struct avm_parser *p = &dev->parser;

	memset(p->hold, 0, sizeof(p->hold));
	p->hold_n = 0;
	p->stream_locked = false;
	p->in_video = false;
	p->in_audio = false;
	p->in_audio_padding = false;
	p->remaining_audio_words = 0;
	p->frame_words = 0;
	p->have_seq = false;
	p->field_flags = 0;
	memset(p->frame, 0, AVM_FRAME_BPF);
	memset(p->weave, 0, AVM_FRAME_BPF);
	p->parity_reliable = 0;
	p->fields_woven = 0;
	p->weave_toggle = 0;
	p->last_device_parity = -1;
	p->device_parity_changes = 0;
	p->pending_w = p->pending_h = p->pending_count = 0;
	p->active_w = 0;
	p->active_h = 0;
	p->mode_serial = 0;
	p->frame_sequence = 0;
	/* stamp a fresh stream as live; last_real_kt is diagnostic only since
	 * the overlay pump was removed (2026-10-15) */
	p->last_real_kt = ktime_get();
	p->fps_t0 = 0;
	p->fps_cnt = 0;
	p->measured_fps = 0;
	atomic_set(&p->valid_frames, 0);
	atomic_set(&p->dropped_frames, 0);
	atomic_set(&p->corrupt_frames, 0);
	atomic_set(&p->padded_frames, 0);
	atomic_set(&p->resync_count, 0);
}

/*
 * Rolling delivered-frame rate, for g_parm.  Counted at the delivery
 * points only: interlaced fields arrive at twice the woven frame rate, so
 * the counter must tick on the half-height pair, not on every field.
 */
static void avm_note_frame(struct avm_parser *p)
{
	p->fps_cnt++;
	if (p->fps_t0 == 0) {
		p->fps_t0 = ktime_get();
	} else {
		s64 el_ms = ktime_to_ms(ktime_sub(ktime_get(), p->fps_t0));

		/*
		 * Rate over a ~2.5 s window, with two corrections that stop a
		 * 60 Hz source from reporting 61:
		 *  - the window spans (fps_cnt - 1) frame periods: the first
		 *    counted frame only started the clock, it did not end one.
		 *    Plainly dividing fps_cnt gave 61 frames / 1000 ms = 61.
		 *  - round to nearest instead of flooring.
		 * OBS displays the G_PARM / enumerated interval, so a steady
		 * 60 Hz source must show 60, not 60.x.
		 */
		if (el_ms >= 2500 && p->fps_cnt > 1) {
			u32 fps = ((p->fps_cnt - 1) * 1000 + el_ms / 2) / el_ms;
			u32 cur = p->measured_fps;

			if (fps > 240)
				fps = 0; /* bogus: let it settle */
			/* Drop a single-count flap (window phase noise on a
			 * steady source); a real change is always >= 2. */
			if (!cur || fps > cur + 1 || fps + 1 < cur)
				p->measured_fps = fps;
			p->fps_cnt = 0;
			p->fps_t0 = 0;
			/* A fresh rate snapshot may let the CVX8897 converter
			 * decide (it needs the input rate). */
			avm_cvx8897_kick(container_of(p, struct avm_device,
						     parser));
		}
	}
}

/* Deliver a completed frame into the oldest queued vb2 buffer (if any). */
void avm_deliver_frame(struct avm_device *dev, const u8 *src, u32 words)
{
	struct avm_parser *p = &dev->parser;
	struct avm_buffer *buf;
	unsigned long flags;
	void *dst;

	if (!READ_ONCE(dev->streaming))
		return;

	spin_lock_irqsave(&dev->urbs_lock, flags);
	buf = list_first_entry_or_null(&dev->vb_bufs, struct avm_buffer, list);
	if (buf) {
		list_del_init(&buf->list);
		dev->pending_bufs--;
		/* Advance the sequence under the same lock that pops the
		 * buffer so two concurrent deliverers cannot stamp duplicate
		 * sequence numbers. */
		buf->vb.sequence = p->frame_sequence++;
	}
	spin_unlock_irqrestore(&dev->urbs_lock, flags);

	if (!buf)
		return; /* no consumer: drop (counted via frame skip) */

	if (words > AVM_MAX_FRAME_WORDS || (u64)words * 4 > AVM_FRAME_BPF) {
		/* unreachable behind the size gates; never over-fill */
		atomic_inc(&p->corrupt_frames);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	if (!dst) {
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	memcpy(dst, src, words * 4);
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, words * 4);
	/*
	 * The driver owns the timestamp and sequence for a capture queue with
	 * V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC; vb2 does not fill them.  Leaving
	 * them zero makes a consumer that paces/syncs on the buffer timestamp
	 * (OBS) accumulate latency, so stamp every delivered frame.
	 */
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	atomic_inc(&p->valid_frames);
}

/*
 * Interlaced field weaving.  A C1 with a word count matching the active
 * interlaced geometry delivers one field (half the frame height).  Weave
 * two alternating fields into a full frame before presenting it.
 */
static void avm_weave_field(struct avm_device *dev, u32 field_words)
{
	struct avm_parser *p = &dev->parser;
	u32 stride_words = p->active_w / 2;      /* macropixels per line */
	u32 field_lines = p->active_h / 2;       /* lines per field */
	u32 frame_words = stride_words * field_lines * 2;
	bool device_odd = (p->field_flags & 0x01) != 0;
	bool odd_field;
	u32 y;

	if (!p->active_w || field_words > stride_words * field_lines)
		return; /* corrupt field; CV-23: drop, never blend shifted rows */

	/*
	 * K3: the woven frame is twice the field size, and every row is
	 * written below *before* avm_deliver_frame() gets to validate the
	 * size - so the bound must be enforced here or an exotic adaptive
	 * interlaced geometry can overrun p->weave (AVM_FRAME_BPF).
	 */
	if (frame_words > AVM_MAX_FRAME_WORDS) {
		atomic_inc(&p->corrupt_frames);
		atomic_inc(&p->dropped_frames);
		return; /* CV-23: drop, never overrun */
	}

	/* Determine parity: trust the device flag once it has alternated. */
	if (p->last_device_parity >= 0 &&
	    (s8)device_odd != p->last_device_parity &&
	    p->device_parity_changes < 0xFF)
		p->device_parity_changes++;
	p->last_device_parity = (s8)device_odd;
	if (p->device_parity_changes >= 2)
		p->parity_reliable = 1;

	if (p->parity_reliable)
		odd_field = device_odd;
	else {
		p->weave_toggle = !p->weave_toggle;
		odd_field = p->weave_toggle;
	}
	if (p->fields_woven & (odd_field ? 0x02 : 0x01))
		odd_field = !odd_field;

	/*
	 * Source: this field is staged at the top of p->frame.  Destination:
	 * interleave into p->weave at alternating rows.  Because the target
	 * row (2y or 2y+1) is always >= the source row y, a forward copy is
	 * safe: no source line is overwritten before it has been read.
	 */
	for (y = 0; y < field_lines; y++) {
		u32 dst_line = odd_field ? (y * 2 + 1) : (y * 2);

		memcpy(p->weave + dst_line * stride_words * 4,
		       p->frame + y * stride_words * 4,
		       stride_words * 4);
	}
	p->fields_woven |= odd_field ? 0x02 : 0x01;

	if (p->fields_woven == 0x03) {
		p->fields_woven = 0;
		/* keep only the (already interleaved) half-height pair */
		avm_note_frame(p);
		avm_deliver_frame(dev, p->weave, frame_words);
	}
}

/*
 * Reconcile a completed C1 frame/field of `have` words against the locked
 * mode's target.  Vendor behavior: the official driver pre-fills its frame
 * buffer with AVM_PAD_WORD (studio black) and copies the received raster
 * over it, so a frame that lost lines (HDMI re-lock, transient FPGA
 * underrun) is delivered black-padded instead of discarded.  The mode
 * detector has already refused word counts outside the mode's plausible
 * window, so anything that reaches here is a real frame for this mode.
 *
 * Presents exactly want = mode->target words (the geometry consumers were
 * told to expect): a short frame gets a black tail, an over-long frame is
 * truncated.  Returns the word count to present, or 0 for an
 * unrepresentable target.
 */
static u32 avm_reconcile_frame(struct avm_device *dev,
			       const struct avm_mode *mode, u32 have)
{
	struct avm_parser *p = &dev->parser;
	u32 want = mode->target;
	u32 *dst;
	u32 k;

	if (!want || want > AVM_MAX_FRAME_WORDS)
		return 0;

	if (have < want) {
		dst = (u32 *)p->frame;
		for (k = have; k < want; k++)
			dst[k] = AVM_PAD_WORD;
		atomic_inc(&p->padded_frames);
		dev_info_ratelimited(&dev->udev->dev,
			"short frame: %u/%u words, black-padded\n",
			have, want);
	}
	/* have >= want: any tail beyond the target is transport damage. */
	return want;
}

/*
 * Process one C1 frame end.  Frames that land in the locked mode's
 * word-count window are presented at the mode geometry: short frames (HDMI
 * re-lock, transient FPGA underrun) are black-padded with AVM_PAD_WORD to
 * match the vendor, over-long frames are truncated.  Word counts outside
 * every mode window are transport damage and dropped (CV-23).
 */
static void avm_handle_frame_end(struct avm_device *dev, u32 frame_words)
{
	struct avm_parser *p = &dev->parser;
	const struct avm_mode *mode;
	bool mode_w_changed = true;
	/* The FPGA's own C0 scan-mode flag (b2 & 0x80) is the authoritative
	 * interlace source; the status worker's STDI bit is the fallback. */
	bool interlaced;
	u32 present;

	if (!frame_words) {
		atomic_inc(&p->dropped_frames);
		return;
	}

	interlaced = (p->field_flags & 0x80) != 0;
	mode = avm_detect_mode(dev, frame_words, interlaced);
	if (!mode) {
		atomic_inc(&p->corrupt_frames);
		atomic_inc(&p->dropped_frames);
		return;
	}

	/* Real video data reached a recognizable frame: refresh the liveness
	 * stamp (diagnostic; a dying input stops updating it, transitional
	 * drops still count as live data). */
	WRITE_ONCE(p->last_real_kt, ktime_get());

	/* Mode switch hysteresis: 4 consecutive same-geometry frames. */
	if (READ_ONCE(p->active_w) == mode->w &&
	    READ_ONCE(p->active_h) == mode->h) {
		p->pending_count = 0;
		mode_w_changed = false;
	} else {
		if (p->pending_w == mode->w && p->pending_h == mode->h)
			p->pending_count++;
		else {
			p->pending_w = mode->w;
			p->pending_h = mode->h;
			p->pending_count = 1;
		}
		if (p->pending_count >= 4) {
			/* Cross-context writers: avm_live_geometry() (ioctl)
			 * and the cvx8897 worker READ_ONCE these, so the
			 * parser must publish them atomically. */
			WRITE_ONCE(p->active_w, mode->w);
			WRITE_ONCE(p->active_h, mode->h);
			WRITE_ONCE(p->active_interlaced, mode->interlaced);
			p->pending_count = 0;
			WRITE_ONCE(p->mode_serial, READ_ONCE(p->mode_serial) + 1);
			/* tell consumers to re-negotiate the format */
			avm_source_change(dev);
			mode_w_changed = false;
		}
	}

	/* If the mode is not locked yet this frame is transitional: drop. */
	if (mode_w_changed) {
		atomic_inc(&p->dropped_frames);
		return;
	}

	/* Interlaced word counts are FIELD words (half the frame height). */
	present = avm_reconcile_frame(dev, mode, frame_words);
	if (!present) {
		atomic_inc(&p->corrupt_frames);
		atomic_inc(&p->dropped_frames);
		return;
	}

	if (mode->interlaced) {
		avm_weave_field(dev, present);
		return;
	}

	avm_note_frame(p);
	avm_deliver_frame(dev, p->frame, present);
}

void avm_parser_feed(struct avm_device *dev, u32 *d, u32 count)
{
	struct avm_parser *p = &dev->parser;
	u32 i = 0;

	if (count == 0)
		return;

	/*
	 * CV-01: prepend words held back from the previous chunk so markers
	 * that straddle a chunk boundary (C0, C1 trailer, audio header) are
	 * still parsed instead of being discarded.  parse_buf carries 3 words
	 * of headroom exactly for this shift, so count can only grow to
	 * AVM_CHUNK_WORDS + 3 <= capacity.
	 */
	if (p->hold_n) {
		memmove(d + p->hold_n, d, count * sizeof(u32));
		memcpy(d, p->hold, p->hold_n * sizeof(u32));
		count += p->hold_n;
		p->hold_n = 0;
	}

	/* Audio continuation across a chunk boundary: keep feeding the PCM. */
	if (p->in_audio) {
		u32 start = i;
		u32 take = min_t(u32, p->remaining_audio_words, count);

		i += take;
		p->remaining_audio_words -= take;
		avm_audio_push(dev, (const u8 *)(d + start), take * 4);
		if (p->remaining_audio_words == 0) {
			/* payload complete: back to padding, then markers */
			p->in_audio = false;
			if (i < count)
				p->in_audio_padding = true;
			else
				return;
		} else {
			return; /* payload continues into the next chunk */
		}
	}

	/* Initial lock hunt: discard data before the first valid C0 header. */
	if (!p->stream_locked) {
		while (i + 1 < count) {
			u32 meta, b0, b1, b2, b3, chk;

			if (d[i] != AVM_MARKER_C0) {
				i++;
				continue;
			}
			meta = d[i + 1];
			b0 = meta & 0xFF;
			b1 = (meta >> 8) & 0xFF;
			b2 = (meta >> 16) & 0xFF;
			b3 = (meta >> 24) & 0xFF;
			chk = (b0 + b1 + b2 - 0x40) & 0xFF;
			if (b1 == 0x01 && chk == b3) {
				p->last_seq = (u8)b0;
				p->have_seq = true;
				p->field_flags = (u32)b2;
				p->in_video = true;
				p->frame_words = 0;
				p->fields_woven = 0;
				p->stream_locked = true;
				i += 2;
				break;
			}
			i++;
		}
		if (!p->stream_locked) {
			/* last word may be the first half of a C0 pair */
			if (i < count)
				avm_hold(p, d, i, count);
			return;
		}
	}

	/* Main parser loop */
	while (i < count) {
		/* Skip audio padding up to the next marker */
		if (p->in_audio_padding) {
			while (i < count && d[i] == AVM_MARKER_AUDIO_PAD)
				i++;
			if (i < count)
				p->in_audio_padding = false;
			else
				break;
		}

		/* C0: new video frame / field */
		if (d[i] == AVM_MARKER_C0) {
			u32 meta, b0, b1, b2, b3, chk;

			if (i + 1 >= count) {
				avm_hold(p, d, i, count);
				return;
			}
			meta = d[i + 1];
			b0 = meta & 0xFF;
			b1 = (meta >> 8) & 0xFF;
			b2 = (meta >> 16) & 0xFF;
			b3 = (meta >> 24) & 0xFF;
			chk = (b0 + b1 + b2 - 0x40) & 0xFF;
			if (b1 == 0x01 && chk == b3) {
				if (p->have_seq && b0 != avm_next_seq(p->last_seq))
					atomic_inc(&p->resync_count);
				p->last_seq = (u8)b0;
				p->have_seq = true;
				p->field_flags = (u32)b2;
				if (p->frame_words > 0) {
					/* prior frame never reached C1 */
					atomic_inc(&p->dropped_frames);
					p->fields_woven = 0;
				}
				p->frame_words = 0;
				p->in_video = true;
				p->in_audio = false;
				p->in_audio_padding = false;
				i += 2;
				continue;
			}
			/* Invalid C0: cannot trust this boundary; drop partial frame */
			atomic_inc(&p->corrupt_frames);
			p->frame_words = 0;
			p->in_video = false;
			p->fields_woven = 0;
			i += 2;
			continue;
		}

		/* C1: frame end */
		if (d[i] == AVM_MARKER_C1) {
			u32 t1, t2, tb0, tb1, tb2, tb3, b4, expected;
			bool trailer_valid;

			if (i + 2 >= count) {
				avm_hold(p, d, i, count);
				return;
			}
			t1 = d[i + 1];
			tb0 = t1 & 0xFF;
			tb1 = (t1 >> 8) & 0xFF;
			tb2 = (t1 >> 16) & 0xFF;
			tb3 = (t1 >> 24) & 0xFF;
			t2 = d[i + 2];
			b4 = t2 & 0xFF;
			expected = (tb0 + tb1 + tb2 + tb3 - 0x3F) & 0xFF;

			trailer_valid = (tb1 == 0x02 && expected == b4 &&
					 (!p->have_seq || tb0 == p->last_seq));

			/* FPGA HDMI clock/PLL recovering flag (tb2 bit 4): drop. */
			if (trailer_valid && (tb2 & 0x10)) {
				atomic_inc(&p->dropped_frames);
				p->frame_words = 0;
				p->in_video = false;
				p->fields_woven = 0;
				i += 3;
				continue;
			}

			if (trailer_valid && p->in_video && p->frame_words > 0) {
				avm_handle_frame_end(dev, p->frame_words);
			} else if (p->in_video && p->frame_words > 0) {
				/* lone / bad-trailer C1: end frame, discard it */
				atomic_inc(&p->corrupt_frames);
				atomic_inc(&p->dropped_frames);
			}
			p->frame_words = 0;
			p->in_video = false;
			p->fields_woven = 0;

			/* marker + trailer words */
			i += 3;
			continue;
		}

		/* Audio packet */
		if (d[i] == AVM_MARKER_AUDIO) {
			u32 raw, audio_bytes;

			if (i + 1 >= count) {
				avm_hold(p, d, i, count);
				return;
			}
			raw = d[i + 1];
			audio_bytes = (((raw >> 16) & 0xFF) << 8) |
				       ((raw >> 24) & 0xFF);
			if (audio_bytes >= 512 && audio_bytes <= 8192 &&
			    (audio_bytes & 3) == 0) {
				u32 audio_words = audio_bytes / 4;
				u32 avail = count - (i + 2);

				i += 2;
				if (avail >= audio_words) {
					avm_audio_push(dev,
						(const u8 *)(d + i),
						audio_bytes);
					i += audio_words;
					p->in_audio_padding = true;
				} else {
					/* payload spills into the next chunk:
					 * feed the part we have, resume above */
					avm_audio_push(dev,
						(const u8 *)(d + i),
						avail * 4);
					p->in_audio = true;
					p->remaining_audio_words =
						audio_words - avail;
					i = count;
				}
				continue;
			}
			/* Invalid header length: absorb as a video word if framing */
			if (p->in_video && p->frame_words < AVM_MAX_FRAME_WORDS)
				p->frame_words++;
			i++;
			continue;
		}

		/* Plain video words: slice up to the next marker. */
		{
			u32 start = i;
			u32 slice;

			while (i < count && d[i] != AVM_MARKER_C0 &&
			       d[i] != AVM_MARKER_C1 &&
			       d[i] != AVM_MARKER_AUDIO)
				i++;
			slice = i - start;

			if (p->in_video && slice) {
				u32 room;

				if (p->frame_words >= AVM_MAX_FRAME_WORDS)
					continue;
				room = AVM_MAX_FRAME_WORDS - p->frame_words;
				if (slice > room)
					slice = room;
				memcpy(p->frame + p->frame_words * 4,
				       d + start, slice * 4);
				p->frame_words += slice;
			}
		}
	}
}