// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-audio.c - ALSA capture PCM fed from the FX3 0x58 audio packets.
 *
 * The FPGA interleaves 0x58FFFF00-marked audio frames into the same EP 0x83
 * bulk stream as the C0/C1 video frames; the parser (avmcv710-parser.c)
 * demuxes them and hands the raw payload bytes here via avm_audio_push().
 *
 * Wire format (verified against the official driver and the userspace port,
 * lgxdevice.cpp + SdlAudioOutput.cpp):
 *   - marker 0x58FFFF00, then a length word:
 *       audioBytes = ((raw >> 16) & 0xFF) << 8 | ((raw >> 24) & 0xFF)
 *     valid range 512..8192 bytes, 4-byte aligned (payload is word-sized)
 *   - the payload is packed S16LE stereo at the HDMI audio clock; the
 *     userspace driver defaults the sink to 48 kHz and so do we (parsing the
 *     Audio Infoframe for 32/44.1/48 is a TODO-2 follow-up)
 *   - payload is followed by 0xAA5555AA padding until the next marker
 *
 * Sink model (snd-usb-audio style): the parser worker copies payload bytes
 * straight into the PCM DMA buffer, and pointer() reports the write cursor.
 * Both mmap and the default RW read path therefore serve the same bytes with
 * no copy_user op of our own.  snd_pcm_period_elapsed() is emitted from the
 * (process-context) parser worker every time the write cursor crosses a
 * full period, so the PCM is marked non-atomic.
 *
 * Lifecycle: avm_audio_init() runs at probe (failure is non-fatal: video
 * still works without audio).  Registration is opt-in: the `avm_audio`
 * module param (default 0) gates the whole card - without it the module is
 * video-only, which is the previously stable state.  The struct is owned by
 * the snd_card (private_free), so a still-open arecord survives disconnect;
 * fini() only flags the card as dying and calls snd_card_free_when_closed().
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "avmcv710.h"

/* the PCM device cannot register before the sound core is loaded */
MODULE_SOFTDEP("pre: snd-pcm");

#define AVM_AUDIO_RATE		48000	/* HDMI audio clock, SDL default */
#define AVM_AUDIO_CHANNELS	2	/* S16LE interleaved stereo */
#define AVM_AUDIO_RING_BYTES	(1u << 20) /* PCM buffer max: ~5.5 s @ 48k */
#define AVM_AUDIO_PERIOD_MIN	1920	/* 10 ms @ 48k/2ch/S16LE */

static bool avm_audio;		/* opt-in: card registration OFF until hardware-verified */
module_param(avm_audio, bool, 0444);
MODULE_PARM_DESC(avm_audio, "Register the AVMCV710 ALSA capture card at probe (default 0=off; pass avm_audio=1)");
static int avm_audio_index = -1;	/* snd-card index, -1 = auto */
static char *avm_audio_id = "AVMCV710";
module_param(avm_audio_index, int, 0444);
MODULE_PARM_DESC(avm_audio_index, "ALSA card index for the capture PCM (-1 = auto)");
module_param(avm_audio_id, charp, 0444);
MODULE_PARM_DESC(avm_audio_id, "ALSA card id for the capture PCM");

/*
 * Single consumer.  base is the absolute producer byte count; the write
 * position inside the PCM buffer is base % dma_bytes, so period-crossing
 * detection survives the wrap-around.
 */
struct avm_audio {
	spinlock_t lock;		/* protects everything below */
	struct avm_device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream; /* sole capture client */
	u64 base;			/* absolute bytes pushed */
	u64 produced;			/* stats: total bytes into the buffer */
	u64 missed;			/* stats: bytes dropped, no consumer */
	u64 overruns;			/* stats: bytes dropped, buffer full */
	bool running;			/* PCM triggered START */
	bool dying;			/* card teardown started */
};

static void avm_audio_card_free(struct snd_card *card)
{
	kfree(card->private_data);
}

/* ------------------------------------------------------------------ */
/* PCM ops                                                             */

static const struct snd_pcm_hardware avm_audio_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = AVM_AUDIO_RATE,
	.rate_max = AVM_AUDIO_RATE,
	.channels_min = AVM_AUDIO_CHANNELS,
	.channels_max = AVM_AUDIO_CHANNELS,
	.buffer_bytes_max = AVM_AUDIO_RING_BYTES,
	.period_bytes_min = AVM_AUDIO_PERIOD_MIN,
	.period_bytes_max = AVM_AUDIO_RING_BYTES / 2,
	.periods_min = 2,
	.periods_max = 1024,
	.fifo_size = 0,
};

