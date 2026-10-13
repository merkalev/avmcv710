// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-status.c - ADV7604 receiver polling and standby overlay pump.
 *
 * The official Windows/macOS driver polls the ADV7604 input receiver (~10 Hz)
 * to learn whether a source is attached, and when it is not - or its raster
 * cannot be captured - it shows one of the AVerMedia standby bitmaps.  The
 * vendor overlays those host-side in its renderer; a bare V4L2 node has no
 * renderer, so this driver synthesizes the same image as a captured frame:
 * the 640x480 bitmap is composited (centred, on a limited-range black
 * background) into the current raster and handed to the parser's delivery
 * path, so a consumer sees a correctly sized "no signal" / "out of range"
 * frame at the geometry it already negotiated.
 *
 *   - a per-device delayed work ticks at ~30 Hz; every 3rd tick it reads
 *     the ADV7604 core registers over the FX3 I2C bridge and publishes a
 *     struct avm_signal snapshot (the same reads as the userspace driver,
 *     verified against drivers/media/i2c/adv7604.c):
 *       IO  map 0x20 0x6A : TMDS lock
 *       IO  map 0x20 0x12 : STDI interlace flag (bit 4)
 *       HDMI map 0x34 0x07 : 12-bit active width
 *       HDMI map 0x34 0x09 : 12-bit active height
 *   - while streaming, if no real frame data has been seen for a full idle
 *     window, the same work re-blits the matching overlay into the oldest
 *     queued vb2 buffer (paced at the tick rate).  Real frames simply take
 *     over again as soon as they arrive: the parser stamps last_real_kt on
 *     every valid frame, so the splash never fights live video.
 *
 * Streaming gating: the work is armed by avm_status_start() from
 * start_streaming() *after* advanced work mode is enabled, and cancelled by
 * avm_status_stop() from stop_streaming()/disconnect().  It therefore never
 * touches the receiver I2C while the FPGA is idle - polling from probe was
 * what wedged EP1 in the earlier adaptive-geometry attempt.  The work also
 * stops re-arming itself as soon as dev->streaming clears, and
 * cancel_delayed_work_sync() is safe against that self-requeue.
 *
 * All EP1/I2C transactions are serialized by dev->ctrl_lock inside the hw
 * helpers, so this worker can run concurrently with start/stop without
 * corrupting the control pipe (the CV-03 class of bug).
 */
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "avmcv710.h"
#include "avmcv710-bmp.h"

/* Ticks: the work fires every 33 ms (~30 Hz) and pumps one overlay frame
 * per tick; the ADV7604 poll runs every 3rd tick (~10 Hz). */
#define AVM_STATUS_PERIOD_MS	33

/* Only show an overlay after the stream has been dead this long - short
 * gaps (mode re-lock, transient USB hiccup) must not flash the splash. */
#define AVM_SPLASH_IDLE_MS	250

/* Poll interval in ticks. */
#define AVM_POLL_EVERY		3

/* YUY2 limited-range black: Y=16, U=V=128 per macropixel. */
#define AVM_YUY2_BLACK		0x80108010u

/* Overlay lookup: returns the 640x480 YUY2 raster for a splash kind. */
const u8 *avm_splash_bmp(enum avm_splash kind, u32 *words)
{
	const u8 *bmp;

	switch (kind) {
	case AVM_SPLASH_NO_SIGNAL:
		bmp = avm_bmp_no_signal;
		break;
	case AVM_SPLASH_OUT_OF_RANGE:
		bmp = avm_bmp_out_of_range;
		break;
	case AVM_SPLASH_HDCP:
		bmp = avm_bmp_hdcp;
		break;
	case AVM_SPLASH_CONTENT_PROTECTION:
		bmp = avm_bmp_content_protection;
		break;
	default:
		return NULL;
	}
	if (words)
		*words = AVM_BMP_WORDS;
	return bmp;
}

