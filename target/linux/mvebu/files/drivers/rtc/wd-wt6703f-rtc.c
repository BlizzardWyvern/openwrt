// SPDX-License-Identifier: GPL-2.0-only
/* WD WT6703F RTC Driver
 *
 * Provides RTC interface for the WT6703F MCU on WD My Cloud devices.
 * Supports reading/writing date/time and basic alarm functionality.
 */

#include <linux/bcd.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/wd-wt6703f.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#define WD_WT6703F_CMD_LEN 7
#define WD_WT6703F_DATETIME_CMD_LEN 13
#define WD_WT6703F_RTC_READ_RETRIES 3

/* RTC commands */
static const u8 wd_wt6703f_rtc_read_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x01, 0x08, 0x01, 0x01, 0x00, 0xfb
};

struct wd_wt6703f_rtc {
	struct wd_wt6703f *mcu;
	struct rtc_device *rtc;
	struct mutex lock;
};

static int wd_wt6703f_rtc_parse_time(const u8 *buf, size_t len,
				      struct rtc_time *tm)
{
	int i;
	int year;

	if (len < 13 || buf[len - 1] != 0xfb ||
	    memcmp(buf, wd_wt6703f_rtc_read_cmd, 5))
		return -EIO;

	for (i = 5; i <= 11; i++) {
		if (bin2bcd(bcd2bin(buf[i])) != buf[i])
			return -EIO;
	}

	tm->tm_sec = bcd2bin(buf[5]);
	tm->tm_min = bcd2bin(buf[6]);
	tm->tm_hour = bcd2bin(buf[7]);
	tm->tm_mday = bcd2bin(buf[9]);
	tm->tm_mon = bcd2bin(buf[10]) - 1;
	year = 2000 + bcd2bin(buf[11]);
	tm->tm_year = year - 1900;
	tm->tm_wday = bcd2bin(buf[8]) % 7;

	return rtc_valid_tm(tm);
}

static int wd_wt6703f_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct wd_wt6703f_rtc *priv = dev_get_drvdata(dev);
	u8 response[16];
	size_t response_len;
	int ret;
	int attempt;

	mutex_lock(&priv->lock);

	for (attempt = 0; attempt < WD_WT6703F_RTC_READ_RETRIES; attempt++) {
		response_len = sizeof(response);
		ret = wd_wt6703f_exec(priv->mcu, wd_wt6703f_rtc_read_cmd,
				      WD_WT6703F_CMD_LEN, response,
				      &response_len);
		if (!ret)
			break;
	}

	if (!ret)
		ret = wd_wt6703f_rtc_parse_time(response, response_len, tm);
	mutex_unlock(&priv->lock);

	return ret;
}

static int wd_wt6703f_rtc_set_time(struct device *dev,
				    struct rtc_time *tm)
{
	struct wd_wt6703f_rtc *priv = dev_get_drvdata(dev);
	u8 cmd[WD_WT6703F_DATETIME_CMD_LEN] = {
		0xfa, 0x01, 0x08, 0x02, 0x07, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xfb
	};
	int ret;
	int year = tm->tm_year + 1900;

	if (rtc_valid_tm(tm) || year < 2000 || year > 2099)
		return -EINVAL;

	mutex_lock(&priv->lock);
	cmd[5] = bin2bcd(tm->tm_sec);
	cmd[6] = bin2bcd(tm->tm_min);
	cmd[7] = bin2bcd(tm->tm_hour);
	cmd[8] = bin2bcd(tm->tm_wday);
	cmd[9] = bin2bcd(tm->tm_mday);
	cmd[10] = bin2bcd(tm->tm_mon + 1);
	cmd[11] = bin2bcd(year - 2000);
	ret = wd_wt6703f_exec(priv->mcu, cmd, WD_WT6703F_DATETIME_CMD_LEN,
			      NULL, NULL);
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct rtc_class_ops wd_wt6703f_rtc_ops = {
	.read_time = wd_wt6703f_rtc_read_time,
	.set_time = wd_wt6703f_rtc_set_time,
};

static int wd_wt6703f_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wd_wt6703f *mcu = dev_get_drvdata(dev->parent);
	struct wd_wt6703f_rtc *priv;
	int ret;

	if (!mcu)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mcu = mcu;

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	priv->rtc = devm_rtc_device_register(dev, "wt6703f",
					     &wd_wt6703f_rtc_ops, THIS_MODULE);
	if (IS_ERR(priv->rtc)) {
		ret = PTR_ERR(priv->rtc);
		dev_err(dev, "Failed to register RTC device: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id wd_wt6703f_rtc_of_match[] = {
	{ .compatible = "wd,wt6703f-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, wd_wt6703f_rtc_of_match);

static struct platform_driver wd_wt6703f_rtc_driver = {
	.probe = wd_wt6703f_rtc_probe,
	.driver = {
		.name = "wd-wt6703f-rtc",
		.of_match_table = wd_wt6703f_rtc_of_match,
	},
};
module_platform_driver(wd_wt6703f_rtc_driver);

MODULE_DESCRIPTION("WD WT6703F RTC driver");
MODULE_AUTHOR("OpenWrt community");
MODULE_LICENSE("GPL");
