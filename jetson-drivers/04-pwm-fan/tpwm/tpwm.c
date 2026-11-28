// SPDX-License-Identifier: GPL-2.0
/*
 * tpwm：排针 15 脚（PWM1）的 PWM 消费者驱动。
 * sysfs：period_ns、duty_pct（0~100）、enable、breathe（1 = 呼吸灯效果）
 * 绑定教程覆盖层里的 tutorial-pwm 节点：pwms = <&pwm1 0 1000000>;
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

#define BREATHE_STEP_MS	20

struct tpwm {
	struct pwm_device *pwm;
	struct mutex lock;		/* 保护 duty_pct、breathe 和对 PWM 的操作 */
	unsigned int duty_pct;
	bool breathe;
	int dir;
	struct delayed_work breathe_work;
};

/* 调用者持有 lock。按当前 period 和 duty_pct 更新硬件 */
static int tpwm_apply(struct tpwm *t, u64 period, bool enabled)
{
	struct pwm_state st;

	pwm_get_state(t->pwm, &st);
	st.period = period;
	st.duty_cycle = div_u64(period * t->duty_pct, 100);
	st.enabled = enabled;
	/* 可能睡眠（有的 PWM 控制器挂在 I2C 上），所以不能在中断里调用 */
	return pwm_apply_might_sleep(t->pwm, &st);
}

static void tpwm_breathe_fn(struct work_struct *work)
{
	struct tpwm *t = container_of(to_delayed_work(work), struct tpwm, breathe_work);
	struct pwm_state st;

	mutex_lock(&t->lock);
	if (!t->breathe) {
		mutex_unlock(&t->lock);
		return;
	}
	if (t->duty_pct == 0)
		t->dir = 1;
	else if (t->duty_pct >= 100)
		t->dir = -1;
	t->duty_pct += t->dir;
	pwm_get_state(t->pwm, &st);
	tpwm_apply(t, st.period, true);
	mutex_unlock(&t->lock);

	schedule_delayed_work(&t->breathe_work, msecs_to_jiffies(BREATHE_STEP_MS));
}

static ssize_t period_ns_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpwm *t = dev_get_drvdata(dev);
	struct pwm_state st;

	pwm_get_state(t->pwm, &st);
	return sysfs_emit(buf, "%llu\n", st.period);
}

static ssize_t period_ns_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct tpwm *t = dev_get_drvdata(dev);
	struct pwm_state st;
	u64 period;
	int ret;

	ret = kstrtou64(buf, 0, &period);
	if (ret)
		return ret;
	if (!period)
		return -EINVAL;

	mutex_lock(&t->lock);
	pwm_get_state(t->pwm, &st);
	ret = tpwm_apply(t, period, st.enabled);	/* 控制器做不到的周期，这里会返回错误 */
	mutex_unlock(&t->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(period_ns);

static ssize_t duty_pct_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpwm *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(t->duty_pct));
}

static ssize_t duty_pct_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tpwm *t = dev_get_drvdata(dev);
	struct pwm_state st;
	unsigned int pct;
	int ret;

	ret = kstrtouint(buf, 0, &pct);
	if (ret)
		return ret;
	if (pct > 100)
		return -EINVAL;

	mutex_lock(&t->lock);
	t->duty_pct = pct;
	pwm_get_state(t->pwm, &st);
	ret = tpwm_apply(t, st.period, st.enabled);
	mutex_unlock(&t->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(duty_pct);

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpwm *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", pwm_is_enabled(t->pwm));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct tpwm *t = dev_get_drvdata(dev);
	struct pwm_state st;
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&t->lock);
	pwm_get_state(t->pwm, &st);
	ret = tpwm_apply(t, st.period, on);
	mutex_unlock(&t->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enable);

static ssize_t breathe_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tpwm *t = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(t->breathe));
}

static ssize_t breathe_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct tpwm *t = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&t->lock);
	t->breathe = on;
	mutex_unlock(&t->lock);

	if (on)
		schedule_delayed_work(&t->breathe_work, 0);
	else
		cancel_delayed_work_sync(&t->breathe_work);
	return count;
}
static DEVICE_ATTR_RW(breathe);

static struct attribute *tpwm_attrs[] = {
	&dev_attr_period_ns.attr,
	&dev_attr_duty_pct.attr,
	&dev_attr_enable.attr,
	&dev_attr_breathe.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tpwm);

static void tpwm_shutdown_action(void *data)
{
	struct tpwm *t = data;

	t->breathe = false;
	cancel_delayed_work_sync(&t->breathe_work);
	pwm_disable(t->pwm);
}

static int tpwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_state st;
	struct tpwm *t;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	mutex_init(&t->lock);
	INIT_DELAYED_WORK(&t->breathe_work, tpwm_breathe_fn);
	t->dir = 1;

	/* 第二个参数是 pwm-names 里的名字；只有一路 PWM 时传 NULL */
	t->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(t->pwm))
		return dev_err_probe(dev, PTR_ERR(t->pwm), "get pwm\n");

	/* pwm_init_state：用设备树里写的周期（1000000 ns）初始化，占空比 0、关闭 */
	pwm_init_state(t->pwm, &st);
	st.duty_cycle = 0;
	st.enabled = false;
	ret = pwm_apply_might_sleep(t->pwm, &st);
	if (ret)
		return dev_err_probe(dev, ret, "initial apply\n");

	ret = devm_add_action_or_reset(dev, tpwm_shutdown_action, t);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, t);
	dev_info(dev, "pwm ready, period %llu ns\n", st.period);
	return 0;
}

static const struct of_device_id tpwm_of_match[] = {
	{ .compatible = "tutorial,pwm-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, tpwm_of_match);

static struct platform_driver tpwm_driver = {
	.probe = tpwm_probe,
	.driver = {
		.name = "tutorial-pwm",
		.of_match_table = tpwm_of_match,
		.dev_groups = tpwm_groups,
	},
};
module_platform_driver(tpwm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch04: header PWM consumer");
