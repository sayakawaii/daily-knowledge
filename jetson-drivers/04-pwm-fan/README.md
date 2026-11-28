# 第 4 章：PWM 与风扇（PWM 消费者、hwmon、转速计、温度保护）

> 目标：
> 1. 写一个 PWM 驱动控制排针 15 脚，支持改周期、改占空比、呼吸灯；
> 2. 写一个 hwmon 驱动接管板载风扇：用 PWM 调速，用转速计读转速，温度过高自动全速。
>
> 不需要硬件：直接读 PWM 控制器的寄存器确认硬件被配置了；风扇本身就是现成的硬件。
> 前提：第 2 章的覆盖层已装好。

## 一、PWM 子系统

```
  消费者                                       提供者
  tpwm / tfan / 内核的 pwm-fan、背光驱动        pwm-tegra（PWM1/3/5/7 控制器）
  devm_pwm_get()                                pwm-tegra-tachometer（转速计，NVIDIA 驱动）
  pwm_apply_might_sleep(pwm, &state)  ──►      struct pwm_ops { .apply, .get_state, .capture }
  pwm_capture(pwm, &result, timeout)
```

和单片机最大的不同：**不直接写「分频值」「比较值」，而是给出想要的结果**，控制器驱动去算寄存器值：

```c
struct pwm_state st;
pwm_get_state(pwm, &st);          // 先拿当前状态
st.period = 1000000;              // 周期 1 ms（单位都是纳秒）
st.duty_cycle = 250000;           // 高电平 0.25 ms
st.enabled = true;
pwm_apply_might_sleep(pwm, &st);  // 一次性生效；硬件做不到会返回 -EINVAL
```

一次提交整个状态（而不是分别设周期、设占空比）是为了避免中间出现不合法的组合，比如新周期比旧占空比还短。

设备树里 `pwms = <&pwm1 0 1000000>`：第 0 路，默认周期 1 ms。`pwm_init_state()` 用这个默认周期初始化 state。

用户态也能直接操作（不写驱动时）：

```bash
cd /sys/class/pwm/pwmchip0          # 0 = 3280000.pwm（PWM1，15 脚）
echo 0 | sudo tee export
echo 1000000 | sudo tee pwm0/period
echo 250000 | sudo tee pwm0/duty_cycle
echo 1 | sudo tee pwm0/enable
sudo cat /sys/kernel/debug/pwm      # 所有 PWM 的状态、谁在用
echo 0 | sudo tee pwm0/enable; echo 0 | sudo tee unexport
```

本板的 PWM 控制器：

| 控制器 | sysfs | 用途 |
|---|---|---|
| 3280000.pwm（PWM1） | pwmchip0 | 排针 15 脚，本章 tpwm |
| 32a0000.pwm（PWM3） | pwmchip1 | 板载风扇，被 pwm-fan 占用 |
| 32c0000.pwm（PWM5） | pwmchip2 | 排针 33 脚 |
| 32e0000.pwm（PWM7） | pwmchip3 | 排针 32 脚 |
| 39c0000.tachometer | pwmchip4 | 风扇转速计（只能 capture，不能输出） |

## 二、Tegra PWM 硬件：能做到什么频率

寄存器只有一个（`PWM_CSR_0`）：

```
 31      30..25   24........16   15..13   12..........0
 使能     保留     占空比          保留      分频值
                   0~256（256 = 100%）
```

控制器时钟 408 MHz，一个 PWM 周期固定分成 256 份，分频器 13 位，所以：

- 最短周期 ≈ 256 ÷ 408 MHz ≈ 628 ns（约 1.6 MHz）
- 最长周期 ≈ 256 × 8192 ÷ 408 MHz ≈ 5.1 ms（约 195 Hz）
- 占空比只有 256 级

