// SPDX-License-Identifier: GPL-2.0-only
/* WD WT6703F Fan Driver
 *
 * Provides hwmon interface for fan control on WD My Cloud devices.
 * Controls fan speed via PWM values (0-255, where 0 = off, 255 = full speed).
 * Integrates with thermal subsystem as a cooling device for automatic fan control.
 */

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/module.h>
#include <linux/mfd/wd-wt6703f.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define WD_WT6703F_CMD_LEN 7
#define WD_WT6703F_FAN_MAX 255

/* Fan control commands */
static const u8 wd_wt6703f_fan_get_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x02, 0x01, 0x00, 0x00, 0x00, 0xfb
};

static const u8 wd_wt6703f_fan_set_cmd[WD_WT6703F_CMD_LEN] = {
	0xfa, 0x02, 0x00, 0x00, 0x00, 0x00, 0xfb
};

struct wd_wt6703f_fan {
	struct device *dev;
	struct wd_wt6703f *mcu;
	struct device *hwmon_dev;
	struct thermal_cooling_device *cdev;
	struct mutex lock;
	u8 pwm_value;
};

static int wd_wt6703f_fan_set_speed(struct wd_wt6703f_fan *priv, u8 speed)
{
	u8 cmd[WD_WT6703F_CMD_LEN];
	int ret;

	memcpy(cmd, wd_wt6703f_fan_set_cmd, WD_WT6703F_CMD_LEN);
	cmd[3] = speed;

	mutex_lock(&priv->lock);
	/* Fan speed writes return only the MCU ACK frame. */
	ret = wd_wt6703f_exec(priv->mcu, cmd, WD_WT6703F_CMD_LEN, NULL, NULL);
	if (!ret)
		priv->pwm_value = speed;
	mutex_unlock(&priv->lock);

	return ret;
}

static int wd_wt6703f_fan_get_rpm(struct wd_wt6703f_fan *priv, int *rpm)
{
	u8 response[16];
	size_t response_len = sizeof(response);
	int ret;

	mutex_lock(&priv->lock);
	ret = wd_wt6703f_exec(priv->mcu, wd_wt6703f_fan_get_cmd,
			      WD_WT6703F_CMD_LEN, response, &response_len);
	if (!ret && response_len >= 6) {
		u8 period = response[5];

		/*
		 * The MCU reports tachometer period, not PWM. The daemon
		 * converts it using 300000 / period to obtain RPM.
		 */
		*rpm = period ? 300000 / period : 0;
	}
	mutex_unlock(&priv->lock);

	return ret;
}

/* Thermal cooling device operations */
static int wd_wt6703f_fan_get_max_state(struct thermal_cooling_device *cdev,
					 unsigned long *state)
{
	*state = WD_WT6703F_FAN_MAX;
	return 0;
}

static int wd_wt6703f_fan_get_cur_state(struct thermal_cooling_device *cdev,
					 unsigned long *state)
{
	struct wd_wt6703f_fan *priv = cdev->devdata;

	*state = priv->pwm_value;
	return 0;
}

static int wd_wt6703f_fan_set_cur_state(struct thermal_cooling_device *cdev,
					 unsigned long state)
{
	struct wd_wt6703f_fan *priv = cdev->devdata;

	if (state > WD_WT6703F_FAN_MAX)
		return -EINVAL;

	return wd_wt6703f_fan_set_speed(priv, (u8)state);
}

static const struct thermal_cooling_device_ops wd_wt6703f_fan_cooling_ops = {
	.get_max_state = wd_wt6703f_fan_get_max_state,
	.get_cur_state = wd_wt6703f_fan_get_cur_state,
	.set_cur_state = wd_wt6703f_fan_set_cur_state,
};

/* Sysfs attributes */
static ssize_t pwm1_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct wd_wt6703f_fan *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->pwm_value);
}

static ssize_t pwm1_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct wd_wt6703f_fan *priv = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 255)
		return -EINVAL;

	ret = wd_wt6703f_fan_set_speed(priv, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t fan1_input_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct wd_wt6703f_fan *priv = dev_get_drvdata(dev);
	int rpm;
	int ret;

	ret = wd_wt6703f_fan_get_rpm(priv, &rpm);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", rpm);
}

static DEVICE_ATTR_RW(pwm1);
static DEVICE_ATTR_RO(fan1_input);

static struct attribute *wd_wt6703f_fan_attrs[] = {
	&dev_attr_pwm1.attr,
	&dev_attr_fan1_input.attr,
	NULL,
};

ATTRIBUTE_GROUPS(wd_wt6703f_fan);

static int wd_wt6703f_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wd_wt6703f *mcu = dev_get_drvdata(dev->parent);
	struct wd_wt6703f_fan *priv;
	int ret;

	dev_info(dev, "WD WT6703F fan probing\n");
	
	if (!mcu)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->mcu = mcu;
	priv->pwm_value = 0;

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	priv->hwmon_dev = devm_hwmon_device_register_with_groups(dev, "wt6703f_fan",
							priv, wd_wt6703f_fan_groups);
	if (IS_ERR(priv->hwmon_dev))
		return PTR_ERR(priv->hwmon_dev);

	/* Register as thermal cooling device for automatic thermal control */
	priv->cdev = devm_thermal_of_cooling_device_register(dev, dev->of_node,
							     "wt6703f-fan",
							     priv,
							     &wd_wt6703f_fan_cooling_ops);
	if (IS_ERR(priv->cdev)) {
		ret = PTR_ERR(priv->cdev);
		dev_warn(dev, "Failed to register thermal cooling device: %d\n", ret);
		/* Continue anyway - sysfs control still works */
	} else {
		dev_info(dev, "WD WT6703F fan registered as cooling device\n");
	}

	return 0;
}

static const struct of_device_id wd_wt6703f_fan_of_match[] = {
	{ .compatible = "wd,wt6703f-fan" },
	{ }
};
MODULE_DEVICE_TABLE(of, wd_wt6703f_fan_of_match);

static struct platform_driver wd_wt6703f_fan_driver = {
	.probe = wd_wt6703f_fan_probe,
	.driver = {
		.name = "wd-wt6703f-fan",
		.of_match_table = wd_wt6703f_fan_of_match,
	},
};
module_platform_driver(wd_wt6703f_fan_driver);

MODULE_DESCRIPTION("WD WT6703F Fan driver");
MODULE_AUTHOR("OpenWrt community");
MODULE_LICENSE("GPL");