static void avm_poll_signal(struct avm_device *dev)
{
	struct avm_signal sig;
	u8 io_lock = 0, io_flags = 0;
	u8 wb[2] = { 0, 0 }, hb[2] = { 0, 0 };
	u8 cs = 0;
	bool lock_change, prev_locked;
	unsigned long flags;
	int fails = 0;

	fails += avm_i2c_read_block(dev, 0x20, 0x6A, &io_lock, 1);
	fails += avm_i2c_read_block(dev, 0x20, 0x12, &io_flags, 1);
	fails += avm_i2c_read_block(dev, 0x34, 0x07, wb, 2);
	fails += avm_i2c_read_block(dev, 0x34, 0x09, hb, 2);

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (fails) {
		/* keep the last known good snapshot, count the miss */
		dev->sig.poll_failures++;
		spin_unlock_irqrestore(&dev->sig_lock, flags);
		return;
	}

	memset(&sig, 0, sizeof(sig));
	sig.valid = true;
	sig.poll_failures = 0;
	/* no_signal() semantics from Linux adv7604.c: IO 0x6a 0xe0 mask is
	 * the TMDS PLL lock, and bit 4 the per-port signal detect. */
	sig.tmds_locked = (io_lock & 0xE0) == 0xE0 && (io_lock & 0x10) != 0;
	sig.interlaced = (io_flags & 0x10) != 0;
	sig.w = ((wb[0] & 0x0F) << 8) | wb[1];
	sig.h = ((hb[0] & 0x0F) << 8) | hb[1];

	/* Optional colorspace read (HDMI 0x34 reg 0x53, low nibble <= 1 is RGB) */
	if (avm_i2c_read_block(dev, 0x34, 0x53, &cs, 1) == 0) {
		sig.colorspace = cs & 0x0F;
		sig.rgb = (sig.colorspace <= 0x01);
	} else {
		sig.colorspace = 0xFF;
		sig.rgb = false;
	}

	/* one-shot telemetry on lock transitions, so the poll is visible in
	 * the kernel log without spamming every 100 ms (logged after the
	 * spinlock is dropped - printk should not run IRQ-disabled) */
	prev_locked = dev->sig.valid && dev->sig.tmds_locked;
	lock_change = prev_locked != sig.tmds_locked;

	dev->sig = sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	if (lock_change && sig.tmds_locked)
		dev_info(&dev->udev->dev, "signal: TMDS locked %ux%u%s (%s)\n",
			 sig.w, sig.h, sig.interlaced ? " (interlaced)" : "",
			 sig.rgb ? "RGB" : "YCbCr");
	else if (lock_change && !sig.tmds_locked)
		dev_info(&dev->udev->dev, "signal: TMDS lost\n");
}

/* Does the measured geometry produce stageable, deliverable frames? */
static bool avm_signal_ok(const struct avm_signal *sig)
{
	if (!sig->valid)
		return false;
	if (!sig->tmds_locked)
		return false;
	if (sig->w < 320 || sig->w > 4095 || sig->h < 120 || sig->h > 2048)
		return false;
	/* frame must fit the staging buffers or it can never be delivered */
	if ((u64)sig->w * sig->h / 2 > AVM_MAX_FRAME_WORDS)
		return false;
	return true;
}

/*
 * Composite the overlay bitmap for `kind` into a w x h YUY2 raster held in
 * dev->splash_frame.  Cached by (kind, w, h): the raster is rebuilt only
 * when the overlay kind or the live geometry changes, so the steady-state
 * pump is just a re-delivery of the already-built buffer.
 */
static u32 avm_splash_compose(struct avm_device *dev, enum avm_splash kind,
			      u32 w, u32 h)
{
	const u8 *bmp;
	u32 bmp_words, words, x, y, cw, ch, row;
	u32 dst_stride = w / 2;		/* YUY2 words per output line */
	u32 src_stride = AVM_BMP_W / 2;	/* YUY2 words per bitmap line */
	u8 *dst = dev->splash_frame;

	bmp = avm_splash_bmp(kind, &bmp_words);
	if (!bmp || !dst || !w || !h || (w & 1))
		return 0;

	words = (u32)((u64)w * h / 2);
	if (words > AVM_MAX_FRAME_WORDS)
		return 0;	/* unreachable: geometry is bounded above */

	/* limited-range black background */
	for (row = 0; row < words; row++)
		((u32 *)dst)[row] = AVM_YUY2_BLACK;

	/* YUY2 blits must land on a macropixel (2-pixel) boundary */
	x = w > AVM_BMP_W ? (w - AVM_BMP_W) / 2 : 0;
	x &= ~1u;
	y = h > AVM_BMP_H ? (h - AVM_BMP_H) / 2 : 0;
	cw = min(w - x, AVM_BMP_W);	/* pixels, even by construction */
	ch = min(h - y, AVM_BMP_H);	/* lines */

	for (row = 0; row < ch; row++) {
		const u32 *s = (const u32 *)bmp + row * src_stride;
		u32 *d = (u32 *)dst + (y + row) * dst_stride + x / 2;

		memcpy(d, s, cw * 2);
	}

	dev->splash_cached = kind;
	dev->splash_w = w;
	dev->splash_h = h;
	dev->splash_words = words;
	return words;
}

