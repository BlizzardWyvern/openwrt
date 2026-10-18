// SPDX-License-Identifier: GPL-2.0-only
/* WD WT6703F WOL Driver
 *
 * Provides sysfs interface for Wake-On-LAN control on WD My Cloud devices.
 * WOL allows the device to power on when receiving a magic packet over the network.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/wd-wt6703f.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define WD_WT6703F_CMD_LEN 7

/* WOL commands */
static const u8 wd_wt6703f_wol_enable_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x03, 0x0a, 0x01, 0x00, 0x00, 0xfb
};

static const u8 wd_wt6703f_wol_disable_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x03, 0x0a, 0x00, 0x00, 0x00, 0xfb
};

static const u8 wd_wt6703f_wol_status_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x03, 0x0a, 0x02, 0x00, 0x00, 0xfb
};

struct wd_wt6703f_wol {
	struct wd_wt6703f *mcu;
	struct mutex lock;
	bool enabled;
};

static int wd_wt6703f_wol_exec(struct wd_wt6703f_wol *priv, const u8 *cmd)
{
	u8 response[16];
	size_t response_len = sizeof(response);

	return wd_wt6703f_exec(priv->mcu, cmd, WD_WT6703F_CMD_LEN,
			      response, &response_len);
}

static int wd_wt6703f_wol_get_status(struct wd_wt6703f_wol *priv)
{
	u8 response[16];
	size_t response_len = sizeof(response);
	int ret;

	mutex_lock(&priv->lock);
	ret = wd_wt6703f_exec(priv->mcu, wd_wt6703f_wol_status_cmd,
			      WD_WT6703F_CMD_LEN, response, &response_len);
	if (!ret && response_len >= 4) {
		/* Response contains status in byte 3 */
		priv->enabled = (response[3] != 0);
	}
	mutex_unlock(&priv->lock);

	return ret;
}

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct wd_wt6703f_wol *priv = dev_get_drvdata(dev);
	int ret;

	ret = wd_wt6703f_wol_get_status(priv);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", priv->enabled ? 1 : 0);
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct wd_wt6703f_wol *priv = dev_get_drvdata(dev);
	unsigned long val;
	const u8 *cmd;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	cmd = val ? wd_wt6703f_wol_enable_cmd : wd_wt6703f_wol_disable_cmd;

	mutex_lock(&priv->lock);
	ret = wd_wt6703f_wol_exec(priv, cmd);
	if (!ret)
		priv->enabled = !!val;
	mutex_unlock(&priv->lock);

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(enabled);

static struct attribute *wd_wt6703f_wol_attrs[] = {
	&dev_attr_enabled.attr,
	NULL,
};

ATTRIBUTE_GROUPS(wd_wt6703f_wol);

static int wd_wt6703f_wol_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wd_wt6703f *mcu = dev_get_drvdata(dev->parent);
	struct wd_wt6703f_wol *priv;
	int ret;

	if (!mcu)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mcu = mcu;
	priv->enabled = false;

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	ret = sysfs_create_groups(&dev->kobj, wd_wt6703f_wol_groups);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	/* Query initial status */
	wd_wt6703f_wol_get_status(priv);

	dev_info(dev, "WD WT6703F WOL driver initialized\n");
	return 0;
}

static void wd_wt6703f_wol_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	sysfs_remove_groups(&dev->kobj, wd_wt6703f_wol_groups);
}

static const struct of_device_id wd_wt6703f_wol_of_match[] = {
	{ .compatible = "wd,wt6703f-wol" },
	{ }
};
MODULE_DEVICE_TABLE(of, wd_wt6703f_wol_of_match);

static struct platform_driver wd_wt6703f_wol_driver = {
	.probe = wd_wt6703f_wol_probe,
	.remove_new = wd_wt6703f_wol_remove,
	.driver = {
		.name = "wd-wt6703f-wol",
		.of_match_table = wd_wt6703f_wol_of_match,
	},
};
module_platform_driver(wd_wt6703f_wol_driver);

MODULE_DESCRIPTION("WD WT6703F Wake-On-LAN driver");
MODULE_AUTHOR("OpenWrt community");
MODULE_LICENSE("GPL");
