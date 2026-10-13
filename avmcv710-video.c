// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-video.c - V4L2 capture node for the CV710.
 *
 * A single video capture device: YUY2, auto-detected resolution.  The
 * framing parser delivers whichever geometry the source locks (via the
 * 4-frame hysteresis), so a consumer should watch V4L2_EVENT_SOURCE_CHANGE
 * and re-negotiate the format, or simply poll g_fmt.  Buffer planes are
 * always sized to the largest possible frame (1080p) so a resolution
 * change mid-stream can never overflow a queued buffer; vb2 payload is set
 * to the *actual* delivered frame size per buffer.
 */
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "avmcv710.h"

static inline struct avm_buffer *to_avm_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct avm_buffer, vb);
}

/*
 * Source-paced capture: the geometry is whatever the HDMI source locks.  The
 * FPGA passes the raster through and cannot scale, so a requested size can
 * never be honoured and every format ioctl reports the live geometry
 * (avm_pixfmt()).  enum_framesizes() therefore advertises that one live size.
 *
 * The frame rate is different: the CVX8897 between the receiver and the
 * capture path can halve it in hardware (the vendor's [60p]/[30p]/[50p]/[25p]
 * "Video Input Frame Rate" selector).  When that converter is enabled
 * (avmcv710-cvx8897.c), enum_frameintervals() offers its real menu and S_PARM
 * selects the matching half-rate table; otherwise the source paces the
 * cadence and only the measured interval is honest.
 */

/* ------------------------------------------------------------------ */
/* v4l2 ioctls                                                         */

static int avm_querycap(struct file *file, void *fh,
			struct v4l2_capability *cap)
{
	struct avm_device *dev = video_drvdata(file);

	strscpy(cap->driver, "avmcv710", sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia ExtremeCap U3 (CV710)",
		sizeof(cap->card));
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	/* NOTE: V4L2_CAP_TIMEPERFRAME must NOT appear here - it aliases
	 * V4L2_CAP_VIDEO_CAPTURE_MPLANE (both are 0x1000) and would make
	 * tools probe a Multiplanar queue we do not provide. */
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			   V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int avm_enum_fmt(struct file *file, void *fh,
			struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

/* Current detected geometry (falls back to the ADV7604 measurement before
 * the first frame locks, then 1920x1080).  Always reports the *frame*
 * geometry: the receiver may report only the field height for interlaced
 * input, so double a short (< 700) measured height.
 *
 * Shared with the overlay pump (avmcv710-status.c) so a synthesized
 * no-signal frame is composited at exactly the geometry consumers were told
 * to expect. */
void avm_live_geometry(struct avm_device *dev, u32 *w, u32 *h)
{
	u32 ww = READ_ONCE(dev->parser.active_w);
	u32 hh = READ_ONCE(dev->parser.active_h);
	bool interlaced = false;
	unsigned long flags;

	if (!ww || !hh) {
		spin_lock_irqsave(&dev->sig_lock, flags);
		if (dev->sig.valid && dev->sig.tmds_locked &&
		    dev->sig.w >= 320 && dev->sig.w <= 4095 &&
		    dev->sig.h >= 120 && dev->sig.h <= 2048) {
			ww = dev->sig.w;
			hh = dev->sig.h;
			interlaced = dev->sig.interlaced;
		}
		spin_unlock_irqrestore(&dev->sig_lock, flags);
	}
	if (!ww || !hh) {
		ww = 1920;
		hh = 1080;
	} else if (interlaced && hh <= 700) {
		hh *= 2;
	}
	*w = ww;
	*h = hh;
}

static void avm_pixfmt(struct avm_device *dev, struct v4l2_pix_format *pix)
{
	u32 w, h;
	u8 cs = 0xFF;
	unsigned long flags;

	avm_live_geometry(dev, &w, &h);

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (dev->sig.valid)
		cs = dev->sig.colorspace;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	pix->width = w;
	pix->height = h;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = w * 2;
	pix->sizeimage = w * h * 2;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
	/*
	 * Declare the colorimetry the FPGA passes through.  HDMI YCbCr is
	 * limited range; BT.601 for SD, BT.709 for HD.
	 * If ADV7604 reported full-range (HDMI 0x53 codes 1=RGB full,
	 * 6=YCbCr601 full, 7=YCbCr709 full), declare full range so OBS
	 * does not clip whites or crush blacks.
	 */
	if (cs == 1 || cs == 6 || cs == 7)
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;

	if (h > 576) {
		pix->colorspace = V4L2_COLORSPACE_REC709;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
		pix->xfer_func = V4L2_XFER_FUNC_709;
	} else {
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
		pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	}
	pix->priv = 0;
}

static int avm_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct avm_device *dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	avm_pixfmt(dev, &f->fmt.pix);
	return 0;
}

static int avm_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct avm_device *dev = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/*
	 * Source-paced: the geometry is whatever the HDMI source locks, so a
	 * requested size cannot be honoured.  Always report the current
	 * geometry so the negotiated bytesperline/sizeimage match what the
	 * parser actually delivers - otherwise a consumer that asked for
	 * 1920x1080 but receives a 1366x768 payload would read it with the
	 * wrong stride and shear the picture.  A consumer learns about a
	 * change through V4L2_EVENT_SOURCE_CHANGE and re-queries g_fmt.
	 */
	avm_pixfmt(dev, pix);
	return 0;
}

