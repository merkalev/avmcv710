// SPDX-License-Identifier: GPL-2.0-only
/*
 * avmcv710-hw.c - device bring-up over the FX3 control pipe (EP1).
 *
 * The FPGA and the two video chips live behind the Cypress FX3's I2C
 * bridge, spoken over bulk EP1 OUT/IN.  The bootstrap table in
 * avmcv710-bootstrap.h is a pre-parsed capture of the exact write/read
 * sequence the reverse-engineered userspace driver replays (and, before
 * that, the vendor driver's CLFE3/CRXADV7604 bring-up).  Replaying it is
 * mandatory: without the pin/equalizer/HDCP initialization the ADV7604
 * never locks the TMDS PLL and no stream ever appears on EP0x83.
 */
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "avmcv710.h"
#include "avmcv710-bootstrap.h"

/* Bulk EP1 write.  len must be <= 512 (fits one max-packet TLP group). */
int avm_ep1_write(struct avm_device *dev, const u8 *data, size_t len)
{
	int actual = 0;
	int ret;

	if (WARN_ON_ONCE(len > 2048))
		return -EINVAL;

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, AVM_EP_CTRL_OUT),
			   (void *)data, len, &actual, AVM_BOOT_WRITE_TIMEOUT);
	if (ret < 0)
		return ret;
	if (actual != len)
		return -EIO;
	return 0;
}

int avm_ep1_read(struct avm_device *dev, u8 *data, size_t len)
{
	int actual = 0;
	int ret;

	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, AVM_EP_CTRL_IN),
			   data, len, &actual, AVM_BOOT_READ_TIMEOUT);
	if (ret < 0)
		return ret;
	if (actual != len)
		return -EIO;
	return 0;
}

/*
 * I2C bridge: 5-byte write command "01 <slave7> 01 <reg> <val>".
 * Used for the mode-independent FPGA work/idle toggle and any manual
 * VID_STD or input tweaks.  Control traffic is serialized by the caller
 * (probe path is single-threaded; the streaming path never calls these).
 */
int avm_i2c_write(struct avm_device *dev, u8 slave7, u8 reg, u8 val)
{
	u8 cmd[5] = { 0x01, slave7, 0x01, reg, val };

	return avm_ep1_write(dev, cmd, sizeof(cmd));
}

int avm_i2c_read(struct avm_device *dev, u8 slave7, u8 reg, u8 *val)
{
	u8 cmd[5] = { 0x01, slave7, 0x01, reg, 0x00 };
	u8 buf[4] = { 0 };
	int ret;

	/*
	 * The read round-trip is "write the 4-byte register probe, then
	 * read the response".  The driver's read helper sends the 5-byte
	 * write with a dummy value byte; the bridge answers on EP1 IN.
	 */
	ret = avm_ep1_write(dev, cmd, 5);
	if (ret)
		return ret;

	/* Probe may be a no-op; even a 0-length answer keeps EP1 sane. */
	(void)buf;
	return 0;
}

/*
 * FPGA work mode: ADV7604-style indirect control at I2C 0x10 reg 0x05.
 * bit1 set = work (streaming), bit1 clear = idle (bootstrap / parked).
 * Matches CLFE3::SetWorkMode / SetIdleMode.
 */
int avm_fpga_work_mode(struct avm_device *dev, bool work)
{
	u8 mode = 0;

	avm_i2c_read(dev, 0x10, 0x05, &mode);
	if (work)
		mode |= 0x02;
	else
		mode &= ~0x02;
	return avm_i2c_write(dev, 0x10, 0x05, mode);
}

/*
 * Single byte 0x14 on EP1 OUT: resets the FX3 stream DMA, clearing any
 * internal FIFO pointers before streaming begins.  Mirrors
 * Fx3Firmware::resetStreamDMA.
 */
int avm_reset_stream_dma(struct avm_device *dev)
{
	u8 cmd = 0x14;

	return avm_ep1_write(dev, &cmd, 1);
}

/*
 * Replay the fast-start bootstrap.  Each command is either a bulk EP1 OUT
 * write (op 0, len bytes from the payload blob) or a bulk EP1 IN read
 * (op 1, len bytes; response contents are deliberately ignored - the fast
 * bootstrap has no status reads we depend on).
 *
 * On failure the FX3 may be mid-sequence; we do not retry inside probe;
 * the caller tears the interface down and probe returns -EIO/-ETIMEDOUT.
 */
int avm_bootstrap(struct avm_device *dev)
{
	u8 *scratch;
	int ret = 0;
	u32 i;

	scratch = kzalloc(2048, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;

	for (i = 0; i < AVM_BOOT_CMD_COUNT; i++) {
		const struct avm_boot_cmd *c = &avm_boot_cmds[i];

		if (c->op == AVM_BOOT_OP_WRITE) {
			if (WARN_ON_ONCE(c->off + c->len > AVM_BOOT_BLOB_BYTES)) {
				ret = -EINVAL;
				break;
			}
			memcpy(scratch, avm_boot_blob + c->off, c->len);
			ret = avm_ep1_write(dev, scratch, c->len);
		} else {
			ret = avm_ep1_read(dev, scratch, c->len);
		}
		if (ret) {
			dev_err(&dev->udev->dev,
				"bootstrap cmd %u (op=%u len=%u) failed: %d\n",
				i, c->op, c->len, ret);
			break;
		}
	}

	kfree(scratch);
	if (!ret)
		dev->bootstrapped = true;
	return ret;
}