static int avm_audio_pcm_open(struct snd_pcm_substream *subs)
{
	struct avm_audio *a = snd_pcm_substream_chip(subs);
	unsigned long flags;

	spin_lock_irqsave(&a->lock, flags);
	if (a->substream) {	/* one capture client at a time */
		spin_unlock_irqrestore(&a->lock, flags);
		return -EBUSY;
	}
	a->substream = subs;
	spin_unlock_irqrestore(&a->lock, flags);

	subs->runtime->hw = avm_audio_hw;
	/* payload and period sizes stay frame-aligned (4 B S16LE stereo) */
	snd_pcm_hw_constraint_step(subs->runtime, 0,
				   SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 4);
	snd_pcm_hw_constraint_step(subs->runtime, 0,
				   SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 4);
	return 0;
}

static int avm_audio_pcm_close(struct snd_pcm_substream *subs)
{
	struct avm_audio *a = snd_pcm_substream_chip(subs);
	unsigned long flags;

	spin_lock_irqsave(&a->lock, flags);
	a->substream = NULL;
	a->running = false;
	spin_unlock_irqrestore(&a->lock, flags);
	return 0;
}

static int avm_audio_pcm_hw_params(struct snd_pcm_substream *subs,
				  struct snd_pcm_hw_params *params)
{
	/* the hw constraints already pin these; belt and braces */
	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE ||
	    params_channels(params) != AVM_AUDIO_CHANNELS ||
	    params_rate(params) != AVM_AUDIO_RATE)
		return -EINVAL;
	return 0;
}

static int avm_audio_pcm_prepare(struct snd_pcm_substream *subs)
{
	/*
	 * Mandatory, not optional: snd_pcm_do_prepare() calls
	 * ops->prepare() without a NULL check (sound/core/pcm_native.c).
	 * A NULL op here is an instruction fetch at RIP 0x0 - exactly the
	 * oops WirePlumber hit the moment the card registered (it opens +
	 * PREPAREs every capture PCM to enumerate it), which then killed
	 * the whole audio graph minutes later.
	 *
	 * Everything the sink needs is pinned by the constraint set and
	 * re-checked in hw_params; the push path re-validates the runtime
	 * on every packet.  Nothing left to do but exist.
	 */
	return 0;
}

static int avm_audio_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
	struct avm_audio *a = snd_pcm_substream_chip(subs);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&a->lock, flags);
		/* start the capture window clean: drop anything that was
		 * in flight before the app said go */
		a->base = 0;
		a->running = true;
		if (subs->runtime->dma_area)
			memset(subs->runtime->dma_area, 0,
			       subs->runtime->dma_bytes);
		spin_unlock_irqrestore(&a->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&a->lock, flags);
		a->running = false;
		spin_unlock_irqrestore(&a->lock, flags);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t avm_audio_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct avm_audio *a = snd_pcm_substream_chip(subs);
	struct snd_pcm_runtime *rt = subs->runtime;
	u32 pos;

	/* frame_bits is only set once hw_params ran; nothing to report before */
	if (!rt->frame_bits || !rt->dma_bytes)
		return 0;
	pos = (u32)(READ_ONCE(a->base) % rt->dma_bytes);
	return bytes_to_frames(rt, pos);
}

static const struct snd_pcm_ops avm_audio_ops = {
	.open		= avm_audio_pcm_open,
	.close		= avm_audio_pcm_close,
	.hw_params	= avm_audio_pcm_hw_params,
	.prepare	= avm_audio_pcm_prepare,
	.trigger	= avm_audio_pcm_trigger,
	.pointer	= avm_audio_pcm_pointer,
};

/* ------------------------------------------------------------------ */
/* Parser entry: push captured 0x58 payload bytes into the PCM buffer  */

