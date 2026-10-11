// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-usb.c - USB probe / disconnect and URB streaming pipeline.
 *
 * Streaming model:
 *   - STREAMON (avm_start_streaming in video.c) allocates AVM_NUM_URBs URBs,
 *     each with an AVM_CHUNK_BYTES buffer (the FX3 FPGA DMA chunk size),
 *     anchors them, then submits all.  FPGA work mode is armed and FX3
 *     stream DMA reset before the first read so the FIFO starts clean.
 *   - the URB completion callback runs in atomic context: it only chains
 *     the completed avm_urb onto dev->drain_list and schedules the parser
 *     workqueue.  Nothing else happens in interrupt context.
 *   - the workqueue (process context) drains the completed URBs into the
 *     framing parser, then resubmits them.  Resubmission is conditional on
 *     dev->streaming, so stop_streaming() can never race a resubmit.
 *   - stop_streaming() clears streaming, kills every anchored URB
 *     (usb_kill_anchored_urbs), flushes the workqueue, then frees the
 *     buffers.  disconnect() additionally unregisters the v4l2 device.
 */
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/videobuf2-v4l2.h>

#include "avmcv710.h"

/*
 * Consecutive completion errors after which a single URB is retired from the
 * pool.  Transient bulk errors recover well before this; a persistently
 * failing endpoint stops being resubmitted instead of spinning the worker.
 */
#define AVM_URB_MAX_ERRORS	8

/*
 * The FX3 occasionally answers an EP1 read short/-EAGAIN in the first
 * milliseconds after enumeration; replaying the whole (idempotent) bootstrap is
 * the userspace driver's recovery, so allow a few attempts.
 */
#define AVM_BOOT_ATTEMPTS	3

static void avm_free_urbs(struct avm_device *dev);
static void avm_watchdog(struct work_struct *work);

/*
 * Return every queued-but-unfilled buffer to vb2.
 *
 * The state matters: stop_streaming() and disconnect must hand buffers back
 * as VB2_BUF_STATE_ERROR, but a *failed* start_streaming() must hand them
 * back as VB2_BUF_STATE_QUEUED (see the start_streaming() contract in
 * videobuf2-core.h).  Using ERROR there puts them on q->done_list and trips
 * WARN_ON(!list_empty(&q->done_list)) in vb2_start_streaming().
 */
