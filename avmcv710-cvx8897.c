// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-cvx8897.c - CVX8897 hardware frame-rate converter.
 *
 * The CVX8897 sits between the ADV7604 HDMI receiver and the capture path.
 * The vendor exposes it as the "Video Input Frame Rate" [60p]/[30p]/[50p]/
 * [25p] selector: it halves the delivered frame rate in hardware.  It does
 * *not* scale geometry - every table keeps the input raster and merely drops
 * alternate frames - so this driver still reports the live source size.
 *
 * The 251-register images were recovered from the vendor drivers and live in
 * the generated avmcv710-cvx8897.h (regenerate with tools/gen_cvx8897.py;
 * see ../cv710userspace/tools/cvx8897/README.md).
 *
 * Transport: the converter is at 7-bit I2C 0x18 and its reset / output-enable
 * lines hang off a PCA9539 GPIO expander at 7-bit I2C 0x74, both behind the
 * FX3 EP1 bridge (avm_i2c_read_block / avm_i2c_write).  Mirrors
 * UsbStream::cvx8897SetModeImpl / pca9539SetGpio / cvx8897SetAllOutputModeBits.
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>

#include "avmcv710.h"
#include "avmcv710-cvx8897.h"

/* PCA9539 GPIO expander (vendor CPCA9539A, 8-bit 0xE8).  CVX8897 uses port
 * 0: bit 4 = active-low reset, bit 3 = output enable.  Each pin's direction
 * lives in the config register (0x06/0x07, 1 = input) and its level in the
 * output register (0x02/0x03). */
#define AVM_PCA9539_ADDR	0x74
#define AVM_PCA9539_REG_OUT0	0x02
#define AVM_PCA9539_REG_CFG0	0x06
#define AVM_PCA9539_PIN_RESET	4
#define AVM_PCA9539_PIN_ENABLE	3

/* Vendor HWReset(): reset high, 1 s, low, 1 s, high, 1 s. */
#define AVM_CVX_RESET_DELAY_MS	1000

/* CVX8897::SetAllOutputMode() registers. */
#define AVM_CVX_REG_OUT_MODE0	0x8a	/* bit 1 */
#define AVM_CVX_REG_OUT_MODE1	0x0d	/* bit 7 */

static int avm_pca9539_set_gpio(struct avm_device *dev, u8 pin, bool value)
{
	const u8 bit = BIT(pin & 7);
	u8 cfg, out;

	if (avm_i2c_read_block(dev, AVM_PCA9539_ADDR, AVM_PCA9539_REG_CFG0,
			       &cfg, 1))
		return -EIO;
	cfg = (cfg & ~bit);		/* direction: output */
	if (avm_i2c_write(dev, AVM_PCA9539_ADDR, AVM_PCA9539_REG_CFG0, cfg))
		return -EIO;

	if (avm_i2c_read_block(dev, AVM_PCA9539_ADDR, AVM_PCA9539_REG_OUT0,
			       &out, 1))
		return -EIO;
	if (value)
		out |= bit;
	else
		out &= ~bit;
	return avm_i2c_write(dev, AVM_PCA9539_ADDR, AVM_PCA9539_REG_OUT0, out);
}

static int avm_cvx_set_all_output_mode_bits(struct avm_device *dev, bool set)
{
	u8 v;

	if (avm_i2c_read_block(dev, AVM_CVX_I2C_ADDR, AVM_CVX_REG_OUT_MODE0,
			       &v, 1))
		return -EIO;
	v = set ? (v | 0x02) : (v & ~0x02);
	if (avm_i2c_write(dev, AVM_CVX_I2C_ADDR, AVM_CVX_REG_OUT_MODE0, v))
		return -EIO;

	if (avm_i2c_read_block(dev, AVM_CVX_I2C_ADDR, AVM_CVX_REG_OUT_MODE1,
			       &v, 1))
		return -EIO;
	v = set ? (v | 0x80) : (v & ~0x80);
	return avm_i2c_write(dev, AVM_CVX_I2C_ADDR, AVM_CVX_REG_OUT_MODE1, v);
}

/*
 * Vendor CVX8897::SetMode(2) (converted output) / SetMode(1) (native
 * passthrough): reset pulse, SetAllOutputMode(converted ? 0 : 1), then drive
 * the output-enable line.  Sleeps ~3 s, so callers must be in process context
 * and must not hold a lock the streaming path needs.
 */