static int avm_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct avm_device *dev = video_drvdata(file);
	int ret;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (vb2_is_busy(&dev->q))
		return -EBUSY;

	ret = avm_try_fmt(file, fh, f);
	if (ret)
		return ret;
	dev->fmt = f->fmt.pix;
	return 0;
}

/* Single HDMI input. */
static int avm_enum_input(struct file *file, void *fh,
			  struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;
	memset(inp, 0, sizeof(*inp));
	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int avm_g_input(struct file *file, void *fh, unsigned int *index)
{
	*index = 0;
	return 0;
}

static int avm_s_input(struct file *file, void *fh, unsigned int index)
{
	return index == 0 ? 0 : -EINVAL;
}

static const struct {
	u32 width;
	u32 height;
} avm_frame_sizes[] = {
	{ 1920, 1080 },
	{ 1280,  720 },
	{  720,  576 },
	{  720,  480 },
	{  640,  480 },
};

static int avm_enum_framesizes(struct file *file, void *fh,
			       struct v4l2_frmsizeenum *fsize)
{
	struct avm_device *dev = video_drvdata(file);
	u32 live_w, live_h;

	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	avm_live_geometry(dev, &live_w, &live_h);

	/* Index 0 is always the currently live/detected geometry */
	if (fsize->index == 0) {
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = live_w;
		fsize->discrete.height = live_h;
		return 0;
	}

	if (fsize->index <= ARRAY_SIZE(avm_frame_sizes)) {
		u32 idx = fsize->index - 1;

		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = avm_frame_sizes[idx].width;
		fsize->discrete.height = avm_frame_sizes[idx].height;
		return 0;
	}
	return -EINVAL;
}

/* Measured frame rate once the stream is locked, else default nominal 60. */
static void avm_current_interval(struct avm_device *dev,
				 struct v4l2_fract *tpf)
{
	u32 fps = READ_ONCE(dev->parser.measured_fps);

	if (!fps || fps > 240)
		fps = 60;	/* Default nominal rate: 60 fps (1080p60) */
	tpf->numerator = 1;
	tpf->denominator = fps;
}

/* Standard capture rates supported by hardware/OBS */
static const struct v4l2_fract avm_intervals[] = {
	{ 1, 60 },	/* 60.00 fps */
	{ 1001, 60000 },/* 59.94 fps */
	{ 1, 50 },	/* 50.00 fps */
	{ 1, 30 },	/* 30.00 fps */
	{ 1001, 30000 },/* 29.97 fps */
	{ 1, 25 },	/* 25.00 fps */
	{ 1, 24 },	/* 24.00 fps */
};

static int avm_enum_frameintervals(struct file *file, void *fh,
				   struct v4l2_frmivalenum *fiv)
{
	if (fiv->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	if (fiv->index >= ARRAY_SIZE(avm_intervals))
		return -EINVAL;

	fiv->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fiv->discrete = avm_intervals[fiv->index];
	return 0;
}

static int avm_g_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct avm_device *dev = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	p->parm.capture.readbuffers = 4;
	avm_current_interval(dev, &p->parm.capture.timeperframe);
	return 0;
}

static int avm_s_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct avm_device *dev = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	/*
	 * The source paces the cadence, but the CVX8897 can halve it in
	 * hardware.  Record the requested rate so the converter worker can
	 * pick the matching half-rate table (or stay native).
	 */
	if (p->parm.capture.timeperframe.numerator)
		avm_cvx8897_request_fps(dev,
			p->parm.capture.timeperframe.denominator /
			p->parm.capture.timeperframe.numerator);
	return avm_g_parm(file, fh, p);
}