void avm_return_buffers(struct avm_device *dev, enum vb2_buffer_state state)
{
	struct avm_buffer *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(bufq);

	spin_lock_irqsave(&dev->urbs_lock, flags);
	list_splice_init(&dev->vb_bufs, &bufq);
	dev->pending_bufs = 0;
	spin_unlock_irqrestore(&dev->urbs_lock, flags);

	list_for_each_entry_safe(buf, tmp, &bufq, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

/* Final teardown - called from the vdev release callback, i.e. only after
 * every open file handle on the video node has been closed. */
void avm_usb_release(struct avm_device *dev)
{
	avm_cvx8897_fini(dev);
	destroy_workqueue(dev->wq);
	avm_free_urbs(dev);
}

/* ------------------------------------------------------------------ */
/* Parser workqueue                                                    */

static void avm_parse_work(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device,
					      parse_work);
	unsigned long flags;
	struct avm_urb *u, *tmp;
	LIST_HEAD(batch);

	spin_lock_irqsave(&dev->urbs_lock, flags);
	list_splice_init(&dev->drain_list, &batch);
	spin_unlock_irqrestore(&dev->urbs_lock, flags);

	if (list_empty(&batch))
		return;

	/*
	 * Feed every completed chunk into the parser while streaming.
	 * No stream_lock here: stop_streaming() (which is called with
	 * q->lock held) flushes this workqueue, so taking the mutex here
	 * would self-deadlock.  Parser state is only written by this
	 * worker and reset by start_streaming() after stop_streaming()
	 * has flushed it - the vb2 queue serializes those transitions.
	 */
	if (READ_ONCE(dev->streaming)) {
		list_for_each_entry(u, &batch, done) {
			u32 words;

			/* an errored URB holds no valid chunk */
			if (u->err)
				continue;
			words = u->urb->actual_length / 4;
			if (words > AVM_CHUNK_WORDS)
				words = AVM_CHUNK_WORDS;
			avm_parser_feed(dev, (u32 *)u->parse_buf, words);
		}
	}

	/* Resubmit (only while streaming).  Dead URBs just drop here; the
	 * stop path kills whatever is anchored and frees the rest. */
	list_for_each_entry_safe(u, tmp, &batch, done) {
		int ret;

		list_del_init(&u->done);
		if (!READ_ONCE(dev->streaming))
			continue;

		/*
		 * A transient bulk error (overflow, babble, timeout) must not
		 * permanently retire one of the few URBs, but a persistently
		 * failing endpoint must not spin either: keep resubmitting up
		 * to the per-URB retry cap, then leave it out of the pool.
		 */
		if (u->err) {
			if (u->errors < 3)
				dev_warn_ratelimited(&dev->udev->dev,
					"video URB status %d (error %u)\n",
					u->urb->status, u->errors + 1);
			if (++u->errors > AVM_URB_MAX_ERRORS) {
				dev_warn_ratelimited(&dev->udev->dev,
					"bulk URB failing repeatedly; retiring one URB\n");
				continue;
			}
		} else {
			u->errors = 0;
		}
		u->err = false;

		usb_anchor_urb(u->urb, &dev->urb_anchor);
		ret = usb_submit_urb(u->urb, GFP_KERNEL);
		if (ret) {
			/* endpoint halted / disconnect: leave it unanchored
			 * and unarmed; the pipeline simply goes quiet and
			 * stop_streaming() reclaims the buffer. */
			usb_unanchor_urb(u->urb);
		}
	}
}

/* URB completion - atomic context only: chain + kick the worker.  Every
 * completion is chained, errors included: the worker decides whether to
 * parse (clean chunks only) and to resubmit (while streaming, up to a retry
 * cap).  A single transient bulk error must not silently shrink the URB
 * pool until the stream dies. */
static void avm_urb_complete(struct urb *urb)
{
	struct avm_urb *u = urb->context;
	struct avm_device *dev = u->dev;
	unsigned long flags;

	/* A completed EP 0x83 chunk means the FPGA produced data (even an
	 * errored completion broke the silence - the watchdog keys off
	 * completions, not successful payloads). */
	atomic64_inc(&dev->urb_chunks);

	u->err = urb->status != 0;

	spin_lock_irqsave(&dev->urbs_lock, flags);
	list_add_tail(&u->done, &dev->drain_list);
	spin_unlock_irqrestore(&dev->urbs_lock, flags);
	queue_work(dev->wq, &dev->parse_work);
}

/* ------------------------------------------------------------------ */
/* probe / disconnect                                                  */

static int avm_init_urbs(struct avm_device *dev)
{
	int i;

	for (i = 0; i < AVM_NUM_URBS; i++) {
		struct avm_urb *u = &dev->urbs[i];

		u->dev = dev;
		INIT_LIST_HEAD(&u->done);
		/* 3 words of headroom for CV-01 marker re-merge in the parser */
		u->parse_buf = kmalloc(AVM_CHUNK_BYTES + 3 * sizeof(u32),
				       GFP_KERNEL);
		if (!u->parse_buf)
			goto fail;
		u->transfer = u->parse_buf;

		u->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!u->urb)
			goto fail;

		usb_fill_bulk_urb(u->urb, dev->udev,
				  usb_rcvbulkpipe(dev->udev, AVM_EP_VIDEO_IN),
				  u->transfer, AVM_CHUNK_BYTES,
				  avm_urb_complete, u);
	}
	return 0;

fail:
	while (i--) {
		struct avm_urb *u = &dev->urbs[i];

		if (u->urb) {
			usb_free_urb(u->urb);
			u->urb = NULL;
		}
		kfree(u->parse_buf);
		u->parse_buf = NULL;
		u->transfer = NULL;
	}
	return -ENOMEM;
}

static void avm_free_urbs(struct avm_device *dev)
{
	int i;

	for (i = 0; i < AVM_NUM_URBS; i++) {
		struct avm_urb *u = &dev->urbs[i];

		if (u->urb) {
			usb_free_urb(u->urb);
			u->urb = NULL;
		}
		kfree(u->parse_buf);
		u->parse_buf = NULL;
		u->transfer = NULL;
	}
}

