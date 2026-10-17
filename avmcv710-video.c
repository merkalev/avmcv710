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
 * "Video Input Frame Rate" selector).  enum_frameintervals() therefore offers
 * only {native, native/2} - and the half only when that converter is enabled
 * (avmcv710-cvx8897.c), which S_PARM then programs; otherwise the source paces
 * the cadence and only the measured rate is honest.
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
 * The receiver poll (avmcv710-status.c) publishes the same geometry so the
 * parser fallback and the format negotiation agree. */
void avm_live_geometry(struct avm_device *dev, u32 *w, u32 *h)
{
	u32 ww = 0, hh = 0;
	bool interlaced = false;
	unsigned long flags;

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (dev->sig.valid && dev->sig.tmds_locked &&
	    dev->sig.w >= 320 && dev->sig.w <= 4095 &&
	    (dev->sig.w & 1) == 0 &&
	    dev->sig.h >= 120 && dev->sig.h <= 2048) {
		ww = dev->sig.w;
		hh = dev->sig.h;
		interlaced = dev->sig.interlaced;
	}
	spin_unlock_irqrestore(&dev->sig_lock, flags);
	if (interlaced && hh <= 700)
		hh *= 2;
	if ((u64)ww * hh / 2 > AVM_MAX_FRAME_WORDS) {
		ww = 0;
		hh = 0;
	}

	if (!ww || !hh) {
		ww = READ_ONCE(dev->parser.active_w);
		hh = READ_ONCE(dev->parser.active_h);
	}
	if (!ww || !hh) {
		ww = 1920;
		hh = 1080;
	}
	*w = ww;
	*h = hh;
}

/*
 * True when the consumer negotiated a capture geometry the source cannot
 * provide.  The FPGA passes the raster through with no scaler, so the
 * official driver rejects a non-native S_FMT by showing the "signal out of
 * range" overlay; this driver does the same (avmcv710-status.c delivers the
 * overlay, the parser drops the live frames while the mismatch holds).
 * Called from the parser hot path, so it is a plain READ_ONCE comparison.
 */
bool avm_req_mismatch(struct avm_device *dev, u32 w, u32 h)
{
	u32 rw = READ_ONCE(dev->req_w);
	u32 rh = READ_ONCE(dev->req_h);

	return rw && rh && (rw != w || rh != h);
}

static void avm_geom_pixfmt(struct v4l2_pix_format *pix, u32 w, u32 h, u8 cs)
{
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

	avm_geom_pixfmt(pix, w, h, cs);
}

/*
 * Publish a new source geometry into dev->fmt.  Only valid when the
 * consumer followed the live geometry (follow_source, "leave unchanged"):
 * the mode-acquisition path in the parser calls this, then queues
 * V4L2_EVENT_SOURCE_CHANGE, and the consumer renegotiates buffers against
 * the new geometry.  The queued buffers still carry the old geometry, so
 * the parser delivery gate (qfmt snapshot) keeps mismatched frames out
 * until the renegotiation completes.
 */
void avm_publish_geometry(struct avm_device *dev, u32 w, u32 h)
{
	struct v4l2_pix_format pix;
	u8 cs = 0xFF;
	unsigned long flags;

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (dev->sig.valid)
		cs = dev->sig.colorspace;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	avm_geom_pixfmt(&pix, w, h, cs);
	dev->fmt = pix;
}

static int avm_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct avm_device *dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_is_busy(&dev->q)) {
		f->fmt.pix = dev->fmt;
		return 0;
	}

	avm_pixfmt(dev, &f->fmt.pix);
	dev->fmt = f->fmt.pix;
	return 0;
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

/* Standard geometries the DirectShow pin table exposes (resolution menu). */