/*
 * V4L2_EVENT_SOURCE_CHANGE is queued from the parser worker whenever the
 * locked geometry changes (avm_source_change), so a consumer re-queries
 * g_fmt/S_FMT instead of reading the new raster with the old stride - the
 * resolution-switch freeze/hang the official-driver model avoids.
 */
void avm_source_change(struct avm_device *dev)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue(&dev->vdev, &ev);
	/* A new locked input may need a different converter table. */
	avm_cvx8897_kick(dev);
}

static int avm_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_SOURCE_CHANGE)
		return -EINVAL;
	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static int avm_unsubscribe_event(struct v4l2_fh *fh,
				 const struct v4l2_event_subscription *sub)
{
	return v4l2_event_unsubscribe(fh, sub);
}

static const struct v4l2_file_operations avm_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	/*
	 * vb2_fop_release, NOT v4l2_fh_release: the latter only drops the
	 * event-queue handle, leaving the vb2 queue allocated (q->num_buffers
	 * != 0) and any active stream uncancelled.  The next REQBUFS/S_FMT on
	 * the re-opened node then failed with -EBUSY, so OBS froze on every
	 * resolution/fps change until the device was replugged; a crashed or
	 * killed consumer also kept the FPGA DMA running forever (system-wide
	 * load).  vb2_fop_release frees the buffers and cancels streaming.
	 */
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

static const struct v4l2_ioctl_ops avm_ioctl_ops = {
	.vidioc_querycap	= avm_querycap,
	.vidioc_enum_fmt_vid_cap = avm_enum_fmt,
	.vidioc_g_fmt_vid_cap	= avm_g_fmt,
	.vidioc_try_fmt_vid_cap = avm_try_fmt,
	.vidioc_s_fmt_vid_cap	= avm_s_fmt,
	.vidioc_enum_input	= avm_enum_input,
	.vidioc_g_input		= avm_g_input,
	.vidioc_s_input		= avm_s_input,
	.vidioc_enum_framesizes	= avm_enum_framesizes,
	.vidioc_enum_frameintervals = avm_enum_frameintervals,
	.vidioc_g_parm		= avm_g_parm,
	.vidioc_s_parm		= avm_s_parm,
	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_create_bufs	= vb2_ioctl_create_bufs,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
	.vidioc_subscribe_event	= avm_subscribe_event,
	.vidioc_unsubscribe_event = avm_unsubscribe_event,
};

/* ------------------------------------------------------------------ */
/* vb2 queue ops                                                       */

static int avm_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			   unsigned int *num_planes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	if (!*num_buffers)
		*num_buffers = 4;
	if (*num_buffers < 2)
		*num_buffers = 2;
	if (*num_buffers > 32)
		*num_buffers = 32;

	*num_planes = 1;
	sizes[0] = AVM_FRAME_BPF;	/* always max-sized, never overflows */
	return 0;
}

static void avm_buf_queue(struct vb2_buffer *vb)
{
	struct avm_device *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct avm_buffer *buf = to_avm_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->urbs_lock, flags);
	list_add_tail(&buf->list, &dev->vb_bufs);
	dev->pending_bufs++;
	spin_unlock_irqrestore(&dev->urbs_lock, flags);
}

