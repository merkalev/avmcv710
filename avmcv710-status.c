// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-status.c - ADV7604 status polling and no-signal/out-of-range
 * overlay pump.
 *
 * The official Windows/macOS drivers poll the ADV7604 receiver (~10 Hz)
 * for TMDS lock and measured geometry, and when the input is absent or
 * unusable they composite a 640x480 overlay bitmap into the stream so the
 * user sees "no signal" / "out of range" instead of a black or frozen
 * frame.  We mirror that here:
 *
 *   - a per-device delayed work ticks at ~30 Hz; every 3rd tick it reads
 *     the ADV7604 core registers over the FX3 I2C bridge and publishes a
 *     struct avm_signal snapshot (the same reads as the userspace driver,
 *     verified against drivers/media/i2c/adv7604.c):
 *       IO  map 0x20 0x6A : TMDS lock
 *       IO  map 0x20 0x12 : STDI interlace flag (bit 4)
 *       HDMI map 0x34 0x07 : 12-bit active width
 *       HDMI map 0x34 0x09 : 12-bit active height
 *   - while streaming, if no real frame data has been seen for a full
 *     idle window, the same work blits the matching overlay into the
 *     oldest queued vb2 buffer (paced at the tick rate).  Real frames
 *     simply take over again as soon as they arrive - the parser stamps
 *     last_real_kt on every valid frame, so the splash never fights live
 *     video.
 *
 * All EP1/I2C transactions are serialized by dev->ctrl_lock inside the
 * hw helpers, so this worker can run concurrently with start/stop without
 * corrupting the control pipe (the CV-03 class of bug).
 *
 * Teardown: avm_status_shutdown() cancel_delayed_work_sync()s the work
 * BEFORE disconnect() starts killing URBs, so no I2C traffic can race the
 * device teardown.
 */
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "avmcv710.h"
#include "avmcv710-bmp.h"

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

/* Ticks: the work fires every 33 ms (~30 Hz) and pumps one overlay frame
 * per tick; the ADV7604 poll runs every 3rd tick (~10 Hz). */
#define AVM_STATUS_PERIOD_MS	33

/* Only show an overlay after the stream has been dead this long - short
 * gaps (mode re-lock, transient USB hiccup) must not flash the splash. */
#define AVM_SPLASH_IDLE_MS	250

/* Poll interval in ticks. */
#define AVM_POLL_EVERY		3

static void avm_poll_signal(struct avm_device *dev)
{
	struct avm_signal sig;
	u8 io_lock = 0, io_flags = 0;
	u8 wb[2] = { 0, 0 }, hb[2] = { 0, 0 };
	unsigned long flags;
	int fails = 0;

	fails += avm_i2c_read_block(dev, 0x20, 0x6A, &io_lock, 1);
	fails += avm_i2c_read_block(dev, 0x20, 0x12, &io_flags, 1);
	fails += avm_i2c_read_block(dev, 0x34, 0x07, wb, 2);
	fails += avm_i2c_read_block(dev, 0x34, 0x09, hb, 2);

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (fails) {
		/* keep last known good snapshot, count the miss */
		dev->sig.poll_failures++;
		spin_unlock_irqrestore(&dev->sig_lock, flags);
		return;
	}

	sig.valid = true;
	sig.poll_failures = 0;
	/* no_signal() semantics from Linux adv7604.c: IO 0x6a lower nibble
	 * is the per-port no-signal flag, 0xe0 mask is TMDS PLL lock. */
	sig.tmds_locked = (io_lock & 0xE0) == 0xE0 && (io_lock & 0x10) != 0;
	sig.interlaced = (io_flags & 0x10) != 0;
	sig.w = ((wb[0] & 0x0F) << 8) | wb[1];
	sig.h = ((hb[0] & 0x0F) << 8) | hb[1];
	dev->sig = sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);
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

static void avm_splash_tick(struct avm_device *dev)
{
	struct avm_parser *p = &dev->parser;
	struct avm_signal sig;
	enum avm_splash kind = AVM_SPLASH_NONE;
	const u8 *bmp;
	u32 words;
	unsigned long flags;
	ktime_t idle;

	spin_lock_irqsave(&dev->sig_lock, flags);
	sig = dev->sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	if (!READ_ONCE(dev->streaming)) {
		dev->splash = AVM_SPLASH_NONE;
		return;
	}

	idle = ktime_sub(ktime_get(), READ_ONCE(p->last_real_kt));
	if (ktime_to_ms(idle) < AVM_SPLASH_IDLE_MS) {
		if (dev->splash != AVM_SPLASH_NONE) {
			dev_info(&dev->udev->dev, "video resumed; overlay cleared\n");
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
		kind = AVM_SPLASH_NO_SIGNAL; /* locked but dark: same as none */

	if (kind != dev->splash) {
		dev_info(&dev->udev->dev, "no video: showing %s overlay\n",
			 kind == AVM_SPLASH_NO_SIGNAL ? "no-signal"
						      : "out-of-range");
		dev->splash = kind;
	}

	bmp = avm_splash_bmp(kind, &words);
	if (bmp)
		avm_deliver_frame(dev, bmp, words);
}

static void avm_status_work(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device,
					      status_work.work);

	if ((dev->status_tick++ % AVM_POLL_EVERY) == 0)
		avm_poll_signal(dev);
	avm_splash_tick(dev);

	schedule_delayed_work(&dev->status_work,
			      msecs_to_jiffies(AVM_STATUS_PERIOD_MS));
}

void avm_status_init(struct avm_device *dev)
{
	spin_lock_init(&dev->sig_lock);
	dev->sig.valid = false;
	dev->splash = AVM_SPLASH_NONE;
	INIT_DELAYED_WORK(&dev->status_work, avm_status_work);
	schedule_delayed_work(&dev->status_work,
			      msecs_to_jiffies(AVM_STATUS_PERIOD_MS));
}

void avm_status_shutdown(struct avm_device *dev)
{
	cancel_delayed_work_sync(&dev->status_work);
}