static int avm_usb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct avm_device *dev;
	unsigned int attempt;
	int ret;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(udev);
	dev->intf = intf;
	mutex_init(&dev->stream_lock);
	mutex_init(&dev->ctrl_lock);
	spin_lock_init(&dev->urbs_lock);
	INIT_LIST_HEAD(&dev->drain_list);
	INIT_LIST_HEAD(&dev->vb_bufs);
	init_usb_anchor(&dev->urb_anchor);

	dev->parser.frame = kvzalloc(AVM_FRAME_BPF, GFP_KERNEL);
	dev->parser.weave = kvzalloc(AVM_FRAME_BPF, GFP_KERNEL);
	if (!dev->parser.frame || !dev->parser.weave) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = avm_init_urbs(dev);
	if (ret)
		goto err_free;

	/* Single-threaded, unbounded: parser runs are serialized and never
	 * block a CPU-bound worker pool; explicit flags keep newer kernels
	 * from warning about an ambiguous per-cpu/unbound workqueue. */
	dev->wq = alloc_ordered_workqueue("avmcv710", WQ_MEM_RECLAIM);
	if (!dev->wq) {
		ret = -ENOMEM;
		goto err_urbs;
	}
	INIT_WORK(&dev->parse_work, avm_parse_work);

	ret = avm_cvx8897_init(dev);
	if (ret)
		goto err_wq;

	/*
	 * Replay the 871-command FX3/ADV7604 bring-up (bulk EP1).
	 *
	 * A soft rmmod/modprobe leaves the FX3 powered, so its EP1 data toggle
	 * and any unread reply byte survive into the next probe; the first
	 * bootstrap read then returns a short packet and the whole sequence
	 * desyncs into -EOVERFLOW and a wedged endpoint.  Drain the FIFO, reset
	 * both EP1 toggles, and park the FPGA + reset the stream DMA (the
	 * setFpgaIdle + resetStreamDMA pair the userspace driver brackets the
	 * command list with) before every attempt.  Retry the idempotent
	 * sequence a few times, as the first EP1 transaction after enumeration
	 * can transiently fail.
	 */
	msleep(100);

	for (attempt = 0; attempt < AVM_BOOT_ATTEMPTS; attempt++) {
		/*
		 * Every attempt first drains any unread EP1 reply byte left by
		 * the previous session (a cheap 20 ms read).  Only the recovery
		 * attempts additionally reset the endpoint toggles; CLEAR_FEATURE
		 * costs ~5 s per call on this device, so keep it off the normal
		 * path and drain again after it.
		 */
		if (attempt) {
			avm_ep1_drain(dev);
			usb_clear_halt(udev, usb_rcvbulkpipe(udev, AVM_EP_CTRL_IN));
			usb_clear_halt(udev, usb_sndbulkpipe(udev, AVM_EP_CTRL_OUT));
		}
		avm_ep1_drain(dev);

		avm_fpga_work_mode(dev, false);
		avm_reset_stream_dma(dev);

		ret = avm_bootstrap(dev);
		if (!ret)
			break;
		dev_warn(&udev->dev, "bootstrap attempt %u/%u failed: %d\n",
			 attempt + 1, AVM_BOOT_ATTEMPTS, ret);
		msleep(100);
	}
	if (ret) {
		dev_err(&udev->dev, "bootstrap failed: %d\n", ret);
		goto err_wq;
	}
	avm_fpga_work_mode(dev, false);
	avm_reset_stream_dma(dev);
	avm_parser_reset(dev);

	usb_set_intfdata(intf, dev);

	/*
	 * Stage 0 (video-only bring-up): no receiver poll runs, so the
	 * signal snapshot stays invalid and the parser falls back to its
	 * word-count mode table.  sig_lock must still be live because the
	 * parser and the format probes read dev->sig under it.
	 */
	spin_lock_init(&dev->sig_lock);
	dev->sig.valid = false;

	/* STREAMON watchdog: armed before registration (queue_work only
	 * below, once all failure paths are gone), cancelled on disconnect. */
	atomic64_set(&dev->urb_chunks, 0);
	dev->watch_silent = false;
	INIT_DELAYED_WORK(&dev->watch_work, avm_watchdog);

	ret = avm_video_register(dev);
	if (ret)
		goto err_wq;

	dev_info(&udev->dev,
		 "AVerMedia ExtremeCap U3 (CV710) ready, %u x %u byte URBs\n",
		 AVM_NUM_URBS, AVM_CHUNK_BYTES);
	queue_delayed_work(system_wq, &dev->watch_work, 5 * HZ);
	return 0;

