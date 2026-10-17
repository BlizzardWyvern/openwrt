// SPDX-License-Identifier: GPL-2.0-only
/* WD WT6703F MCU Driver
 *
 * The WT6703F MCU is connected over UART and controls chassis functions,
 * including the front status LED, fan speed, chassis temperature, 
 * real-time clock (RTC), wake-on-lan (WOL), auto-power-recovery (APR) 
 * and other hardware features used by WD My Cloud devices.
 */

#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/serdev.h>
#include <linux/slab.h>

#include <linux/mfd/wd-wt6703f.h>

#define WD_WT6703F_FRAME_LEN		7
#define WD_WT6703F_RX_BUF_SIZE		32
#define WD_WT6703F_CMD_TIMEOUT		HZ

#define WD_WT6703F_CMD_START		0xfa
#define WD_WT6703F_CMD_STOP			0xfb
#define WD_WT6703F_ACK				0x30

struct wd_wt6703f {
	struct serdev_device *serdev;
	struct mutex lock;
	struct completion reply_ready;
	u8 reply_buf[WD_WT6703F_RX_BUF_SIZE];
	size_t reply_len;
	bool in_frame;
};

static const u8 wd_wt6703f_device_ready_cmd[WD_WT6703F_FRAME_LEN] = {
	0xfa, 0x03, 0x01, 0x00, 0x00, 0x00, 0xfb,
};

static size_t wd_wt6703f_receive_buf(struct serdev_device *serdev,
					const u8 *data, size_t count)
{
	struct wd_wt6703f *wt6703f = serdev_device_get_drvdata(serdev);
	size_t i;

	for (i = 0; i < count; i++) {
		u8 byte = data[i];

		if (!wt6703f->in_frame) {
			if (byte != WD_WT6703F_CMD_START)
				continue;

			wt6703f->reply_len = 0;
			wt6703f->in_frame = true;
		}

		if (wt6703f->reply_len >= WD_WT6703F_RX_BUF_SIZE) {
			/*
			 * Frame too large. Drop it and wait for the next
			 * start byte.
			 */
			wt6703f->in_frame = false;
			continue;
		}

		wt6703f->reply_buf[wt6703f->reply_len++] = byte;

		if (byte == WD_WT6703F_CMD_STOP) {
			wt6703f->in_frame = false;
			complete(&wt6703f->reply_ready);
		}
	}

	return count;
}

static const struct serdev_device_ops wd_wt6703f_serdev_ops = {
	.receive_buf = wd_wt6703f_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

int wd_wt6703f_exec(struct wd_wt6703f *wt6703f, const u8 *cmd, size_t cmd_len,
			u8 *reply, size_t *reply_len)
{
	unsigned long timeout;
	int ret;

	if (!wt6703f || !cmd || cmd_len != WD_WT6703F_FRAME_LEN)
		return -EINVAL;

	if (cmd[0] != WD_WT6703F_CMD_START || cmd[cmd_len - 1] != WD_WT6703F_CMD_STOP)
		return -EINVAL;

	mutex_lock(&wt6703f->lock);

	reinit_completion(&wt6703f->reply_ready);
	wt6703f->reply_len = 0;
	wt6703f->in_frame = false;

	serdev_device_write_flush(wt6703f->serdev);
	dev_info(&wt6703f->serdev->dev,
	 "sending: %*ph\n", (int)cmd_len, cmd);
	ret = serdev_device_write_buf(wt6703f->serdev, cmd, cmd_len);
	if (ret < 0)
		goto out_unlock;
	if (ret != cmd_len) {
		ret = -EIO;
		goto out_unlock;
	}

	serdev_device_wait_until_sent(wt6703f->serdev, WD_WT6703F_CMD_TIMEOUT);
	timeout = wait_for_completion_timeout(&wt6703f->reply_ready,
					      WD_WT6703F_CMD_TIMEOUT);
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	if (wt6703f->reply_len < 5) {
		ret = -EIO;
		goto out_unlock;
	}

	dev_info(&wt6703f->serdev->dev,
		"MCU reply (%zu bytes): %*ph\n",
		wt6703f->reply_len,
		(int)wt6703f->reply_len,
		wt6703f->reply_buf);

	if (wt6703f->reply_len != WD_WT6703F_FRAME_LEN) {
		ret = -EPROTO;
		goto out_unlock;
	}

	if (wt6703f->reply_buf[0] != WD_WT6703F_CMD_START ||
		wt6703f->reply_buf[1] != WD_WT6703F_ACK ||
		wt6703f->reply_buf[2] != 0x00 ||
		wt6703f->reply_buf[3] != 0x00 ||
		wt6703f->reply_buf[4] != 0x00 ||
		wt6703f->reply_buf[5] != 0x00 ||
		wt6703f->reply_buf[6] != WD_WT6703F_CMD_STOP) {
		dev_err(&wt6703f->serdev->dev,
			"Invalid MCU response: %*ph\n",
			(int)wt6703f->reply_len, wt6703f->reply_buf);
		ret = -EPROTO;
		goto out_unlock;
	}

	if (reply && reply_len) {
		size_t copy_len = min(*reply_len, wt6703f->reply_len);

		memcpy(reply, wt6703f->reply_buf, copy_len);
		*reply_len = copy_len;
	}

	ret = 0;

out_unlock:
	mutex_unlock(&wt6703f->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(wd_wt6703f_exec);

static int wd_wt6703f_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct wd_wt6703f *wt6703f;
	size_t reply_len = 0;
	u32 baud = 19200;
	int ret;

	wt6703f = devm_kzalloc(dev, sizeof(*wt6703f), GFP_KERNEL);
	if (!wt6703f)
		return -ENOMEM;

	wt6703f->serdev = serdev;
	init_completion(&wt6703f->reply_ready);
	ret = devm_mutex_init(dev, &wt6703f->lock);
	if (ret)
		return ret;

	if (device_property_read_u32(dev, "current-speed", &baud))
		dev_dbg(dev, "No current-speed set, defaulting to 19200 baud\n");

	serdev_device_set_drvdata(serdev, wt6703f);
	serdev_device_set_client_ops(serdev, &wd_wt6703f_serdev_ops);

	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return ret;

	ret = serdev_device_set_baudrate(serdev, baud);
	if (ret < 0)
		return dev_err_probe(dev, ret,
					"Failed to set baudrate\n");

	serdev_device_set_flow_control(serdev, false);

	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set serial parity\n");

	ret = wd_wt6703f_exec(wt6703f, wd_wt6703f_device_ready_cmd,
			      sizeof(wd_wt6703f_device_ready_cmd), NULL,
			      &reply_len);
	if (ret)
		dev_warn(dev, "DeviceReady command failed: %d\n", ret);

	return devm_of_platform_populate(dev);
}

static const struct of_device_id wd_wt6703f_of_match[] = {
	{ .compatible = "wd,wt6703f" },
	{ }
};
MODULE_DEVICE_TABLE(of, wd_wt6703f_of_match);

static struct serdev_device_driver wd_wt6703f_driver = {
	.probe = wd_wt6703f_probe,
	.driver = {
		.name = "wd-wt6703f",
		.of_match_table = wd_wt6703f_of_match,
	},
};
module_serdev_device_driver(wd_wt6703f_driver);

MODULE_DESCRIPTION("WD WT6703F MCU driver");
MODULE_AUTHOR("OpenWrt community");
MODULE_LICENSE("GPL");
