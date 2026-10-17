// SPDX-License-Identifier: GPL-2.0-only
/* WD WT6703F Thermal Driver
 *
 * Provides thermal zone interface for temperature monitoring on WD My Cloud devices.
 * Uses reverse-engineered thermal table for ADC to temperature conversion.
 */

#include <linux/kernel.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mfd/wd-wt6703f.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define WD_WT6703F_CMD_LEN 7

/* Thermal status command */
static const u8 wd_wt6703f_thermal_get_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x03, 0x08, 0x00, 0x00, 0x00, 0xfb
};

/* ADC to temperature conversion table */
static const u8 wd_wt6703f_thermal_table[] = {
	0x74, 0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B,
	0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61,
	0x60, 0x5F, 0x5E, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57,
	0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D,
	0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x47, 0x46, 0x45, 0x44, 0x43,
	0x42, 0x41, 0x41, 0x40, 0x3F, 0x3E, 0x3E, 0x3D, 0x3D, 0x3C,
	0x3B, 0x3A, 0x3A, 0x39, 0x38, 0x38, 0x37, 0x36, 0x36, 0x35,
	0x34, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x30, 0x2F,
	0x2F, 0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2B, 0x2A, 0x2A,
	0x29, 0x29, 0x28, 0x28, 0x27, 0x27, 0x27, 0x26, 0x26, 0x25,
	0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x22, 0x21, 0x21, 0x21,
	0x20, 0x20, 0x1F, 0x1F, 0x1E, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C,
	0x1C, 0x1B, 0x1B, 0x1B, 0x1B, 0x1A, 0x19, 0x19, 0x19, 0x18,
	0x18, 0x17, 0x17, 0x25, 0x1B, 0x1B, 0x19, 0x19, 0x19, 0x18,
	0x18, 0x17, 0x17, 0x16, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14,
	0x14, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x10, 0x10,
	0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C,
	0x0C, 0x0B, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x09, 0x08, 0x08,
	0x07, 0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x04,
	0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

struct wd_wt6703f_thermal {
	struct wd_wt6703f *mcu;
	struct thermal_zone_device *tz;
	struct device *hwmon_dev;
	struct mutex lock;
	int last_temp;
};

static int wd_wt6703f_thermal_read_temp(struct wd_wt6703f_thermal *priv,
					int *temperature)
{
	u8 response[16];
	size_t response_len = sizeof(response);
	int ret;
	u8 raw_temp;

	if (!priv || !priv->mcu)
		return -ENODEV;

	mutex_lock(&priv->lock);

	ret = wd_wt6703f_exec(priv->mcu, wd_wt6703f_thermal_get_cmd,
			      WD_WT6703F_CMD_LEN, response, &response_len);
	if (ret) {
		mutex_unlock(&priv->lock);
		return ret;
	}

	if (response_len < 6) {
		mutex_unlock(&priv->lock);
		return -EIO;
	}

	/* The raw temperature value is the last payload byte. */
	raw_temp = response[5];

	if (raw_temp >= ARRAY_SIZE(wd_wt6703f_thermal_table)) {
		mutex_unlock(&priv->lock);
		return -ERANGE;
	}

	*temperature = (int)wd_wt6703f_thermal_table[raw_temp] * 1000;
	priv->last_temp = *temperature;
	mutex_unlock(&priv->lock);

	return 0;
}

static int wd_wt6703f_thermal_get_temp(struct thermal_zone_device *tz,
				       int *temperature)
{
	return wd_wt6703f_thermal_read_temp(thermal_zone_device_priv(tz),
					    temperature);
}

static const struct thermal_zone_device_ops wd_wt6703f_thermal_ops = {
	.get_temp = wd_wt6703f_thermal_get_temp,
};

static int wd_wt6703f_thermal_hwmon_read(struct device *dev,
					 enum hwmon_sensor_types type,
					 u32 attr, int channel, long *val)
{
	struct wd_wt6703f_thermal *priv = dev_get_drvdata(dev);
	int temperature;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input || channel != 0)
		return -EINVAL;

	ret = wd_wt6703f_thermal_read_temp(priv, &temperature);
	if (ret)
		return ret;

	*val = temperature;
	return 0;
}

static umode_t wd_wt6703f_thermal_hwmon_is_visible(
	const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input && channel == 0)
		return 0444;

	return 0;
}

static const struct hwmon_ops wd_wt6703f_thermal_hwmon_ops = {
	.read = wd_wt6703f_thermal_hwmon_read,
	.is_visible = wd_wt6703f_thermal_hwmon_is_visible,
};

static const struct hwmon_channel_info *wd_wt6703f_thermal_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL,
};

static const struct hwmon_chip_info wd_wt6703f_thermal_hwmon_chip_info = {
	.ops = &wd_wt6703f_thermal_hwmon_ops,
	.info = wd_wt6703f_thermal_hwmon_info,
};

static int wd_wt6703f_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wd_wt6703f *mcu = dev_get_drvdata(dev->parent);
	struct wd_wt6703f_thermal *priv;
	int ret;

	if (!mcu)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mcu = mcu;
	priv->last_temp = 0;

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	priv->hwmon_dev = devm_hwmon_device_register_with_info(
		dev, "wt6703f_thermal", priv,
		&wd_wt6703f_thermal_hwmon_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev))
		return PTR_ERR(priv->hwmon_dev);

	/* Register as thermal zone with device tree support when available. */
	priv->tz = devm_thermal_of_zone_register(dev, 0, priv,
						 &wd_wt6703f_thermal_ops);
	if (IS_ERR(priv->tz)) {
		dev_warn(dev, "Failed to register thermal zone: %ld\n",
			 PTR_ERR(priv->tz));
		priv->tz = NULL;
	} else {
		dev_info(dev, "WD WT6703F thermal zone registered\n");
	}

	dev_info(dev, "WD WT6703F hwmon temperature registered\n");
	return 0;
}

static const struct of_device_id wd_wt6703f_thermal_of_match[] = {
	{ .compatible = "wd,wt6703f-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, wd_wt6703f_thermal_of_match);

static struct platform_driver wd_wt6703f_thermal_driver = {
	.probe = wd_wt6703f_thermal_probe,
	.driver = {
		.name = "wd-wt6703f-thermal",
		.of_match_table = wd_wt6703f_thermal_of_match,
	},
};
module_platform_driver(wd_wt6703f_thermal_driver);

MODULE_DESCRIPTION("WD WT6703F Thermal driver");
MODULE_AUTHOR("OpenWrt community");
MODULE_LICENSE("GPL");
