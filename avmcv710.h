/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * avmcv710 - kernel capture driver for the AVerMedia ExtremeCap U3 (CV710)
 *
 * Cypress FX3 (CYUSB3014) + Analog Devices ADV7604 + Lattice ECP3 FPGA.
 * See ../.. PROTOCOL.md (cv710userspace) for the wire framing: this is the
 * same C0/C1 audio-interleaved bulk protocol, ported to a URB + vb2
 * pipeline.  No TrueStream involved.
 *
 * Kernel-safety contract (why this driver does not panic):
 *   - every parser index is bounds-checked against the transfer word count
 *     before it is used; arithmetic uses guarded >= / min() clamps so a
 *     "words - cur" can never underflow (that family of bugs was the
 *     source of the userspace memcpy fault that motivated the CV-23 rules)
 *   - all URBs are anchored and killed before their buffers are freed
 *   - parsing happens in a workqueue (process context), never in the URB
 *     completion callback
 *   - vb2 buffers are only filled while streaming, and queued buffers are
 *     returned with VB2_BUF_STATE_ERROR on any stop path
 *   - every bulk transfer during bootstrap has a timeout; probe() returns
 *     -ETIMEDOUT / -EIO instead of hanging the USB stack
 */
#ifndef AVM_CV710_H
#define AVM_CV710_H

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

/* USB identity */
#define AVM_USB_VID		0x07ca
#define AVM_USB_PID		0x0710
#define AVM_EP_CTRL_OUT		0x01	/* FX3 control pipe, bulk OUT */
#define AVM_EP_CTRL_IN		0x81	/* FX3 control pipe, bulk IN  */
#define AVM_EP_VIDEO_IN		0x83	/* FX3 video/audio bulk IN   */

/*
 * Transfer chunk: the FX3/FPGA DMA boundary.  Every bulk read is this big;
 * a frame spans several chunks and a chunk may hold several frames.
 */
#define AVM_CHUNK_BYTES		0x1FC000u	/* 2,080,768 */
#define AVM_CHUNK_WORDS		(AVM_CHUNK_BYTES / 4)

/* Frame limits (words; 1 word = one YUY2 macropixel = 2 pixels) */
#define AVM_MAX_FRAME_WORDS	(1920u * 1200u / 2u)	/* 1,152,000 */
#define AVM_FRAME_BPF		(AVM_MAX_FRAME_WORDS * 4)

/* Markers (uint32 LE) - none of these are legal YUY2 pairs */
#define AVM_MARKER_C0		0xC0FFFF00u	/* video frame start  */
#define AVM_MARKER_C1		0xC1FFFF00u	/* video frame end    */
#define AVM_MARKER_AUDIO	0x58FFFF00u	/* audio packet start */
#define AVM_MARKER_AUDIO_PAD	0xAA5555AAu	/* audio pad / DMA align */

/* Streaming pipeline */
#define AVM_NUM_URBS		4
/* Bootstrap control timeouts */
#define AVM_BOOT_WRITE_TIMEOUT	2000
#define AVM_BOOT_READ_TIMEOUT	2000

struct avm_device;

/* Per-USB-request bookkeeping */
struct avm_urb {
	struct avm_device *dev;	/* owning device */
	struct urb *urb;
	u8 *transfer;		/* URB DMA target (AVM_CHUNK_BYTES) */
	u8 *parse_buf;		/* headroom buffer passed to the parser */
	struct list_head done;	/* chained in dev->drain_list */
};

/* Framing parser state (port of Device::onFrameData, kernel-safe) */
struct avm_parser {
	/* transport */
	u32 hold[3];		/* words held back across chunk boundary */
	unsigned int hold_n;

	/* stream state */
	bool in_video;
	bool in_audio;
	bool in_audio_padding;
	bool clock_recovering;

	/* audio payload remaining across chunk boundary */
	u32 remaining_audio_words;

	/* frame bookkeeping */
	u32 frame_words;	/* words accumulated for the current frame */
	u8 last_seq;
	bool have_seq;
	u32 field_flags;	/* C0 metadata b2 */

	/* staged frame data (kvzalloc, AVM_FRAME_BPF each) */
	u8 *frame;		/* current frame/field staging buffer */
	u8 *weave;		/* interlaced weave destination */
	u8 parity_reliable;
	u8 fields_woven;
	u8 weave_toggle;
	s8 last_device_parity;
	u8 device_parity_changes;

	/* mode hysteresis (needs 4 consecutive matching frames) */
	u32 pending_w, pending_h, pending_count;
	u32 active_w, active_h;
	u32 target_words;	/* words expected for active mode */
	unsigned int mode_serial; /* bumped on each locked switch */

	/* stats (atomic-visible counters) */
	atomic_t valid_frames;
	atomic_t dropped_frames;
	atomic_t corrupt_frames;
	atomic_t resync_count;
};

struct avm_buffer {
	struct vb2_v4l2_buffer vb;	/* must be first */
	struct list_head list;
};

struct avm_device {
	struct usb_device *udev;
	struct usb_interface *intf;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue q;

	struct v4l2_pix_format fmt;	/* current negotiated format */

	/* streaming */
	bool streaming;			/* protected by stream_lock */
	struct mutex stream_lock;
	struct workqueue_struct *wq;
	struct work_struct parse_work;
	struct avm_urb urbs[AVM_NUM_URBS];
	spinlock_t urbs_lock;		/* protects vb_bufs/drain lists */
	struct list_head drain_list;	/* avm_urb.done chunks for parser */
	struct list_head vb_bufs;	/* avm_buffer.list waiting for a frame */
	unsigned int pending_bufs;
	struct avm_parser parser;

	/* USB plumbing */
	struct usb_anchor urb_anchor;	/* anchored streaming URBs */
	bool bootstrapped;
};

/* avmcv710-hw.c */
int avm_bootstrap(struct avm_device *dev);
int avm_ep1_write(struct avm_device *dev, const u8 *data, size_t len);
int avm_ep1_read(struct avm_device *dev, u8 *data, size_t len);
int avm_i2c_write(struct avm_device *dev, u8 slave7, u8 reg, u8 val);
int avm_i2c_read(struct avm_device *dev, u8 slave7, u8 reg, u8 *val);
int avm_fpga_work_mode(struct avm_device *dev, bool work);
int avm_reset_stream_dma(struct avm_device *dev);

/* avmcv710-video.c */
int avm_video_register(struct avm_device *dev);

/* avmcv710-usb.c */
void avm_return_buffers(struct avm_device *dev);
void avm_usb_release(struct avm_device *dev);

/* avmcv710-parser.c */
void avm_parser_reset(struct avm_device *dev);
void avm_parser_feed(struct avm_device *dev, u32 *d, u32 count);

#endif /* AVM_CV710_H */