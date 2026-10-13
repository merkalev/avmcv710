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
/*
 * usb_bulk_msg() needs a DMA-able buffer: callers routinely pass stack
 * arrays, which are vmalloc'd under CONFIG_VMAP_STACK.  Bounce every EP1
 * transfer through kmalloc'd memory here so no caller has to care.
 */
static int avm_ep1_write_t(struct avm_device *dev, const u8 *data,
			   size_t len, int timeout)
{
	int actual = 0;
	int ret;
	u8 *buf;

	if (WARN_ON_ONCE(len > 2048))
		return -EINVAL;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, AVM_EP_CTRL_OUT),
			   buf, len, &actual, timeout);
	kfree(buf);
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
	u8 *buf;

	if (WARN_ON_ONCE(len > 2048))
		return -EINVAL;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, AVM_EP_CTRL_IN),
			   buf, len, &actual, timeout);
	if (ret >= 0 && actual > 0)
		memcpy(data, buf, min_t(size_t, actual, len));
	kfree(buf);
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

/*
 * Drain any bytes left in the EP1 IN FIFO by a previous session.
 *
 * If a status/fpga read is aborted (URB killed on timeout) after the FX3 has
 * already queued its reply, that reply sits in the endpoint FIFO while the
 * host's data toggle is left behind.  The next bootstrap then reads the stale
 * byte as a short packet (-EIO), every following read is shifted, and the FX3
 * eventually overruns (-EOVERFLOW) and wedges the endpoint.  Reading the
 * leftover out with the same pipe advances the host toggle to match the
 * device, so the next bootstrap starts clean.  A short read or a timeout
 * means the FIFO is empty.
 */
void avm_ep1_drain(struct avm_device *dev)
{
	u8 *buf;
	int actual, ret, i;

	buf = kmalloc(512, GFP_KERNEL);
	if (!buf)
		return;

	for (i = 0; i < 4; i++) {
		actual = 0;
		ret = usb_bulk_msg(dev->udev,
				   usb_rcvbulkpipe(dev->udev, AVM_EP_CTRL_IN),
				   buf, 512, &actual, 20);
		if (ret || actual < 512)
			break;
	}
	kfree(buf);
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
	if (ret)
		avm_ep1_drain(dev);
	mutex_unlock(&dev->ctrl_lock);
	return ret;
}

int avm_i2c_read_block(struct avm_device *dev, u8 slave7, u8 reg,
		       u8 *data, u8 len)
{
	u8 cmd[5] = { 0x02, slave7, 0x01, len, reg };
	int ret, wret;

	if (WARN_ON_ONCE(len == 0 || len > 32))
		return -EINVAL;

	mutex_lock(&dev->ctrl_lock);
	wret = avm_ep1_write_t(dev, cmd, sizeof(cmd), AVM_STATUS_IO_TIMEOUT);
	if (!wret)
		ret = avm_ep1_read_t(dev, data, len, AVM_STATUS_IO_TIMEOUT);
	else
		ret = wret;
	if (ret) {
		/*
		 * A failed read can leave the FX3's late reply sitting in the
		 * EP1 IN FIFO; the next control command then reads that stale
		 * byte, desyncs, and wedges the endpoint.  Consume it now.
		 */
		avm_ep1_drain(dev);
		dev_warn_ratelimited(&dev->udev->dev,
			"i2c read %02x/%02x len %u failed: w=%d r=%d\n",
			slave7, reg, len, wret, ret);
	}
	mutex_unlock(&dev->ctrl_lock);
	return ret;
}

/*
 * FPGA work mode: indirect control at I2C 0x10 reg 0x05.
 * bit1 set = work (streaming), bit1 clear = idle (bootstrap / parked).
 * Matches CLFE3::SetWorkMode / SetIdleMode.
 *
 * Write-only.  The bootstrap's last write to 0x05 is exactly 0x02 (work bit
 * only), so the register's other bits are zero and there is nothing to
 * preserve.  The old read-modify-write read 0x05 first, and that EP1 IN
 * transfer intermittently fails -EAGAIN just after the bootstrap and at
 * STREAMON (the FX3 is still busy); the aborted URB then leaves the endpoint
 * desynced and the following control command times out.  Writing the known
 * value avoids the failing read entirely.  The same is safe before the
 * bootstrap: the command list reprograms 0x05 from scratch.
 */
int avm_fpga_work_mode(struct avm_device *dev, bool work)
{
	u8 mode = work ? 0x02 : 0x00;
	int ret;

	ret = avm_i2c_write(dev, 0x10, 0x05, mode);
	if (ret)
		dev_warn_ratelimited(&dev->udev->dev,
			"fpga work mode: write 0x10/0x05=0x%02x failed: %d\n",
			mode, ret);
	return ret;
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
	if (ret)
		dev_warn_ratelimited(&dev->udev->dev,
			"reset stream DMA (0x14) failed: %d\n", ret);
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

		/* Never feed the 2048-byte scratch more than it holds: a
		 * regenerated trace with an oversized entry would overrun
		 * the stack/heap buffer below (today's table max is 512). */
		if (WARN_ON_ONCE(c->len > 2048)) {
			ret = -EINVAL;
			break;
		}

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