err_wq:
	avm_cvx8897_fini(dev);
	destroy_workqueue(dev->wq);
err_urbs:
	avm_free_urbs(dev);
err_free:
	kvfree(dev->parser.frame);
	kvfree(dev->parser.weave);
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

/*
 * STREAMON watchdog: every 5 s, if streaming is up but no EP 0x83 chunk has
 * completed since the last sample, the pipe is silently idle.  This is the
 * soft-reload no-output signature - a replug reboots the FX3 firmware, a
 * module reload does not, so URBs just sit unfulfilled with no error logged
 * and no endpoint halt to report.  One warn when the silence starts, one
 * info when data resumes.
 */
static void avm_watchdog(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device,
					      watch_work.work);
	u64 chunks = atomic64_read(&dev->urb_chunks);
	bool streaming = READ_ONCE(dev->streaming);

	if (streaming) {
		if (chunks == dev->watch_last_chunks) {
			if (!dev->watch_silent) {
				dev->watch_silent = true;
				dev->watch_silent_at = ktime_get_seconds();
				dev_warn(&dev->udev->dev,
					 "watchdog: STREAMON but no EP0x83 chunk "
					 "completed for 5 s - silent FPGA after "
					 "a soft reload? Replug the device\n");
			}
		} else if (dev->watch_silent) {
			dev->watch_silent = false;
			dev_info(&dev->udev->dev,
				 "watchdog: EP0x83 data resumed after %llu s\n",
				 (u64)(ktime_get_seconds() -
				       dev->watch_silent_at));
		}
	}
	dev->watch_last_chunks = chunks;
	queue_delayed_work(system_wq, &dev->watch_work, 5 * HZ);
}

static void avm_usb_disconnect(struct usb_interface *intf)
{
	struct avm_device *dev = usb_get_intfdata(intf);

	if (!dev)
		return;
	usb_set_intfdata(intf, NULL);

	/*
	 * Stop all I/O immediately.  The vdev release callback (fired once
	 * every open file handle is closed) performs the final teardown -
	 * workqueue destroy / URB free / device free must outlive any user
	 * that is still holding the video node.
	 */
	mutex_lock(&dev->stream_lock);
	dev->streaming = false;
	mutex_unlock(&dev->stream_lock);

	usb_kill_anchored_urbs(&dev->urb_anchor);
	cancel_delayed_work_sync(&dev->watch_work);
	cancel_work_sync(&dev->cvx_work);
	flush_workqueue(dev->wq);

	/*
	 * Unregistering the video device does not stop the vb2 queue, so a
	 * consumer blocked in read()/poll() would otherwise hang until it
	 * times out.  Flag the queue as errored to wake it (poll() reports
	 * POLLERR, read()/DQBUF return -EIO); the queued buffers are then
	 * returned when the file is closed (vb2 cancel -> stop_streaming).
	 * Guarded to streaming so the sticky error flag cannot leak into a
	 * later stream on the same queue.
	 */
	if (vb2_is_streaming(&dev->q))
		vb2_queue_error(&dev->q);

	video_unregister_device(&dev->vdev);
}

static const struct usb_device_id avm_usb_ids[] = {
	{ USB_DEVICE(AVM_USB_VID, AVM_USB_PID) },
	{ },
};
MODULE_DEVICE_TABLE(usb, avm_usb_ids);

static struct usb_driver avm_usb_driver = {
	.name		= "avmcv710",
	.probe		= avm_usb_probe,
	.disconnect	= avm_usb_disconnect,
	.id_table	= avm_usb_ids,
};

module_usb_driver(avm_usb_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("merkalev <merkalev@users.noreply.github.com>");
MODULE_DESCRIPTION("AVerMedia ExtremeCap U3 (CV710) capture - FX3 + ADV7604 + ECP3");
MODULE_VERSION("0.1.0");