int avm_cvx8897_set_mode(struct avm_device *dev, bool converted)
{
	if (avm_pca9539_set_gpio(dev, AVM_PCA9539_PIN_RESET, true))
		return -EIO;
	msleep(AVM_CVX_RESET_DELAY_MS);
	if (avm_pca9539_set_gpio(dev, AVM_PCA9539_PIN_RESET, false))
		return -EIO;
	msleep(AVM_CVX_RESET_DELAY_MS);
	if (avm_pca9539_set_gpio(dev, AVM_PCA9539_PIN_RESET, true))
		return -EIO;
	msleep(AVM_CVX_RESET_DELAY_MS);

	if (avm_cvx_set_all_output_mode_bits(dev, !converted))
		return -EIO;
	return avm_pca9539_set_gpio(dev, AVM_PCA9539_PIN_ENABLE, converted);
}

/* Upload one full register image: 251 writes of {reg_order[i], values[i]}. */
int avm_cvx8897_apply(struct avm_device *dev, const u8 *values)
{
	unsigned int i;
	int failures = 0;

	for (i = 0; i < AVM_CVX_REG_COUNT; i++) {
		if (avm_i2c_write(dev, AVM_CVX_I2C_ADDR,
				  avm_cvx_reg_order[i], values[i]))
			failures++;
	}
	return failures;
}

/*
 * Pick the half-rate table (60->30 / 50->25) matching a detected input, or
 * NULL to leave the converter native.  Port of cvx8897::selectHalfRate(): the
 * tables are progressive-only and exist only for the 60 Hz and 50 Hz families,
 * so any other rate (24p, 30p, ...) or an interlaced source returns NULL.
 *
 * `fps` must be the *input* rate, not the delivered one.
 */
const u8 *avm_cvx8897_half_rate(u32 w, u32 h, u32 fps, bool rgb, bool interlaced)
{
	const bool fifty = fps >= 45 && fps < 55;
	const bool sixty = fps >= 55 && fps < 66;

	if (interlaced || (!fifty && !sixty))
		return NULL;

	if (w == 1920 && h == 1080)
		return fifty ? avm_cvx_1920_1080_50P_422_To_1920_1080_25P_422
			     : avm_cvx_1920_1080_60P_422_To_1920_1080_30P_422;

	if (w == 1280 && h == 720) {
		if (fifty)
			return avm_cvx_1280_720_50P_422_To_1280_720_25P_422;
		return rgb ? avm_cvx_1280_720_60P_RGB_To_1280_720_30P_RGB
			   : avm_cvx_1280_720_60P_422_To_1280_720_30P_422;
	}

	/* The remaining registered modes only have 60->30 tables. */
	if (!sixty)
		return NULL;
	if ((w == 1360 || w == 1366) && h == 768)
		return avm_cvx_1360_768_60P_422_To_1360_768_30P_422;
	if (w == 1680 && h == 1050)
		return avm_cvx_1680_1050_60P_422_To_1680_1050_30P_422;
	if (w == 1440 && h == 900)
		return avm_cvx_1440_900_60P_422_To_1440_900_30P_422;
	if (w == 1280 && h == 1024)
		return avm_cvx_1280_1024_60P_422_To_1280_1024_30P_422;
	if (w == 1280 && h == 800)
		return avm_cvx_1280_800_60P_422_To_1280_800_30P_422;
	if (w == 1280 && h == 768)
		return avm_cvx_1280_768_60P_422_To_1280_768_30P_422;
	if (w == 1024 && h == 768)
		return avm_cvx_1024_768_60P_422_To_1024_768_30P_422;
	if (w == 800 && h == 600)
		return avm_cvx_800_600_60P_422_To_800_600_30P_422;

	return NULL;
}

/*
 * Frame-rate conversion is opt-in.  The decision needs the *input* rate (to
 * tell "half the source" from "the source rate"), and Stage 0 runs no ADV7604
 * poll, so it is inferred from the delivered rate while the converter is
 * native.  0 = leave the converter alone (default); 1 = follow s_parm and
 * halve the rate when the consumer asks for about half the source rate.
 * Writable at runtime via /sys/module/avmcv710/parameters/cvx8897.
 */
static int cvx8897;
module_param(cvx8897, int, 0644);
MODULE_PARM_DESC(cvx8897,
	"CVX8897 frame-rate conversion: 0=off (default), 1=follow s_parm");

