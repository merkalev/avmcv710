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

/* Bulk EP1 primitives.  len must be <= 512 (fits one max-packet TLP
 * group).  Bootstrap uses generous timeouts; live status reads use the
 * same 500 ms the userspace driver does so a wedged control pipe cannot
 * stall the poll worker for seconds. */
static int avm_ep1_write_t(struct avm_device *dev, const u8 *data,
			   size_t len, int timeout)
{
	int actual = 0;
	int ret;

	if (WARN_ON_ONCE(len > 2048))
		return -EINVAL;

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, AVM_EP_CTRL_OUT),
			   (void *)data, len, &actual, timeout);
	if (ret < 0)
		return ret;
	if (actual != len)
		return -EIO;
	return 0;
}

static int avm_ep1_read_t(struct avm_device *dev, u8 *data, size_t len,
			  int timeout)
{
	int actual = 0;
	int ret;

	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, AVM_EP_CTRL_IN),
			   data, len, &actual, timeout);
	if (ret < 0)
		return ret;
	if (actual != len)
		return -EIO;
	return 0;
}

int avm_ep1_write(struct avm_device *dev, const u8 *data, size_t len)
{
	return avm_ep1_write_t(dev, data, len, AVM_BOOT_WRITE_TIMEOUT);
}

int avm_ep1_read(struct avm_device *dev, u8 *data, size_t len)
{
	return avm_ep1_read_t(dev, data, len, AVM_BOOT_READ_TIMEOUT);
}

/* Status-path EP1 transactions (500 ms, like the userspace driver). */
#define AVM_STATUS_IO_TIMEOUT	500

/*
 * I2C bridge (matches UsbStream::sendI2cWrite/sendI2cRead exactly):
 *   write: 5-byte "01 <slave7> 01 <reg> <val>"
 *   read:  5-byte "02 <slave7> 01 <len> <reg>", then <len> bytes on EP1 IN.
 * Every command+response pair is atomic under dev->ctrl_lock so the status
 * worker can poll the ADV7604 while start/stop toggles the FPGA mode
 * without two transactions interleaving (a response stolen by the other
 * thread would be garbage, exactly the class of CV-03 bug the userspace
 * driver serializes against).
 */
int avm_i2c_write(struct avm_device *dev, u8 slave7, u8 reg, u8 val)
{
	u8 cmd[5] = { 0x01, slave7, 0x01, reg, val };
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = avm_ep1_write_t(dev, cmd, sizeof(cmd), AVM_STATUS_IO_TIMEOUT);
	mutex_unlock(&dev->ctrl_lock);
	return ret;
}

int avm_i2c_read_block(struct avm_device *dev, u8 slave7, u8 reg,
		       u8 *data, u8 len)
{
	u8 cmd[5] = { 0x02, slave7, 0x01, len, reg };
	int ret;

	if (WARN_ON_ONCE(len == 0 || len > 32))
		return -EINVAL;

	mutex_lock(&dev->ctrl_lock);
	ret = avm_ep1_write_t(dev, cmd, sizeof(cmd), AVM_STATUS_IO_TIMEOUT);
	if (!ret)
		ret = avm_ep1_read_t(dev, data, len, AVM_STATUS_IO_TIMEOUT);
	mutex_unlock(&dev->ctrl_lock);
	return ret;
}

/*
 * FPGA work mode: ADV7604-style indirect control at I2C 0x10 reg 0x05.
 * bit1 set = work (streaming), bit1 clear = idle (bootstrap / parked).
 * Matches CLFE3::SetWorkMode / SetIdleMode.
 */
int avm_fpga_work_mode(struct avm_device *dev, bool work)
{
	u8 mode = 0;

	avm_i2c_read_block(dev, 0x10, 0x05, &mode, 1);
	if (work)
		mode |= 0x02;
	else
		mode &= ~0x02;
	return avm_i2c_write(dev, 0x10, 0x05, mode);
}

/*
 * Single byte 0x14 on EP1 OUT: resets the FX3 stream DMA, clearing any
 * internal FIFO pointers before streaming begins.  Mirrors
 * Fx3Firmware::resetStreamDMA.  Serialized under ctrl_lock with the
 * other control traffic.
 */
int avm_reset_stream_dma(struct avm_device *dev)
{
	u8 cmd = 0x14;
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = avm_ep1_write(dev, &cmd, 1);
	mutex_unlock(&dev->ctrl_lock);
	return ret;
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