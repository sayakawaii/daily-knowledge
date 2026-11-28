# 第 3 章：GPIO 驱动（LED、按键、中断、去抖）

> 目标：写一个平台驱动，控制一个 LED、读一个按键；按键用中断检测，软件去抖，用户程序用 `poll` 等按键。
> 不需要硬件：用内核的 GPIO 模拟器 gpio-sim 测试。有杜邦线以后再用真实引脚测一遍。
> 前提：第 2 章的覆盖层已装好。

## 一、和单片机的对比

| 单片机 | Linux |
|---|---|
| `GPIOC->ODR |= 1 << 13` 直接写寄存器 | 驱动调用 `gpiod_set_value(desc, 1)`，寄存器由 GPIO 控制器驱动（tegra186-gpio）去写 |
| 引脚号写死在代码里 | 引脚在设备树里：`led-gpios = <&gpio TEGRA234_MAIN_GPIO(Q, 5) GPIO_ACTIVE_HIGH>;` |
| `EXTI15_10_IRQHandler` 里处理按键 | `devm_request_irq()` 注册处理函数，中断号用 `gpiod_to_irq()` 从 GPIO 换算 |
| 中断里 `HAL_Delay(20)` 去抖（坏习惯，但能用） | **中断里绝对不能睡眠**，要把耗时的事推到工作队列或中断线程 |
| 低电平有效要自己记着取反 | 设备树里写 `GPIO_ACTIVE_LOW`，`gpiod_get_value()` 返回的就是逻辑值（1 = 按下） |

## 二、GPIO 子系统的两边

```
   消费者（consumer）                     提供者（provider）
  你的 LED/按键驱动                       GPIO 控制器驱动
  gpiod_get / gpiod_set_value     ──►     struct gpio_chip { .set, .get, .direction_output ... }
  gpiod_to_irq                             tegra186-gpio（真芯片）或 gpio-sim（模拟器）
           ▲
           │ GPIO 从哪来？
   设备树 led-gpios = <&gpio ...>   或   板级文件 gpiod_lookup_table
```

本章写的是消费者。消费者代码完全不关心 GPIO 是真芯片还是模拟器，所以**同一个驱动**在两种情况下都能跑，这就是能用模拟器测试的原因。

几个要点：

- **只用 `gpiod_*`**（描述符接口）。`gpio_request(617)` 这类整数接口已经废弃，网上老教程里很多。
- 用户态工具是 libgpiod 的 `gpiodetect`、`gpioinfo`、`gpioset`、`gpioget`、`gpiomon`。`/sys/class/gpio/export` 也已经废弃。
- `_cansleep` 版本：有的 GPIO 控制器挂在 I2C 上（比如 I2C 扩展芯片），读写要睡眠。在可以睡眠的地方一律用 `gpiod_get_value_cansleep()`，两种控制器都能用；gpio-sim 就是 `can_sleep = true`，用错会报警告。

## 三、先在用户态把引脚玩一遍

驱动没加载时，引脚可以直接用 libgpiod 工具操作。排针 29 脚是 `gpiochip0` 的第 105 根线（PQ.05），31 脚是第 106 根（PQ.06），完整对照表见总 README。

```bash
gpiodetect                                    # gpiochip0 [tegra234-gpio] (164 lines) ...
gpioinfo gpiochip0 | grep -E "PQ.05|PQ.06"
gpioset -m time -s 2 gpiochip0 105=1          # 29 脚输出高电平 2 秒
gpiomon -n 2 gpiochip0 106                    # 等 31 脚两个边沿（用杜邦线把它碰一下 GND）
sudo cat /sys/kernel/debug/gpio | grep -E "PQ.05|PQ.06"   # 谁占用了、方向、电平
```

> 本板实测：31 脚悬空时读到 0，开了内部上拉也一样。开发板载板上排针引脚很可能经过了电平转换（SoC 引脚是 1.8 V，排针是 3.3 V），内部上拉到不了排针那一侧（未核实）。**所以接按键一定要外接 10 kΩ 上拉电阻到 3.3 V（1 脚）。**

## 四、驱动结构

代码：[tgpio/tgpio.c](tgpio/tgpio.c)

```
probe
 ├─ devm_gpiod_get(dev, "led", GPIOD_OUT_LOW)     找 led-gpios，设成输出低电平
 ├─ devm_gpiod_get(dev, "button", GPIOD_IN)
 ├─ irq = gpiod_to_irq(button)
 ├─ devm_add_action_or_reset(取消去抖工作)
 └─ devm_request_irq(irq, tgpio_irq, 上升沿 | 下降沿)

按键电平变化
 └─ tgpio_irq（硬中断上下文）
       edges++；mod_delayed_work(去抖工作, debounce_ms)       ← 只做这两件事，马上返回
                       │  电平稳定 debounce_ms 之后
                       ▼
    tgpio_debounce_fn（工作队列，进程上下文，可以睡眠）
       读按键；和上次稳定值不同 → 更新；如果是按下：presses++、翻转 LED、sysfs_notify
```

### 去抖为什么这样写

`mod_delayed_work()` 的意思是「把这个工作安排在 N 毫秒后执行；如果已经安排了，就改成从现在起 N 毫秒后」。按键抖动时边沿一个接一个，工作不停被往后推；抖动结束、电平稳定 `debounce_ms` 后，工作只执行一次。测试里 10 次快速来回拉，产生 21 个中断，presses 只加 1。

这和单片机上「中断里启动一个定时器，定时器到了再读引脚」是同一个思路，只是 Linux 不允许在中断里等。

### 中断上下文的规矩

