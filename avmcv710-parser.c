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
 *   - frame word counts are gated: a frame whose size does not match the
 *     locked mode geometry is dropped, never padded or presented skewed
 *     (CV-23 rule)
 *   - audio packets are parsed and skipped so the video raster stays
 *     aligned; there is no ALSA sink yet (future work), audio bytes are
 *     counted and discarded
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
struct avm_mode {
	u32 w, h, target;
	const char *name;
	bool interlaced;
};

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
 * Map an observed frame/field word count to a mode.  The original
 * userspace fallback table is kept verbatim (including the two 720p bands;
 * the 1080i field band and the 526k-545k 720p band only touch near their
 * edges and the userspace driver accepted that overlap for years).
 */
static const struct avm_mode *avm_detect_mode(u32 words)
{
	unsigned int i;

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
	atomic_set(&p->valid_frames, 0);
	atomic_set(&p->dropped_frames, 0);
	atomic_set(&p->corrupt_frames, 0);
	atomic_set(&p->resync_count, 0);
}

/* Deliver a completed frame into the oldest queued vb2 buffer (if any). */
static void avm_deliver_frame(struct avm_device *dev, const u8 *src, u32 words)
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
		avm_deliver_frame(dev, p->weave, frame_words);
	}
}

/*
 * Process one C1 frame end.  Delivers only frames that exactly match the
 * locked mode geometry; everything else is transport damage (CV-23).
 */
static void avm_handle_frame_end(struct avm_device *dev, u32 frame_words)
{
	struct avm_parser *p = &dev->parser;
	const struct avm_mode *mode;
	bool mode_w_changed = true;

	if (!frame_words) {
		atomic_inc(&p->dropped_frames);
		return;
	}

	mode = avm_detect_mode(frame_words);
	if (!mode) {
		atomic_inc(&p->corrupt_frames);
		atomic_inc(&p->dropped_frames);
		return;
	}

	/* Mode switch hysteresis: 4 consecutive same-geometry frames. */
	if (p->active_w == mode->w && p->active_h == mode->h) {
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
			p->active_w = mode->w;
			p->active_h = mode->h;
			p->pending_count = 0;
			p->mode_serial++;
			mode_w_changed = false;
		}
	}

	/* If the mode is not locked yet this frame is transitional: drop. */
	if (mode_w_changed) {
		atomic_inc(&p->dropped_frames);
		return;
	}

	if (mode->interlaced) {
		/* interlaced word counts are FIELD words (CV-23 exact-size gate) */
		if (frame_words != mode->target) {
			atomic_inc(&p->corrupt_frames);
			atomic_inc(&p->dropped_frames);
			return;
		}
		avm_weave_field(dev, frame_words);
		return;
	}

	if (frame_words != mode->target) {
		atomic_inc(&p->corrupt_frames);
		atomic_inc(&p->dropped_frames);
		return;
	}

	avm_deliver_frame(dev, p->frame, frame_words);
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

	/* Audio continuation across a chunk boundary (payload skipped). */
	if (p->in_audio) {
		u32 take = min_t(u32, p->remaining_audio_words, count);

		i += take;
		p->remaining_audio_words -= take;
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
	if (!p->in_video && p->frame_words == 0) {
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
				i += 2;
				break;
			}
			i++;
		}
		if (!p->in_video) {
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
				i += 2;
				if (i < count && (d[i] < 0x10000 ||
						  (d[i] & 0xFF000000) == 0))
					i++;
				continue;
			}

			if (trailer_valid && p->in_video && p->frame_words >= 40000) {
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
			if (audio_bytes >= 512 && audio_bytes <= 8192) {
				u32 audio_words = audio_bytes / 4;
				u32 avail = count - (i + 2);

				i += 2;
				if (avail >= audio_words) {
					i += audio_words;
					p->in_audio_padding = true;
				} else {
					/* payload spills into the next chunk */
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