实测：1 µs 到 5 ms 都能设，10 ms、20 ms、500 ns 返回 `-EINVAL`。**舵机要的 50 Hz（20 ms）这几个 PWM 做不到**，需要舵机的话用 I2C 接口的 PCA9685 16 路 PWM 模块（十几块钱，第 5 章的 I2C 练习正好可以给它写驱动）。

### 读寄存器验证（以及一个坑）

[tools/regpeek](../tools/regpeek/regpeek.c) 通过 `/dev/mem` 读物理地址：

```bash
make -C tools/regpeek
sudo tools/regpeek/regpeek 0x32a0000      # 风扇 PWM：0x80400047 = 使能，占空比 64/256，分频 0x47
```

> **踩过的坑**：读一个**时钟没打开**的外设（比如没人用的 PWM 控制器），总线会超时，dmesg 里打出一大段 `CBB ... TIMEOUT_ERR`，读到 `0xffffffff`。更麻烦的是，总线桥随后会把这个外设**封掉**（日志里的 `AXI2APB_x_BLOCK_TMO_STATUS`），之后连内核驱动访问它也超时，要重启才恢复。
>
> 我写这一章时就这样把 PWM1 和 PWM5 弄「坏」过一次，还以为是硬件问题。所以测试脚本里先确认 `enable` 是 1 才去读寄存器。
>
> 单片机上读未使能时钟的外设通常只是读到 0；在大 SoC 上，总线有超时和防火墙保护。驱动里的规矩也一样：**访问寄存器前确保时钟和电源已打开**（`pm_runtime_resume_and_get`），这就是很多驱动 probe 里一上来就开时钟的原因。

## 三、tpwm：排针 PWM 驱动

代码：[tpwm/tpwm.c](tpwm/tpwm.c)

| sysfs 属性 | 说明 |
|---|---|
| `period_ns` | 周期，写硬件做不到的值返回错误 |
| `duty_pct` | 0~100，周期变了自动按比例重算 |
| `enable` | 0/1 |
| `breathe` | 1 = 每 20 ms 占空比加减 1，接 LED 就是呼吸灯 |

要点：

- 所有操作 PWM 的地方都持有同一个 mutex：`pwm_apply_might_sleep` 本身是线程安全的，但「读当前状态 → 改一个字段 → 写回去」这三步不是。
- 呼吸灯用 `delayed_work` 而不是定时器：定时器回调在软中断里，不能调用可能睡眠的 `pwm_apply_might_sleep`。
- 卸载时（devm action）先停呼吸灯工作，再关 PWM。

### 测试

```bash
cd 04-pwm-fan/tpwm
make
./test.sh
```

3 处 TODO：通过 sysfs 设占空比并打开、用位运算从寄存器值里取出占空比字段、把周期改回来。全部完成是 12 项 PASS。

### 有硬件时

- 接 LED：15 脚 → 1 kΩ → LED → GND，`echo 1 | sudo tee /sys/bus/platform/devices/tutorial-pwm/breathe`。
- 自己量频率：用杜邦线把 15 脚接到 31 脚，**卸载 tgpio**（31 脚要空出来），设 1 kHz：
  ```bash
  timeout 1 gpiomon -r gpiochip0 106 | wc -l      # 1 秒内的上升沿数，应该接近 1000
  ```
  这是「用一个外设测另一个外设」：没有示波器时很实用。（这一步需要硬件，我没有实测过。）

## 四、hwmon 子系统

hwmon 是「硬件监控」：温度、电压、电流、风扇转速、风扇 PWM。它规定了**统一的文件名**，所以 `sensors` 命令、`nvfancontrol`、各种监控软件不用认识具体芯片：

| 文件 | 含义 | 单位 |
|---|---|---|
| `temp1_input` | 温度 | 毫摄氏度 |
| `fan1_input` | 风扇转速 | RPM |
| `pwm1` | 风扇 PWM | 0~255 |
| `in0_input` / `curr1_input` / `power1_input` | 电压 / 电流 / 功率 | mV / mA / µW |

