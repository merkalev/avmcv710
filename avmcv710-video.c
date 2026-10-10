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
#include <linux/slab.h>
#include <linux/usb.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "avmcv710.h"

static inline struct avm_buffer *to_avm_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct avm_buffer, vb);
}

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

/* Current detected geometry (falls back to 1920x1080 before the first
 * frame locks, and after stop_streaming resets the parser). */
static void avm_pixfmt(struct avm_device *dev, struct v4l2_pix_format *pix)
{
	u32 w = READ_ONCE(dev->parser.active_w);
	u32 h = READ_ONCE(dev->parser.active_h);

	if (!w || !h) {
		w = 1920;
		h = 1080;
	}

	pix->width = w;
	pix->height = h;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = w * 2;
	pix->sizeimage = w * h * 2;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
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

	if (pix->pixelformat != V4L2_PIX_FMT_YUYV) {
		/* auto-detect: report the current locked geometry */
		avm_pixfmt(dev, pix);
		return 0;
	}

	/* Accept caller sizes within the palette; planes are always max. */
	if (pix->width == 0 || pix->height == 0) {
		avm_pixfmt(dev, pix);
		return 0;
	}
	if (pix->width > 1920)
		pix->width = 1920;
	if (pix->height > 1080)
		pix->height = 1080;
	pix->width &= ~1u;
	pix->height &= ~1u;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->width * pix->height * 2;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
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
	return 0;
}

static const struct v4l2_file_operations avm_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= v4l2_fh_release,
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
	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
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
	int ret, i;

	/* vb2 core holds q->lock (== stream_lock) while calling this. */
	if (dev->streaming)
		return 0;
	avm_parser_reset(dev);

	/* FPGA work mode then clean FX3 DMA pointer. */
	ret = avm_fpga_work_mode(dev, true);
	if (!ret)
		ret = avm_reset_stream_dma(dev);
	if (ret)
		goto err;

	/*
	 * Publish streaming BEFORE submitting URBs: a URB can complete
	 * before the last submit returns, and the worker must already see
	 * streaming=true or it will skip resubmitting that URB.
	 */
	WRITE_ONCE(dev->streaming, true);

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

	dev_dbg(&dev->udev->dev, "streaming: %u URBs submitted\n",
		AVM_NUM_URBS);
	return 0;

err:
	/* Symmetric with stop_streaming: no URBs may outlive this fail. */
	WRITE_ONCE(dev->streaming, false);
	usb_kill_anchored_urbs(&dev->urb_anchor);
	flush_workqueue(dev->wq);
	avm_return_buffers(dev);
	avm_fpga_work_mode(dev, false);
	return ret;
}

static void avm_stop_streaming(struct vb2_queue *q)
{
	struct avm_device *dev = vb2_get_drv_priv(q);

	/* vb2 core holds q->lock (== stream_lock) while calling this. */
	dev->streaming = false;

	/* Kill in-flight URBs, then wait for parser work to finish. */
	usb_kill_anchored_urbs(&dev->urb_anchor);
	flush_workqueue(dev->wq);

	avm_return_buffers(dev);

	/* Park the FPGA; parser stays reset until the next STREAMON. */
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
	dev->fmt.colorspace = V4L2_COLORSPACE_SMPTE170M;

	ret = v4l2_device_register(&dev->udev->dev, v4l2_dev);
	if (ret) {
		dev_err(&dev->udev->dev, "v4l2_device_register failed: %d\n",
			ret);
		return ret;
	}

	dev->q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->q.io_modes = VB2_MMAP | VB2_USERPTR;
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