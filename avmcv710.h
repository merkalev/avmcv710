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
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

/* ALSA capture PCM object, defined in avmcv710-audio.c */
struct avm_audio;

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

/* Streaming pipeline.  Latency = (urbs - 1) chunks of pre-buffering: each
 * chunk is 2 MB, so 4 URBs hold ~1 frame of slack at 1080p60 (~33 ms) while
 * still covering USB scheduling jitter.  6+ URBs pre-buffered ~2 frames and
 * showed up as visible latency in OBS. */
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
	bool err;		/* completion carried a nonzero status */
	unsigned int errors;	/* consecutive completion errors */
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
	bool active_interlaced;	/* locked mode is interlaced */
	u32 target_words;	/* words expected for active mode */
	unsigned int mode_serial; /* bumped on each locked switch */
	u32 frame_sequence;	/* DQBUF sequence counter (V4L2_BUF.sequence) */

	/* liveness / pacing: last time real frame data was seen, and the
	 * rolling frame rate (used to pace the no-signal/out-of-range
	 * splash and to answer g/s_parm) */
	ktime_t last_real_kt;
	ktime_t fps_t0;
	u32 fps_cnt;
	u32 measured_fps;

	/* stats (atomic-visible counters) */
	atomic_t valid_frames;
	atomic_t dropped_frames;
	atomic_t corrupt_frames;
	atomic_t resync_count;
};

/* ADV7604 status snapshot, published by the status worker (~10 Hz). */
struct avm_signal {
	bool valid;		/* at least one successful poll published */
	bool tmds_locked;	/* IO 0x6a: (v & 0xE0) == 0xE0 && (v & 0x10) */
	bool interlaced;	/* IO 0x12 STDI bit 4 */
	u32 w, h;		/* measured active geometry (12-bit, 0 = n/a) */
	u32 poll_failures;	/* consecutive failed polls */
};

/* Overlay bitmap shown when the input is unavailable / unusable. */
enum avm_splash {
	AVM_SPLASH_NONE = 0,
	AVM_SPLASH_NO_SIGNAL,
	AVM_SPLASH_OUT_OF_RANGE,
	AVM_SPLASH_HDCP,
	AVM_SPLASH_CONTENT_PROTECTION,
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

	/* serializes EP1 control traffic (I2C reads/writes are
	 * command+response pairs and must stay atomic) */
	struct mutex ctrl_lock;

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

	/* ADV7604 poll + standby overlay pump (avmcv710-status.c).  The work
	 * is armed only while streaming (start/stop bracket it), so no
	 * receiver I2C is issued while the FPGA link is idle. */
	spinlock_t sig_lock;		/* protects dev->sig */
	struct avm_signal sig;
	struct delayed_work status_work;
	u32 status_tick;		/* ticks since the work was (re)armed */
	enum avm_splash splash;		/* overlay currently shown */
	enum avm_splash splash_cached;	/* bitmap held in splash_frame */
	u8 *splash_frame;		/* AVM_FRAME_BPF composited overlay */
	u32 splash_w, splash_h, splash_words;

	/* ALSA capture PCM (avmcv710-audio.c): the parser pushes 0x58 audio
	 * payload bytes into the PCM DMA buffer.  NULL until probe registers
	 * it and after fini, so every reader lives behind READ_ONCE. */
	struct avm_audio *audio;

	/* USB plumbing */
	struct usb_anchor urb_anchor;	/* anchored streaming URBs */
	bool bootstrapped;

	/* CVX8897 frame-rate converter (avmcv710-cvx8897.c).  The converter
	 * worker runs on its own workqueue because programming it sleeps ~3 s
	 * and must not stall the parser. */
	struct workqueue_struct *cvx_wq;
	struct work_struct cvx_work;
	u32 cvx_req_fps;	/* consumer-requested rate (0 = none) */
	bool cvx_prog_half;	/* converter currently set to half rate */
	u32 cvx_prog_serial;	/* parser mode_serial when last programmed */

	/* STREAMON watchdog (avmcv710-usb.c): a streaming pipe that stops
	 * producing EP 0x83 data while URBs stay idle is the soft-reload
	 * no-output symptom (silent FPGA, no halted endpoint to log). */
	struct delayed_work watch_work;
	atomic64_t urb_chunks;	/* completed EP 0x83 chunks, any status */
	u64 watch_last_chunks;	/* chunks at the previous 5 s sample */
	bool watch_silent;	/* last sample found the pipe idle */
	u64 watch_silent_at;	/* seconds when the silence was spotted */
};

/* avmcv710-hw.c */
int avm_bootstrap(struct avm_device *dev);
int avm_ep1_write(struct avm_device *dev, const u8 *data, size_t len);
int avm_ep1_read(struct avm_device *dev, u8 *data, size_t len);
void avm_ep1_drain(struct avm_device *dev);
int avm_i2c_write(struct avm_device *dev, u8 slave7, u8 reg, u8 val);
int avm_i2c_read_block(struct avm_device *dev, u8 slave7, u8 reg,
		       u8 *data, u8 len);
int avm_fpga_work_mode(struct avm_device *dev, bool work);
int avm_reset_stream_dma(struct avm_device *dev);

/* avmcv710-cvx8897.c */
const u8 *avm_cvx8897_half_rate(u32 w, u32 h, u32 fps, bool rgb,
				bool interlaced);
int avm_cvx8897_apply(struct avm_device *dev, const u8 *values);
int avm_cvx8897_set_mode(struct avm_device *dev, bool converted);
int avm_cvx8897_init(struct avm_device *dev);
void avm_cvx8897_fini(struct avm_device *dev);
void avm_cvx8897_kick(struct avm_device *dev);
void avm_cvx8897_request_fps(struct avm_device *dev, u32 fps);
bool avm_cvx8897_enabled(void);

/* avmcv710-video.c */
int avm_video_register(struct avm_device *dev);
void avm_source_change(struct avm_device *dev);
void avm_live_geometry(struct avm_device *dev, u32 *w, u32 *h);

/* avmcv710-status.c */
int avm_status_init(struct avm_device *dev);
void avm_status_fini(struct avm_device *dev);
void avm_status_start(struct avm_device *dev);
void avm_status_stop(struct avm_device *dev);
const u8 *avm_splash_bmp(enum avm_splash kind, u32 *words);

/* avmcv710-usb.c */
void avm_return_buffers(struct avm_device *dev, enum vb2_buffer_state state);
void avm_usb_release(struct avm_device *dev);

/* avmcv710-parser.c */
void avm_parser_reset(struct avm_device *dev);
void avm_parser_feed(struct avm_device *dev, u32 *d, u32 count);
void avm_deliver_frame(struct avm_device *dev, const u8 *src, u32 words);

/* avmcv710-audio.c */
int avm_audio_init(struct avm_device *dev);
void avm_audio_fini(struct avm_device *dev);
void avm_audio_push(struct avm_device *dev, const u8 *src, u32 bytes);

#endif /* AVM_CV710_H */