本板现有的 hwmon：`pwmfan`（pwm-fan 驱动）、`pwm_tach`（转速计）、`ina3221`（电源监测，I2C 上的芯片）、`nvme`。

```bash
for h in /sys/class/hwmon/hwmon*; do echo "$h $(cat $h/name)"; ls $h | grep -E "input|pwm|rpm" | tr '\n' ' '; echo; done
```

写 hwmon 驱动用 `devm_hwmon_device_register_with_info()`：声明有哪些通道（`HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT)`），实现 `is_visible`、`read`、`write` 三个回调，文件由 hwmon 核心按标准名字生成。不要自己 `DEVICE_ATTR` 造文件名。

## 五、tfan：接管板载风扇

代码：[tfan/tfan.c](tfan/tfan.c)

```
probe
 ├─ devm_pwm_get(dev, "fan")       PWM3，pwm_fan 没卸载时返回 -EBUSY
 ├─ devm_pwm_get(dev, "tach")      转速计（覆盖层里给它加了标签才能引用）
 ├─ thermal_zone_get_zone_by_name("cpu-thermal")
 ├─ 风扇设成 128（约一半）
 ├─ devm_add_action(卸载时：停温度检查，风扇全速)
 ├─ devm_hwmon_device_register_with_info("tutorial_fan", ...)
 └─ 启动温度检查 delayed_work（每秒一次）

读 fan1_input → pwm_capture(tach) → 转一圈的时间 period（纳秒）→ RPM = 60×10⁹ / period
温度 ≥ max_temp_mc → 强制 255；降到 max_temp_mc - 5℃ 以下 → 恢复用户设置（5℃ 回差，防止来回跳）
```

**风扇驱动是安全相关的**，几个设计都是为这个：

- 卸载时风扇留在全速，而不是停转；
- 温度保护期间用户写的 pwm1 只记下来，降温后才生效；
- 测试脚本用 `trap restore EXIT`：不管测试成功、失败还是按了 Ctrl+C，都会恢复 `pwm_fan` 和 `nvfancontrol`。

本板实测：

| pwm1 | 转速 |
|---|---|
| 255 | 5166 RPM |
| 200 | 3973 RPM |
| 60 | 1249 RPM |
| 0 | 1240 RPM（风扇有最低转速，0 并不停转） |

### 测试

```bash
cd 04-pwm-fan/tfan
make
./test.sh
```

脚本会：停 `nvfancontrol` → 卸载 `pwm_fan` → 加载 `tfan` → 测试 → 恢复。风扇会全速转几秒，比较吵，正常。

4 处 TODO：按名字找到 hwmon 目录、写设置 PWM 的函数、从寄存器取占空比、通过模块参数触发温度保护。全部完成是 10 项 PASS。

> 如果测试中途断电或死机，`pwm_fan` 不会自动恢复吗？会的：它是开机自动加载的模块，重启就回来了。只有在测试脚本被 `kill -9` 这种情况下，才需要手动 `sudo modprobe pwm_fan && sudo systemctl start nvfancontrol`。

## 六、进阶练习

1. 给 tfan 加 `pwm1_enable`（hwmon 标准：1 = 手动，2 = 自动），自动模式下按温度曲线调速（比如 40℃ 以下 60，70℃ 以上 255，中间线性）。这就是 `nvfancontrol` 在用户态做的事。
2. 读 `drivers/hwmon/pwm-fan.c`（只有几百行），对比它和 tfan：它是怎么注册成 thermal 冷却设备（`cooling-levels`）、让内核的温控策略直接调它的？
3. 把 tfan 注册成 thermal cooling device（`devm_thermal_of_cooling_device_register`），不用自己写温度检查。
4. tpwm 加一个 `polarity` 属性（`PWM_POLARITY_INVERSED`），看 Tegra PWM 驱动支不支持，返回什么错误。