static int avm_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct avm_device *dev = vb2_get_drv_priv(q);
	int ret, i = 0;

	/* vb2 core holds q->lock (== stream_lock) while calling this. */
	if (dev->streaming)
		return 0;
	avm_parser_reset(dev);

	/*
	 * Publish streaming BEFORE submitting URBs: a URB can complete
	 * before the last submit returns, and the worker must already see
	 * streaming=true or it will skip resubmitting that URB.
	 */
	WRITE_ONCE(dev->streaming, true);

	/* Watchdog baseline: measure a new stream from its start. */
	dev->watch_last_chunks = atomic64_read(&dev->urb_chunks);
	dev->watch_silent = false;
	dev_info(&dev->udev->dev,
		 "STREAMON: %ux%u YUYV, cvx=%s req_fps=%u\n",
		 dev->fmt.width, dev->fmt.height,
		 avm_cvx8897_enabled() ? "on" : "off",
		 READ_ONCE(dev->cvx_req_fps));

	/* Drop any error state left from a previous stream. */
	for (i = 0; i < AVM_NUM_URBS; i++) {
		dev->urbs[i].err = false;
		dev->urbs[i].errors = 0;
	}

	/*
	 * Submit the EP 0x83 video URBs *first*, then clean the FX3 DMA
	 * pointer and enable FPGA work mode - the exact order the userspace
	 * start path uses (UsbStream::queueFrameRead: queueAllFrameReads ->
	 * sendResetStreamDma -> setFpgaWork).  Enabling work mode with no
	 * video reader queued leaves the FX3 DMA with nowhere to drain.
	 */
	ret = 0;
	for (i = 0; i < AVM_NUM_URBS; i++) {
		struct urb *urb = dev->urbs[i].urb;

		usb_anchor_urb(urb, &dev->urb_anchor);
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(urb);
			break;
		}
	}
	if (ret)
		goto err;

	/*
	 * Clean the FX3 DMA pointer, then enable FPGA work mode.
	 *
	 * These are EP1 *control* commands, independent of the EP 0x83 video
	 * pipe.  The userspace driver issues the same pair and ignores a
	 * failure - a bulk timeout here is almost always a synchronisation
	 * artifact (the FX3 finishes the command but the completion is lost),
	 * not a dead endpoint.  Retry once, then warn and stream anyway:
	 * aborting the whole STREAMON on a transient control timeout only
	 * produced a black preview plus a vb2 buffer-state warning.
	 */
	ret = avm_reset_stream_dma(dev);
	if (!ret)
		ret = avm_fpga_work_mode(dev, true);
	if (ret) {
		msleep(50);
		ret = avm_reset_stream_dma(dev);
		if (!ret)
			ret = avm_fpga_work_mode(dev, true);
	}
	if (ret)
		dev_warn(&dev->udev->dev,
			 "FPGA/DMA start command failed: %d (streaming anyway)\n",
			 ret);

	/* Now that the FPGA is producing, arm the receiver poll + overlay
	 * pump (avmcv710-status.c).  Arming here, not in probe, is what keeps
	 * the ADV7604 I2C off the idle link (polling an idle FX3 wedged EP1). */
	avm_status_start(dev);

	dev_dbg(&dev->udev->dev, "streaming: %u URBs submitted\n",
		AVM_NUM_URBS);
	return 0;

err:
	dev_err(&dev->udev->dev, "start_streaming failed: %d (URB %d)\n",
		ret, i);
	/* Symmetric with stop_streaming: no URBs may outlive this fail. */
	WRITE_ONCE(dev->streaming, false);
	usb_kill_anchored_urbs(&dev->urb_anchor);
	flush_workqueue(dev->wq);
	/* vb2 re-queues on a failed STREAMON, so hand buffers back QUEUED
	 * (not ERROR) - see avm_return_buffers() and videobuf2-core.h. */
	avm_return_buffers(dev, VB2_BUF_STATE_QUEUED);
	avm_fpga_work_mode(dev, false);
	return ret;
}