static void avm_splash_tick(struct avm_device *dev)
{
	struct avm_parser *p = &dev->parser;
	struct avm_signal sig;
	enum avm_splash kind;
	u32 w, h;
	unsigned long flags;
	ktime_t idle;

	if (!READ_ONCE(dev->streaming)) {
		dev->splash = AVM_SPLASH_NONE;
		return;
	}

	spin_lock_irqsave(&dev->sig_lock, flags);
	sig = dev->sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	idle = ktime_sub(ktime_get(), READ_ONCE(p->last_real_kt));
	if (ktime_to_ms(idle) < AVM_SPLASH_IDLE_MS) {
		if (dev->splash != AVM_SPLASH_NONE) {
			dev_info(&dev->udev->dev,
				 "video resumed; overlay cleared\n");
			dev->splash = AVM_SPLASH_NONE;
		}
		return;
	}

	/* Stream is silent: pick the overlay that matches the receiver. */
	if (!sig.valid || !sig.tmds_locked)
		kind = AVM_SPLASH_NO_SIGNAL;
	else if (!avm_signal_ok(&sig))
		kind = AVM_SPLASH_OUT_OF_RANGE;
	else
		kind = AVM_SPLASH_NO_SIGNAL;	/* locked but dark */

	avm_live_geometry(dev, &w, &h);
	if (kind != dev->splash_cached || w != dev->splash_w ||
	    h != dev->splash_h) {
		if (!avm_splash_compose(dev, kind, w, h))
			return;
	}
	if (kind != dev->splash) {
		dev_info(&dev->udev->dev, "no video: showing %s overlay %ux%u\n",
			 kind == AVM_SPLASH_NO_SIGNAL ? "no-signal"
						      : "out-of-range",
			 w, h);
		dev->splash = kind;
	}

	if (dev->splash_frame && dev->splash_words)
		avm_deliver_frame(dev, dev->splash_frame, dev->splash_words);
}

static void avm_status_work(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device,
					      status_work.work);

	/* Stop the chain as soon as streaming ends; start_streaming re-arms. */
	if (!READ_ONCE(dev->streaming))
		return;

	if ((dev->status_tick++ % AVM_POLL_EVERY) == 0)
		avm_poll_signal(dev);
	avm_splash_tick(dev);

	queue_delayed_work(system_dfl_wq, &dev->status_work,
			   msecs_to_jiffies(AVM_STATUS_PERIOD_MS));
}

int avm_status_init(struct avm_device *dev)
{
	spin_lock_init(&dev->sig_lock);
	dev->sig.valid = false;
	dev->splash = AVM_SPLASH_NONE;
	dev->splash_cached = AVM_SPLASH_NONE;
	dev->splash_w = 0;
	dev->splash_h = 0;
	dev->splash_words = 0;
	dev->status_tick = 0;
	/* Initialize the work before anything that can fail, so a failed
	 * init followed by avm_status_fini() cancels a valid work struct. */
	INIT_DELAYED_WORK(&dev->status_work, avm_status_work);
	dev->status_inited = true;
	/* one AVM_FRAME_BPF scratch for the composited overlay */
	dev->splash_frame = kvzalloc(AVM_FRAME_BPF, GFP_KERNEL);
	if (!dev->splash_frame)
		return -ENOMEM;
	return 0;
}

/* Arm the poll/splash pump.  Called from start_streaming() once the FPGA is
 * producing, so no receiver I2C is issued while the link is idle. */
void avm_status_start(struct avm_device *dev)
{
	dev->status_tick = 0;
	dev->splash = AVM_SPLASH_NONE;
	dev->splash_cached = AVM_SPLASH_NONE;	/* force a fresh composite */
	queue_delayed_work(system_dfl_wq, &dev->status_work,
			   msecs_to_jiffies(AVM_STATUS_PERIOD_MS));
}

void avm_status_stop(struct avm_device *dev)
{
	cancel_delayed_work_sync(&dev->status_work);
}

void avm_status_fini(struct avm_device *dev)
{
	/* Probe error paths that fail before avm_status_init() must not
	 * cancel a zeroed work struct (CONFIG_DEBUG_OBJECTS_WORK WARNs). */
	if (!dev->status_inited)
		return;
	dev->status_inited = false;
	avm_status_stop(dev);
	kvfree(dev->splash_frame);
	dev->splash_frame = NULL;
}
