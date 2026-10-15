// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-status.c - ADV7604 receiver telemetry polling.
 *
 * Polls the ADV7604 HDMI receiver core registers at a gentle 1 Hz cadence
 * while streaming to monitor TMDS lock, interlace flags, active geometry,
 * and AVI colorspace.
 *
 * Streaming gating:
 * Armed by avm_status_start() from start_streaming() after the FPGA link
 * is running, and stopped by avm_status_stop() on stop_streaming() /
 * disconnect(). It never touches the receiver I2C while the FPGA is idle.
 *
 * All I2C transactions are serialized by dev->ctrl_lock in avmcv710-hw.c.
 */
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "avmcv710.h"

#define AVM_STATUS_POLL_INTERVAL_MS	1000

static void avm_poll_signal(struct avm_device *dev)
{
	struct avm_signal sig;
	u8 io_lock = 0, io_flags = 0;
	u8 wb[2] = { 0, 0 }, hb[2] = { 0, 0 };
	u8 cs = 0;
	bool lock_change, prev_locked;
	unsigned long flags;

	/*
	 * Abort immediately if any core read fails (cv710userspace CV-13).
	 * Never hammer the control endpoint when the FX3 is busy.
	 */
	if (avm_i2c_read_block(dev, 0x20, 0x6A, &io_lock, 1)) {
		dev->sig.poll_failures++;
		return;
	}
	dev->sig.poll_failures = 0;

	if (avm_i2c_read_block(dev, 0x20, 0x12, &io_flags, 1) ||
	    avm_i2c_read_block(dev, 0x34, 0x07, wb, 2) ||
	    avm_i2c_read_block(dev, 0x34, 0x09, hb, 2))
		return;

	/* AVI colorspace (raw HDMI 0x53 low nibble) */
	if (avm_i2c_read_block(dev, 0x34, 0x53, &cs, 1))
		cs = 0;

	sig.valid = true;
	sig.tmds_locked = ((io_lock & 0xE0) == 0xE0) && (io_lock & 0x10);
	sig.interlaced = (io_flags & 0x10) != 0;
	sig.w = ((u32)(wb[0] & 0x0F) << 8) | wb[1];
	sig.h = ((u32)(hb[0] & 0x0F) << 8) | hb[1];
	sig.colorspace = cs & 0x0F;
	sig.rgb = (sig.colorspace <= 0x01);
	sig.poll_failures = 0;

	spin_lock_irqsave(&dev->sig_lock, flags);
	prev_locked = dev->sig.tmds_locked;
	lock_change = !dev->sig.valid || (dev->sig.tmds_locked != sig.tmds_locked);
	dev->sig = sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	if (lock_change) {
		dev_info(&dev->udev->dev,
			 "[ADV7604] TMDS %s (IO 0x6A=0x%02x, 0x12=0x%02x, %ux%u%s cs=0x%02x)\n",
			 sig.tmds_locked ? "LOCKED" : "UNLOCKED",
			 io_lock, io_flags, sig.w, sig.h,
			 sig.interlaced ? "i" : "p", sig.colorspace);
	}
}

static void avm_status_work_fn(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device,
					      status_work.work);

	if (!READ_ONCE(dev->streaming))
		return;

	avm_poll_signal(dev);

	if (READ_ONCE(dev->streaming)) {
		queue_delayed_work(system_dfl_wq, &dev->status_work,
				   msecs_to_jiffies(AVM_STATUS_POLL_INTERVAL_MS));
	}
}

int avm_status_init(struct avm_device *dev)
{
	spin_lock_init(&dev->sig_lock);
	memset(&dev->sig, 0, sizeof(dev->sig));
	INIT_DELAYED_WORK(&dev->status_work, avm_status_work_fn);
	dev->status_inited = true;
	return 0;
}

void avm_status_fini(struct avm_device *dev)
{
	if (!dev->status_inited)
		return;
	cancel_delayed_work_sync(&dev->status_work);
	dev->status_inited = false;
}

void avm_status_start(struct avm_device *dev)
{
	/* Delay initial poll by 1000 ms to let video DMA stabilize */
	queue_delayed_work(system_dfl_wq, &dev->status_work,
			   msecs_to_jiffies(1000));
}

void avm_status_stop(struct avm_device *dev)
{
	cancel_delayed_work_sync(&dev->status_work);
}