bool avm_cvx8897_enabled(void)
{
	return cvx8897 != 0;
}

static void avm_cvx_work(struct work_struct *work)
{
	struct avm_device *dev = container_of(work, struct avm_device, cvx_work);
	struct avm_parser *p = &dev->parser;
	const u8 *table;
	u32 w, h, in_fps, req, serial;
	bool interlaced, want_half;
	int ret, failures;

	if (!cvx8897)
		return;

	w = READ_ONCE(p->active_w);
	h = READ_ONCE(p->active_h);
	in_fps = READ_ONCE(p->measured_fps);
	interlaced = READ_ONCE(p->active_interlaced);
	serial = READ_ONCE(p->mode_serial);
	req = READ_ONCE(dev->cvx_req_fps);

	/* Need a locked geometry and a measured rate. */
	if (!w || !h || !in_fps)
		return;

	/*
	 * While halving, measured_fps is the delivered rate; recover the input
	 * rate for the decision (and for re-selection after a mode switch that
	 * happens while converted).
	 */
	if (dev->cvx_prog_half)
		in_fps *= 2;

	/*
	 * "Select N fps": convert only when the consumer asks for about half
	 * the input rate.  Asking for the input rate itself - or anything
	 * else, or nothing - means native passthrough.
	 */
	want_half = req && req != in_fps &&
		    req * 2 >= in_fps - in_fps / 8 &&
		    req * 2 <= in_fps + in_fps / 8;

	/*
	 * Assume YCbCr 4:2:2.  The RGB-vs-422 choice only matters for
	 * 1280x720@60 (the vendor has a dedicated RGB table); distinguishing
	 * them needs the ADV7604 AVI poll (Stage 2), so a 720p60 RGB source
	 * gets the 422 table for now.
	 */
	table = want_half ?
		avm_cvx8897_half_rate(w, h, in_fps, false, interlaced) : NULL;
	if (!table)
		want_half = false;

	if (want_half) {
		if (dev->cvx_prog_half && dev->cvx_prog_serial == serial)
			return;		/* already programmed for this input */
		dev_info(&dev->udev->dev, "cvx8897: halving %ux%u@%u\n",
			 w, h, in_fps);
		ret = avm_cvx8897_set_mode(dev, true);
		if (ret) {
			dev_warn_ratelimited(&dev->udev->dev,
					"cvx8897: SetMode(half) failed: %d\n",
					ret);
			return;
		}
		failures = avm_cvx8897_apply(dev, table);
		dev->cvx_prog_half = true;
		dev->cvx_prog_serial = serial;
		if (failures)
			dev_warn_ratelimited(&dev->udev->dev,
					"cvx8897: %d/%u writes failed\n",
					failures, AVM_CVX_REG_COUNT);
		return;
	}

	/* Native passthrough.  The bootstrap leaves the converter native, so
	 * there is nothing to do unless a previous stream selected half. */
	if (!dev->cvx_prog_half)
		return;
	dev_info(&dev->udev->dev, "cvx8897: restoring native %ux%u@%u\n",
		 w, h, in_fps);
	ret = avm_cvx8897_set_mode(dev, false);
	dev->cvx_prog_half = false;
	dev->cvx_prog_serial = serial;
	if (ret)
		dev_warn_ratelimited(&dev->udev->dev,
				"cvx8897: SetMode(native) failed: %d\n", ret);
}

void avm_cvx8897_kick(struct avm_device *dev)
{
	if (!dev->cvx_wq)
		return;
	queue_work(dev->cvx_wq, &dev->cvx_work);
}

void avm_cvx8897_request_fps(struct avm_device *dev, u32 fps)
{
	if (fps > 240)
		fps = 0;		/* bogus: treat as "not requested" */
	WRITE_ONCE(dev->cvx_req_fps, fps);
	avm_cvx8897_kick(dev);
}

int avm_cvx8897_init(struct avm_device *dev)
{
	dev->cvx_wq = alloc_ordered_workqueue("avmcv710-cvx", 0);
	if (!dev->cvx_wq)
		return -ENOMEM;
	INIT_WORK(&dev->cvx_work, avm_cvx_work);
	return 0;
}

void avm_cvx8897_fini(struct avm_device *dev)
{
	if (!dev->cvx_wq)
		return;
	cancel_work_sync(&dev->cvx_work);
	destroy_workqueue(dev->cvx_wq);
	dev->cvx_wq = NULL;
}