| 在哪 | 能睡眠吗 | 能用 |
|---|---|---|
| 硬中断（`request_irq` 的处理函数） | 不能 | spinlock、atomic、`mod_delayed_work`、`wake_up`、非 sleep 的 `gpiod_get_value` |
| 中断线程（`request_threaded_irq` 的 thread_fn） | 能 | 上面的都能用，再加 mutex、I2C/SPI 传输、`_cansleep` |
| 工作队列 | 能 | 同上 |
| 定时器回调（第 0 章） | 不能 | 同硬中断 |

另一种常见写法是 `devm_request_threaded_irq(dev, irq, NULL, thread_fn, IRQF_ONESHOT | ...)`：没有硬中断处理函数，内核自动建一个线程跑 thread_fn。挂在 I2C 上的传感器中断几乎都这么写（第 5 章）。

### 释放顺序

devm 资源按注册的**反序**释放。probe 里先注册「取消工作」，再注册中断，所以卸载时先 `free_irq`，再 `cancel_delayed_work_sync`。反过来就有问题：工作刚取消，又来一个中断，把工作重新排上，模块代码都卸载了它才执行，内核崩溃。

### sysfs 属性和 sysfs_notify

| 文件 | 读写 | 说明 |
|---|---|---|
| `led` | 读写 | 写 0/1 控制 LED |
| `button` | 只读 | 现读引脚（不经过去抖） |
| `presses` | 只读 | 去抖后的按下次数，变化时 `sysfs_notify`，可以 `poll` |
| `edges` | 只读 | 原始中断次数 |
| `debounce_ms` | 读写 | 1 到 1000 |

用户程序等按键的写法在 [tgpio/wait_press.c](tgpio/wait_press.c)，文件开头写了 sysfs `poll` 的三条规则。

## 五、没有设备树时：板级文件

[tgpio/tgpio_simboard.c](tgpio/tgpio_simboard.c) 是设备树出现之前描述硬件的方法：在 C 代码里写一张查找表「设备 tutorial-gpio-sim 的 led 是 tutorial-sim 芯片的第 0 根线」，再注册一个同名的平台设备。驱动里加了一个 `id_table` 按名字匹配它。

驱动 probe 时 `devm_gpiod_get(dev, "led", ...)` 先找设备树，找不到再查这张表，驱动代码不用改。在现在的内核里，这个方法主要用在：测试、x86 板子（没有设备树）、给 ACPI 打补丁。

## 六、GPIO 模拟器

本板内核没开 `CONFIG_GPIO_SIM`。[tools/gpio-sim/build.sh](../tools/gpio-sim/build.sh) 把内核源码里的 `gpio-sim.c` 和它依赖的 `irq_sim.c` 拿出来编译成外部模块（有一处小修改，脚本里写了原因）。第 9 章会讲怎么直接在内核配置里打开它。

```bash
cd tools/gpio-sim
./build.sh                 # 编译 gpio-sim-ext.ko
./sim.sh up                # 用 configfs 建一个 8 根线的模拟芯片，打印 CHIP= 和 SIM=
gpioinfo tutorial-sim      # 新芯片
echo pull-down | sudo tee $SIM/sim_gpio1/pull   # 「外部」把第 1 根线拉低，会产生中断边沿
cat $SIM/sim_gpio0/value                        # 读第 0 根线被驱动输出成了什么
./sim.sh down
```

## 七、测试

### 模拟模式（不需要硬件）

```bash
cd 03-gpio/tgpio
make                       # wait_press.c 没补完时会有一个 unused 警告，正常
./test.sh
```

要填的 TODO：

1. `test.sh` 里写 `press` 函数：低电平有效的按键，按下是拉高还是拉低？
2. `test.sh` 里加载两个模块（顺序有关系吗？）
3. `test.sh` 里通过驱动点亮 LED
4. `wait_press.c` 里的 `lseek` 和 `poll`

全部完成是 13 项 PASS。

### 真实引脚（需要杜邦线）

```bash
# 用一根母对母杜邦线连接排针 29 脚和 31 脚
./test_jumper.sh
```

驱动输出的电平直接被自己的按键输入读到，验证真实 GPIO 控制器的输出、输入、中断都正常。这个脚本没有 TODO。

### 接真 LED 和按键

```
排针 29 脚 ──[1 kΩ]──►|── 排针 6 脚 GND          LED（长脚接电阻那边）
排针 1 脚 3.3V ──[10 kΩ]──┬── 排针 31 脚
                          └──[按键]── 排针 9 脚 GND
```

加载：`sudo insmod tgpio.ko`，按按键 LED 翻转，`cat /sys/bus/platform/devices/tutorial-gpio/presses` 看次数，`edges` 看你的按键抖得多厉害。

> **安全**：排针是 3.3 V 电平，**任何引脚都不能接 5 V**（2、4 脚是 5 V 电源，别碰错）。排针引脚驱动能力很弱，LED 串 1 kΩ 电阻，电流约 1 mA，亮度够看；要驱动更大的负载用三极管或 MOS 管。

## 八、进阶练习

1. 把硬中断加工作队列改成 `devm_request_threaded_irq`，去抖用 `msleep` 加重读。想一想：线程化中断里 `msleep(20)` 期间又来了边沿会怎样？`IRQF_ONESHOT` 起什么作用？
2. 加一个 `long_press` 属性：按住超过 1 秒算一次长按。（提示：按下时再排一个 1 秒的 delayed_work，松开时取消。）
3. 不用 sysfs，改成第 1 章的 misc 字符设备：`read()` 阻塞到下一次按下，返回一个包含时间戳的结构体，支持 `poll`。这就是 `gpio-keys` 和输入子系统做的事，可以去读 `drivers/input/keyboard/gpio_keys.c`。
4. 在 `tutorial-gpio` 节点里加一个 `linux,code`，研究怎么改用内核现成的 `gpio-keys` 驱动，不用自己写。实际项目里，能用现成驱动就不要自己写。