static void avm_stop_streaming(struct vb2_queue *q)
{
	struct avm_device *dev = vb2_get_drv_priv(q);
	unsigned long flags;

	/* vb2 core holds q->lock (== stream_lock) while calling this. */
	WRITE_ONCE(dev->streaming, false);

	/* Cancel the receiver poll / overlay pump first: the worker stops
	 * re-arming as soon as streaming clears, and this waits out any
	 * in-flight I2C read before we park the FPGA below. */
	avm_status_stop(dev);

	/*
	 * Park the FPGA (stop it producing frames) and reset the FX3 DMA
	 * *before* cancelling the EP 0x83 video URBs, mirroring the userspace
	 * shutdown (UsbStream::shutdownStream performs setFpgaIdle +
	 * sendResetStreamDma while the frame transfers are still active, and
	 * only then cancels them).
	 *
	 * Killing the video URBs first leaves the FX3 mid-DMA with its bulk IN
	 * endpoint torn down; the firmware then stops servicing the EP1 control
	 * pipe, the park write times out (-110) and every later command hangs
	 * until the device is replugged.
	 */
	avm_fpga_work_mode(dev, false);
	avm_reset_stream_dma(dev);

	/* Kill in-flight URBs, then wait for parser work to finish. */
	usb_kill_anchored_urbs(&dev->urb_anchor);
	flush_workqueue(dev->wq);

	/*
	 * A worker that passed the streaming check just before the kill can
	 * have re-anchored + resubmitted an URB while usb_kill_anchored_urbs
	 * was draining the anchor, so it escaped the kill.  flush_workqueue
	 * guarantees no further resubmits are possible now, so kill once
	 * more to disarm any straggler - otherwise the next STREAMON would
	 * fail with "URB already submitted" and the stream never starts
	 * again (OBS shows a frozen device after a format/resolution switch).
	 */
	usb_kill_anchored_urbs(&dev->urb_anchor);

	/* Drop any chunks the worker queued after the last splice. */
	spin_lock_irqsave(&dev->urbs_lock, flags);
	INIT_LIST_HEAD(&dev->drain_list);
	spin_unlock_irqrestore(&dev->urbs_lock, flags);

	avm_return_buffers(dev, VB2_BUF_STATE_ERROR);

	/* Parser stays reset until the next STREAMON. */
	avm_fpga_work_mode(dev, false);

	dev_dbg(&dev->udev->dev,
		"stop: valid %d dropped %d corrupt %d resync %d\n",
		atomic_read(&dev->parser.valid_frames),
		atomic_read(&dev->parser.dropped_frames),
		atomic_read(&dev->parser.corrupt_frames),
		atomic_read(&dev->parser.resync_count));
}

static const struct vb2_ops avm_vb2_ops = {
	.queue_setup		= avm_queue_setup,
	.buf_queue		= avm_buf_queue,
	.start_streaming	= avm_start_streaming,
	.stop_streaming		= avm_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* v4l2 device registration                                            */

static void avm_vdev_release(struct video_device *vdev)
{
	struct avm_device *dev = video_get_drvdata(vdev);

	/* Last file handle closed: safe to tear down the whole pipeline. */
	avm_usb_release(dev);
	v4l2_device_unregister(&dev->v4l2_dev);
	kvfree(dev->parser.frame);
	kvfree(dev->parser.weave);
	usb_put_dev(dev->udev);
	kfree(dev);
}

int avm_video_register(struct avm_device *dev)
{
	struct v4l2_device *v4l2_dev = &dev->v4l2_dev;
	struct video_device *vdev = &dev->vdev;
	int ret;

	dev->fmt.width = 1920;
	dev->fmt.height = 1080;
	dev->fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	dev->fmt.field = V4L2_FIELD_NONE;
	dev->fmt.bytesperline = 3840;
	dev->fmt.sizeimage = 1920 * 1080 * 2;
	dev->fmt.colorspace = V4L2_COLORSPACE_REC709;
	dev->fmt.ycbcr_enc = V4L2_YCBCR_ENC_709;
	dev->fmt.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	dev->fmt.xfer_func = V4L2_XFER_FUNC_709;

	ret = v4l2_device_register(&dev->udev->dev, v4l2_dev);
	if (ret) {
		dev_err(&dev->udev->dev, "v4l2_device_register failed: %d\n",
			ret);
		return ret;
	}

	dev->q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->q.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	dev->q.mem_ops = &vb2_vmalloc_memops;
	dev->q.ops = &avm_vb2_ops;
	dev->q.buf_struct_size = sizeof(struct avm_buffer);
	dev->q.drv_priv = dev;
	dev->q.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	dev->q.lock = &dev->stream_lock;
	ret = vb2_queue_init(&dev->q);
	if (ret) {
		dev_err(&dev->udev->dev, "vb2_queue_init failed: %d\n", ret);
		v4l2_device_unregister(v4l2_dev);
		return ret;
	}

	vdev->release = avm_vdev_release;
	vdev->fops = &avm_fops;
	vdev->ioctl_ops = &avm_ioctl_ops;
	vdev->v4l2_dev = v4l2_dev;
	vdev->queue = &dev->q;
	vdev->lock = &dev->stream_lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	strscpy(vdev->name, "AVerMedia ExtremeCap U3 (CV710)",
		sizeof(vdev->name));
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&dev->udev->dev, "video_register_device failed: %d\n",
			ret);
		vb2_queue_release(&dev->q);
		v4l2_device_unregister(v4l2_dev);
		return ret;
	}
	dev_info(&dev->udev->dev, "registered %s\n",
		 video_device_node_name(vdev));
	return 0;
}