void avm_audio_push(struct avm_device *dev, const u8 *src, u32 bytes)
{
	struct avm_audio *a = READ_ONCE(dev->audio);
	struct snd_pcm_runtime *rt;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	u32 size, pos, first;
	unsigned int periods = 0;

	if (!a || !bytes)
		return;

	spin_lock_irqsave(&a->lock, flags);
	/* no consumer (never opened / stopped / dying): drop on the floor */
	if (a->dying || !a->running || !a->substream) {
		a->missed += bytes;
		spin_unlock_irqrestore(&a->lock, flags);
		return;
	}
	subs = a->substream;
	rt = subs->runtime;
	if (!rt || !rt->dma_area || !rt->dma_bytes) {
		a->missed += bytes;
		spin_unlock_irqrestore(&a->lock, flags);
		return;
	}
	/* one packet (max 8192 B) can never exceed the 1 MB buffer */
	size = rt->dma_bytes;
	if (bytes > size) {
		a->overruns += bytes;
		spin_unlock_irqrestore(&a->lock, flags);
		return;
	}

	pos = (u32)(a->base % size);
	first = size - pos;
	if (first > bytes)
		first = bytes;
	memcpy(rt->dma_area + pos, src, first);
	if (bytes > first)
		memcpy(rt->dma_area, src + first, bytes - first);

	a->base += bytes;
	a->produced += bytes;

	/* one period_elapsed per completed period sweep (a chunk can carry
	 * several periods' worth of audio; 8192 B / 1920 B period = 4) */
	if (rt->frame_bits && rt->period_size) {
		u32 pbytes = frames_to_bytes(rt, rt->period_size);
		u64 before = a->base - bytes;

		if (pbytes)
			periods = (unsigned int)(a->base / pbytes -
						 before / pbytes);
	}
	spin_unlock_irqrestore(&a->lock, flags);

	/*
	 * The substream object outlives the PCM runtime: a concurrent close
	 * only frees runtime and clears subs->runtime (period_elapsed checks
	 * PCM_RUNTIME_CHECK), and the substream itself is freed only with the
	 * card - which cannot run concurrently with a push because fini()
	 * happens after the parser is stopped and flushed on disconnect.
	 */
	while (periods--)
		snd_pcm_period_elapsed(subs);
}

/* ------------------------------------------------------------------ */
/* init / fini                                                         */

int avm_audio_init(struct avm_device *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct avm_audio *a;
	int ret;

	/*
	 * Opt-in (default off).  The card registration used to run at every
	 * probe and its PCM oopsed userspace on PREPARE (missing ops->prepare,
	 * fixed above) - enough to take down the whole audio stack.  Until the
	 * sink is verified on hardware the module stays video-only, exactly
	 * the stable state it had before TODO-1, unless avm_audio=1 is passed.
	 */
	if (!avm_audio) {
		dev_info(&dev->udev->dev,
			 "ALSA capture PCM disabled (avm_audio=0); pass avm_audio=1 to opt in\n");
		return 0;
	}

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	spin_lock_init(&a->lock);
	a->dev = dev;

	ret = snd_card_new(&dev->udev->dev, avm_audio_index, avm_audio_id,
			   THIS_MODULE, 0, &card);
	if (ret < 0) {
		dev_warn(&dev->udev->dev, "snd_card_new failed: %d\n", ret);
		kfree(a);
		return ret;
	}
	a->card = card;
	card->private_data = a;
	card->private_free = avm_audio_card_free;

	strscpy(card->driver, "AVMCV710", sizeof(card->driver));
	strscpy(card->shortname, "AVerMedia ExtremeCap U3",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "AVerMedia ExtremeCap U3 (CV710) HDMI audio capture");
	strscpy(card->mixername, "AVMCV710", sizeof(card->mixername));

	ret = snd_pcm_new(card, "AVMCV710", 0, 0, 1, &pcm);
	if (ret < 0)
		goto err_card;
	a->pcm = pcm;
	pcm->private_data = a;
	/* the parser worker (process context) calls period_elapsed() */
	pcm->nonatomic = 1;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &avm_audio_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0,
				       AVM_AUDIO_RING_BYTES);
	strscpy(pcm->name, "HDMI audio capture", sizeof(pcm->name));

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_card;

	WRITE_ONCE(dev->audio, a);
	dev_info(&dev->udev->dev,
		 "ALSA capture PCM registered (S16LE stereo %u Hz)\n",
		 AVM_AUDIO_RATE);
	return 0;

err_card:
	snd_card_free(card);	/* also runs card->private_free -> kfree(a) */
	return ret;
}

void avm_audio_fini(struct avm_device *dev)
{
	struct avm_audio *a = READ_ONCE(dev->audio);
	unsigned long flags;

	if (!a)
		return;
	spin_lock_irqsave(&a->lock, flags);
	a->dying = true;	/* parser must stop pushing immediately */
	spin_unlock_irqrestore(&a->lock, flags);
	WRITE_ONCE(dev->audio, NULL);

	/* never block disconnect on a running arecord: free when closed */
	snd_card_free_when_closed(a->card);
}