static int avm_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct avm_device *dev = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	u32 live_w, live_h;
	u32 req_w = pix->width, req_h = pix->height;
	u32 best_w, best_h;
	u8 cs = 0xFF;
	unsigned long flags;
	int i;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	avm_live_geometry(dev, &live_w, &live_h);

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (dev->sig.valid)
		cs = dev->sig.colorspace;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	best_w = live_w;
	best_h = live_h;
	if (req_w == live_w && req_h == live_h) {
		best_w = live_w;
		best_h = live_h;
	} else {
		for (i = 0; i < ARRAY_SIZE(avm_frame_sizes); i++) {
			if (req_w == avm_frame_sizes[i].width &&
			    req_h == avm_frame_sizes[i].height) {
				best_w = req_w;
				best_h = req_h;
				break;
			}
		}
	}

	pix->width = best_w;
	pix->height = best_h;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = best_w * 2;
	pix->sizeimage = best_w * best_h * 2;

	if (best_h > 576) {
		pix->colorspace = V4L2_COLORSPACE_REC709;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
		pix->xfer_func = V4L2_XFER_FUNC_709;
	} else {
		pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
		pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	}
	if (cs == 1 || cs == 6 || cs == 7)
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->priv = 0;
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

	/*
	 * Record the consumer's negotiated geometry so the parser and standby
	 * overlay pump strictly match the allocated buffer size. Any incoming
	 * raster differing from the negotiated buffer format is gated out
	 * (avm_req_mismatch()) and presents "Signal Out of Range" at the
	 * negotiated buffer size, preventing stride corruption and tearing.
	 */
	WRITE_ONCE(dev->req_w, f->fmt.pix.width);
	WRITE_ONCE(dev->req_h, f->fmt.pix.height);

	/*
	 * Decide whether the consumer pinned a geometry or accepted the live
	 * one.  When it accepted the live geometry (OBS "leave unchanged"),
	 * a mid-stream source resolution change is followed: the parser
	 * publishes the new mode into dev->fmt and fires SOURCE_CHANGE so
	 * the consumer renegotiates buffers at the new raster.  When it
	 * pinned a different geometry, the vendor behavior holds instead:
	 * the out-of-range overlay stays up until the source matches.
	 */
	{
		u32 live_w, live_h;

		avm_live_geometry(dev, &live_w, &live_h);
		WRITE_ONCE(dev->follow_source,
			   f->fmt.pix.width == live_w &&
			   f->fmt.pix.height == live_h);
	}
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

static int avm_enum_framesizes(struct file *file, void *fh,
			       struct v4l2_frmsizeenum *fsize)
{
	struct avm_device *dev = video_drvdata(file);
	u32 live_w, live_h, seen = 0, i;

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

	/* Skip any standard table entry that duplicates live_w x live_h */
	for (i = 0; i < ARRAY_SIZE(avm_frame_sizes); i++) {
		if (avm_frame_sizes[i].width == live_w &&
		    avm_frame_sizes[i].height == live_h)
			continue;
		seen++;
		if (seen == fsize->index) {
			fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			fsize->discrete.width = avm_frame_sizes[i].width;
			fsize->discrete.height = avm_frame_sizes[i].height;
			return 0;
		}
	}
	return -EINVAL;
}

/*
 * Native source cadence.  measured_fps is the *delivered* rate, so while the
 * CVX8897 is halving, the input rate is twice that.  Before the parser locks
 * (or before a first measurement) assume the 1080p60 nominal used elsewhere.
 */
static u32 avm_native_fps(struct avm_device *dev)
{
	u32 fps = READ_ONCE(dev->parser.measured_fps);

	if (!fps || fps > 240)
		return 60;	/* default nominal rate: 60 fps (1080p60) */
	if (READ_ONCE(dev->cvx_prog_half))
		fps *= 2;	/* measured is the half rate; recover the input */
	return fps;
}

/* Measured delivered frame rate once the stream is locked, else nominal 60. */
static void avm_current_interval(struct avm_device *dev,
				 struct v4l2_fract *tpf)
{
	u32 fps = READ_ONCE(dev->parser.measured_fps);

	if (!fps || fps > 240)
		fps = 60;	/* default nominal rate: 60 fps (1080p60) */
	tpf->numerator = 1;
	tpf->denominator = fps;
}

/*
 * Frame-interval menu: only what the hardware can deliver for the live
 * source geometry.  Entry 0 is the native input rate (measured, or the
 * 60 fps nominal before the first lock); entry 1 is that rate halved and
 * is offered only when the CVX8897 has a half-rate table for this mode.
 * The converter covers nothing else (its tables convert 60->30 and
 * 50->25, progressive only), and asking for anything between the two
 * rates (50 on a 60 Hz input, 24 on anything) produced mislabeled
 * cadences or the vendor's 24 fps black screen, so the impossible rates
 * are simply not offered.  A true 24p or 50p input still captures
 * natively: entry 0 follows the source.
 */
static int avm_enum_frameintervals(struct file *file, void *fh,
				   struct v4l2_frmivalenum *fiv)
{
	struct avm_device *dev = video_drvdata(file);
	u32 live_w, live_h, native, half;
	const u8 *table;
	bool interlaced = false, rgb = false;
	unsigned long flags;

