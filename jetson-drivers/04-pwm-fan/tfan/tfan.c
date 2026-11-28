// SPDX-License-Identifier: GPL-2.0
/*
 * tfan：板载风扇的 hwmon 驱动，用来替换内核自带的 pwm-fan。
 *   - 风扇转速控制：PWM3（和 pwm-fan 用的是同一路，所以加载前要先卸载 pwm_fan）
 *   - 转速测量：转速计（tachometer）控制器，通过 PWM 子系统的 capture 接口读一圈的时间
 *   - 温度保护：每秒读一次 CPU 温度，超过 max_temp_mc 就强制全速
 *
 * hwmon 属性（/sys/class/hwmon/hwmonN/）：
 *   name        "tutorial_fan"
 *   pwm1        0~255，读写
 *   fan1_input  转速（RPM），只读
 *   temp1_input CPU 温度（毫摄氏度），只读
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/hwmon.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

static int max_temp_mc = 75000;
module_param(max_temp_mc, int, 0644);
MODULE_PARM_DESC(max_temp_mc, "超过这个温度（毫摄氏度）强制全速");

#define TFAN_SAFE_PWM	128	/* 加载时的初始转速，约一半 */

struct tfan {
	struct device *dev;
	struct pwm_device *fan;
	struct pwm_device *tach;
	struct thermal_zone_device *tz;
	struct mutex lock;
	u8 pwm;			/* 用户设置的值 */
	bool forced;		/* 温度保护是否接管了 */
	struct delayed_work guard_work;
};

/* 调用者持有 lock */
static int tfan_set_hw(struct tfan *f, u8 val)
{
	struct pwm_state st;

	pwm_get_state(f->fan, &st);
	st.duty_cycle = DIV_ROUND_UP_ULL(st.period * val, 255);
	st.enabled = true;
	return pwm_apply_might_sleep(f->fan, &st);
}

static int tfan_read_rpm(struct tfan *f, long *rpm)
{
	struct pwm_capture cap;
	int ret;

	/* 转速计驱动返回的 period 是风扇转一圈的时间（纳秒） */
	ret = pwm_capture(f->tach, &cap, 0);
	if (ret)
		return ret;
	*rpm = cap.period ? div64_u64(60ULL * NSEC_PER_SEC, cap.period) : 0;
	return 0;
}

static void tfan_guard_fn(struct work_struct *work)
{
	struct tfan *f = container_of(to_delayed_work(work), struct tfan, guard_work);
	int temp;

	if (!thermal_zone_get_temp(f->tz, &temp)) {
		mutex_lock(&f->lock);
		if (temp >= READ_ONCE(max_temp_mc) && !f->forced) {
			f->forced = true;
			tfan_set_hw(f, 255);
			dev_warn(f->dev, "temperature %d mC >= %d, fan forced to full speed\n",
				 temp, max_temp_mc);
		} else if (temp < READ_ONCE(max_temp_mc) - 5000 && f->forced) {
			/* 回差 5 度，避免在临界点来回切换 */
			f->forced = false;
			tfan_set_hw(f, f->pwm);
			dev_info(f->dev, "temperature back to %d mC, fan pwm %u\n", temp, f->pwm);
		}
		mutex_unlock(&f->lock);
	}
	schedule_delayed_work(&f->guard_work, HZ);
}

static umode_t tfan_is_visible(const void *data, enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	switch (type) {
	case hwmon_pwm:
		return attr == hwmon_pwm_input ? 0644 : 0;
	case hwmon_fan:
		return attr == hwmon_fan_input ? 0444 : 0;
	case hwmon_temp:
		return attr == hwmon_temp_input ? 0444 : 0;
	default:
		return 0;
	}
}

static int tfan_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *val)
{
	struct tfan *f = dev_get_drvdata(dev);
	int temp, ret;

	switch (type) {
	case hwmon_pwm:
		mutex_lock(&f->lock);
		*val = f->forced ? 255 : f->pwm;
		mutex_unlock(&f->lock);
		return 0;
	case hwmon_fan:
		return tfan_read_rpm(f, val);
	case hwmon_temp:
		ret = thermal_zone_get_temp(f->tz, &temp);
		*val = temp;
		return ret;
	default:
		return -EOPNOTSUPP;
	}
}

static int tfan_write(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long val)
{
	struct tfan *f = dev_get_drvdata(dev);
	int ret = 0;

	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EOPNOTSUPP;
	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&f->lock);
	f->pwm = val;
	if (!f->forced)		/* 温度保护期间只记下来，降温后再生效 */
		ret = tfan_set_hw(f, val);
	mutex_unlock(&f->lock);
	return ret;
}

static const struct hwmon_ops tfan_hwmon_ops = {
	.is_visible = tfan_is_visible,
	.read = tfan_read,
	.write = tfan_write,
};

static const struct hwmon_channel_info * const tfan_info[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_chip_info tfan_chip_info = {
	.ops = &tfan_hwmon_ops,
	.info = tfan_info,
};

/* 卸载时风扇保持全速：宁可吵，不能让芯片没人管 */
static void tfan_cleanup(void *data)
{
	struct tfan *f = data;

	cancel_delayed_work_sync(&f->guard_work);
	mutex_lock(&f->lock);
	tfan_set_hw(f, 255);
	mutex_unlock(&f->lock);
}

static int tfan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct pwm_state st;
	struct tfan *f;
	int ret;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = dev;
	mutex_init(&f->lock);
	INIT_DELAYED_WORK(&f->guard_work, tfan_guard_fn);

	/* pwm-names = "fan", "tach"; pwm_fan 还没卸载时这里会返回 -EBUSY */
	f->fan = devm_pwm_get(dev, "fan");
	if (IS_ERR(f->fan))
		return dev_err_probe(dev, PTR_ERR(f->fan), "get fan pwm (is pwm_fan unloaded?)\n");
	f->tach = devm_pwm_get(dev, "tach");
	if (IS_ERR(f->tach))
		return dev_err_probe(dev, PTR_ERR(f->tach), "get tach\n");

	f->tz = thermal_zone_get_zone_by_name("cpu-thermal");
	if (IS_ERR(f->tz))
		return dev_err_probe(dev, PTR_ERR(f->tz), "find cpu-thermal zone\n");

	pwm_init_state(f->fan, &st);
	ret = pwm_apply_might_sleep(f->fan, &st);
	if (ret)
		return ret;

	mutex_lock(&f->lock);
	f->pwm = TFAN_SAFE_PWM;
	ret = tfan_set_hw(f, f->pwm);
	mutex_unlock(&f->lock);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, tfan_cleanup, f);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, f);
	hwmon = devm_hwmon_device_register_with_info(dev, "tutorial_fan", f,
						     &tfan_chip_info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	schedule_delayed_work(&f->guard_work, 0);
	dev_info(dev, "fan ready, period %llu ns, pwm %u, guard %d mC\n",
		 st.period, f->pwm, max_temp_mc);
	return 0;
}

static const struct of_device_id tfan_of_match[] = {
	{ .compatible = "tutorial,fan-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfan_of_match);

static struct platform_driver tfan_driver = {
	.probe = tfan_probe,
	.driver = {
		.name = "tutorial-fan",
		.of_match_table = tfan_of_match,
	},
};
module_platform_driver(tfan_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch04: fan hwmon driver (pwm + tachometer + thermal guard)");