	if (fiv->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	avm_live_geometry(dev, &live_w, &live_h);
	native = avm_native_fps(dev);
	half = native / 2;

	spin_lock_irqsave(&dev->sig_lock, flags);
	if (dev->sig.valid) {
		interlaced = dev->sig.interlaced;
		rgb = dev->sig.rgb;
	}
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	if (fiv->index == 0) {
		fiv->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fiv->discrete.numerator = 1;
		fiv->discrete.denominator = native;
		return 0;
	}

	table = avm_cvx8897_enabled() ?
		avm_cvx8897_half_rate(live_w, live_h, native, rgb,
				      interlaced) : NULL;
	if (fiv->index == 1 && table && half) {
		fiv->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fiv->discrete.numerator = 1;
		fiv->discrete.denominator = half;
		return 0;
	}

	return -EINVAL;
}

static int avm_g_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct avm_device *dev = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	p->parm.capture.readbuffers = 4;
	if (dev->req_tpf.numerator && dev->req_tpf.denominator)
		p->parm.capture.timeperframe = dev->req_tpf;
	else
		avm_current_interval(dev, &p->parm.capture.timeperframe);
	return 0;
}

/*
 * Pass the frame interval through unclamped.  The converter decides what
 * actually changes: only a request close to half the measured input rate
 * engages the CVX8897; the input rate itself, or nothing (g_parm then
 * reports the measured delivered rate), is native passthrough.  The old
 * behavior snapped the request onto a canned menu and forced 60 fps when
 * the consumer left the rate unset, which mislabeled the cadence and
 * forced rates the converter cannot produce.
 */
static int avm_s_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct avm_device *dev = video_drvdata(file);
	struct v4l2_fract *tpf = &p->parm.capture.timeperframe;
	u32 req_fps = 0;

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (tpf->numerator && tpf->denominator) {
		req_fps = tpf->denominator / tpf->numerator;
		dev->req_tpf = *tpf;
	} else {
		dev->req_tpf.numerator = 0;
		dev->req_tpf.denominator = 0;
	}

	WRITE_ONCE(dev->cvx_req_fps, req_fps);
	avm_cvx8897_request_fps(dev, req_fps);
	return avm_g_parm(file, fh, p);
}

static int avm_log_status(struct file *file, void *fh)
{
	struct avm_device *dev = video_drvdata(file);
	struct avm_parser *p = &dev->parser;
	struct avm_signal sig;
	unsigned long flags;
	u32 live_w, live_h;

	spin_lock_irqsave(&dev->sig_lock, flags);
	sig = dev->sig;
	spin_unlock_irqrestore(&dev->sig_lock, flags);

	avm_live_geometry(dev, &live_w, &live_h);

	dev_info(&dev->udev->dev, "=== avmcv710 status ===\n");
	dev_info(&dev->udev->dev,
		 "  Stream: %s, live=%ux%u%s, req=%ux%u qfmt=%ux%u follow=%u, fps=%u (native=%u)\n",
		 READ_ONCE(dev->streaming) ? "ACTIVE" : "IDLE",
		 live_w, live_h,
		 READ_ONCE(p->active_interlaced) ? "i" : "p",
		 READ_ONCE(dev->req_w), READ_ONCE(dev->req_h),
		 READ_ONCE(dev->qfmt_w), READ_ONCE(dev->qfmt_h),
		 READ_ONCE(dev->follow_source),
		 READ_ONCE(p->measured_fps), avm_native_fps(dev));
	dev_info(&dev->udev->dev,
		 "  ADV7604: valid=%u tmds=%s %ux%u%s cs=0x%02x (%s) aud_lock=%u rate=%u Hz poll_err=%u\n",
		 sig.valid, sig.tmds_locked ? "LOCKED" : "UNLOCKED",
		 sig.w, sig.h, sig.interlaced ? "i" : "p",
		 sig.colorspace, sig.rgb ? "RGB" : "YCbCr",
		 sig.audio_locked, sig.audio_rate, sig.poll_failures);
	dev_info(&dev->udev->dev,
		 "  CVX8897: enabled=%s half_active=%u req_fps=%u\n",
		 avm_cvx8897_enabled() ? "yes" : "no",
		 READ_ONCE(dev->cvx_prog_half),
		 READ_ONCE(dev->cvx_req_fps));
	dev_info(&dev->udev->dev,
		 "  Parser: valid=%d dropped=%d corrupt=%d padded=%d resync=%d chunks=%lld\n",
		 atomic_read(&p->valid_frames),
		 atomic_read(&p->dropped_frames),
		 atomic_read(&p->corrupt_frames),
		 atomic_read(&p->padded_frames),
		 atomic_read(&p->resync_count),
		 (long long)atomic64_read(&dev->urb_chunks));
	avm_audio_log_status(dev);
	return 0;
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
	.vidioc_log_status	= avm_log_status,
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

int avm_pipeline_start(struct avm_device *dev, bool is_video)
{
	int ret, i = 0;

	/* dev->stream_lock is held by the caller */
	if (is_video)
		dev->video_streaming = true;
	else
		dev->audio_streaming = true;

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
		 "STREAMON: %ux%u YUYV, cvx=%s req_fps=%u (trigger=%s)\n",
		 dev->fmt.width, dev->fmt.height,
		 avm_cvx8897_enabled() ? "on" : "off",
		 READ_ONCE(dev->cvx_req_fps),
		 is_video ? "video" : "audio");

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

	/* Now that the FPGA is producing, arm the receiver telemetry poll
	 * (avmcv710-status.c).  Arming here, not in probe, is what keeps
	 * the ADV7604 I2C off the idle link (polling an idle FX3 wedged EP1). */
	avm_status_start(dev);

	dev_dbg(&dev->udev->dev, "streaming: %u URBs submitted\n",
		AVM_NUM_URBS);
	return 0;

err:
	dev_err(&dev->udev->dev, "pipeline start failed: %d (URB %d)\n",
		ret, i);
	/* Symmetric with stop_streaming: no URBs may outlive this fail. */
	WRITE_ONCE(dev->streaming, false);
	usb_kill_anchored_urbs(&dev->urb_anchor);
	flush_workqueue(dev->wq);
	if (is_video)
		avm_return_buffers(dev, VB2_BUF_STATE_QUEUED);
	avm_fpga_work_mode(dev, false);
	if (is_video)
		dev->video_streaming = false;
	else
		dev->audio_streaming = false;
	return ret;
}

void avm_pipeline_stop(struct avm_device *dev, bool is_video)
{
	unsigned long flags;

	/* dev->stream_lock is held by the caller */
	if (is_video) {
		dev->video_streaming = false;
		avm_return_buffers(dev, VB2_BUF_STATE_ERROR);
	} else {
		dev->audio_streaming = false;
	}

	/* Keep hardware pipeline streaming if another consumer is still active */
	if (dev->video_streaming || dev->audio_streaming)
		return;

	if (!dev->streaming)
		return;

	WRITE_ONCE(dev->streaming, false);

	/* Cancel the receiver telemetry poll and CVX8897 worker first: both
	 * stop as soon as streaming clears, and this waits out any in-flight
	 * EP1 I2C transaction before we park the FPGA below. */
	avm_status_stop(dev);
	cancel_work_sync(&dev->cvx_work);

	/*
	 * Park the FPGA (stop it producing frames) and reset the FX3 DMA
	 * *before* cancelling the EP 0x83 video URBs, mirroring the userspace
	 * shutdown (UsbStream::shutdownStream performs setFpgaIdle +
	 * sendResetStreamDma while the frame transfers are still active, and
	 * only then cancels them).
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

	/* Parser stays reset until the next STREAMON. */
	avm_fpga_work_mode(dev, false);

	dev_dbg(&dev->udev->dev,
		"stop: valid %d dropped %d corrupt %d padded %d resync %d\n",
		atomic_read(&dev->parser.valid_frames),
		atomic_read(&dev->parser.dropped_frames),
		atomic_read(&dev->parser.corrupt_frames),
		atomic_read(&dev->parser.padded_frames),
		atomic_read(&dev->parser.resync_count));
}

static int avm_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct avm_device *dev = vb2_get_drv_priv(q);

	/*
	 * Snapshot the geometry the queued buffers were allocated against.
	 * The parser delivery gate compares against this snapshot, so
	 * mismatched rasters can never reach the consumer even while
	 * follow_source already moved dev->fmt to a new source mode.
	 */
	WRITE_ONCE(dev->qfmt_w, READ_ONCE(dev->fmt.width));
	WRITE_ONCE(dev->qfmt_h, READ_ONCE(dev->fmt.height));

	return avm_pipeline_start(dev, true);
}

static void avm_stop_streaming(struct vb2_queue *q)
{
	struct avm_device *dev = vb2_get_drv_priv(q);

	avm_pipeline_stop(dev, true);
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
	dev->req_w = dev->fmt.width;
	dev->req_h = dev->fmt.height;
	dev->qfmt_w = dev->fmt.width;
	dev->qfmt_h = dev->fmt.height;
	dev->follow_source = false;

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