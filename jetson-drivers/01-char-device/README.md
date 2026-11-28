# 第 1 章：字符设备

> 目标：写一个 `/dev/tutorial_fifo`，应用程序能 `open` `read` `write` `ioctl` `poll` 它。
> 这是所有驱动的基本功：后面每一章，用户和驱动打交道的方式都从这里来。
> 本章不需要任何外接硬件。

## 一、字符设备是什么

Linux 把设备分成三类：

| 类型 | 特点 | 例子 |
|---|---|---|
| 字符设备 | 按字节流读写，不能随机访问（或者不常用） | 串口 `/dev/ttyTHS1`、I2C `/dev/i2c-7`、GPIO `/dev/gpiochip0` |
| 块设备 | 按固定大小的块随机读写，有缓存 | NVMe 硬盘 `/dev/nvme0n1` |
| 网络设备 | 不在 `/dev` 下，用 socket 访问 | `wlP1p1s0`、`can0` |

```bash
ls -l /dev/ttyTHS1 /dev/i2c-7 /dev/nvme0n1
# crw-rw---- 1 root dialout 236, 1 ... /dev/ttyTHS1     ← 开头 c：字符设备；236 是主设备号，1 是次设备号
# crw-rw---- 1 root i2c      89, 7 ... /dev/i2c-7
# brw-rw---- 1 root disk    259, 0 ... /dev/nvme0n1     ← 开头 b：块设备
```

**主设备号**表示「归哪个驱动管」，**次设备号**表示「这个驱动管的第几个设备」。应用程序 `open("/dev/ttyTHS1")` 时，内核根据主设备号找到串口驱动，再把次设备号交给它。

## 二、一次 read() 是怎么走到驱动里的

```
应用程序                   内核
read(fd, buf, 100)
   │  系统调用
   ▼
 sys_read() ──► VFS：根据 fd 找到 struct file
                    file->f_op 指向驱动注册的 file_operations
                         │
                         ▼
                 fifo_read(file, buf, 100, &pos)   ← 你写的函数
                    │  copy_to_user(buf, 内核数据, n)
                    ▼
                 return n   ──► 应用程序拿到返回值 n
```

驱动做的事情就是填好一张函数表 `struct file_operations`：

```c
static const struct file_operations fifo_fops = {
	.owner          = THIS_MODULE,
	.read           = fifo_read,     // 应用程序 read() 时调用
	.write          = fifo_write,    // write()
	.poll           = fifo_poll,     // poll() / select() / epoll
	.unlocked_ioctl = fifo_ioctl,    // ioctl()
};
```

没填的项，内核会返回默认的错误。

## 三、注册设备：为什么用 misc

注册字符设备的「完整流程」有好几步：申请设备号（`alloc_chrdev_region`）、初始化 `cdev`、创建设备类（`class_create`）、创建设备节点（`device_create`）。**misc 设备**是内核提供的简化版：主设备号固定是 10，次设备号自动分配，`/dev` 下的节点也自动创建。只需要一个结构体：

```c
static struct miscdevice fifo_misc = {
	.minor = MISC_DYNAMIC_MINOR,   // 自动分配次设备号
	.name  = "tutorial_fifo",      // 会出现 /dev/tutorial_fifo
	.fops  = &fifo_fops,
	.mode  = 0666,                 // 设备节点权限，所有用户可读写（只是为了教程方便）
};
misc_register(&fifo_misc);
```

只有一个设备、不需要很多次设备号的驱动，用 misc 就够了。实际项目里很常见。

## 四、内核和用户之间拷贝数据

用户程序传进来的 `buf` 是**用户空间的虚拟地址**。内核不能直接解引用它：
- 地址可能是非法的（用户程序传错了，或者故意传一个内核地址来攻击）；
- 这块内存可能被换出到磁盘上，访问时要处理缺页。

所以必须用专门的函数：

| 函数 | 方向 | 用在 |
|---|---|---|
| `copy_to_user(用户地址, 内核地址, 长度)` | 内核 → 用户 | `read`、`ioctl` 返回结构体 |
| `copy_from_user(内核地址, 用户地址, 长度)` | 用户 → 内核 | `write`、`ioctl` 传入结构体 |
| `put_user(值, 用户地址)` / `get_user(...)` | 单个整数 | `ioctl` 返回一个数 |

它们返回「没拷贝成功的字节数」，不为 0 就要返回 `-EFAULT`。本章用的 `kfifo_to_user()` 和 `kfifo_from_user()` 内部就是调用这两个函数。

用户程序里的 `__user` 标记（比如 `char __user *buf`）只是给静态检查工具看的，提醒你「这是用户地址，不能直接用」。

## 五、阻塞和非阻塞

缓冲区空的时候，`read()` 应该怎么办？

- **阻塞模式**（默认）：让调用者**睡眠**，等有数据了再唤醒。睡眠期间 CPU 去跑别的进程，不浪费。
- **非阻塞模式**（`open` 时加 `O_NONBLOCK`）：立刻返回 `-EAGAIN`，告诉调用者「现在没有，你等会儿再来」。

睡眠靠**等待队列**（wait queue）实现，类似 RTOS 里的信号量等待：

```c
static DECLARE_WAIT_QUEUE_HEAD(read_wq);

// 读者：条件不满足就睡在 read_wq 上
wait_event_interruptible(read_wq, !kfifo_is_empty(&fifo));

// 写者：写完数据，唤醒睡在 read_wq 上的所有读者
wake_up_interruptible(&read_wq);
```

`_interruptible` 表示睡眠可以被信号打断：用户按 `Ctrl+C` 时，`wait_event_interruptible` 返回非 0，驱动返回 `-ERESTARTSYS`，进程就能正常退出。如果用不可中断的版本，进程会卡在 `D` 状态，`kill -9` 都杀不掉。

### 一个关键细节：睡眠前要先放锁

```c
while (kfifo_is_empty(&fifo)) {
	mutex_unlock(&fifo_lock);        // ① 先放锁
	wait_event_interruptible(...);   // ② 再睡
	mutex_lock(&fifo_lock);          // ③ 醒来重新拿锁，再检查一遍
}
```

如果拿着锁睡觉，写者永远拿不到锁，也就永远不会写入数据、唤醒读者：**死锁**。醒来之后要用 `while` 再检查一遍，因为可能有好几个读者同时被唤醒，数据已经被别人读走了。

## 六、并发：为什么要加互斥锁

这颗 SoC 有 6 个 CPU 核。两个进程可能在两个核上**同时**进入 `fifo_write()`，同时改 kfifo 的读写指针，数据就乱了。所以访问 kfifo 前要拿 `fifo_lock`：

```c
static DEFINE_MUTEX(fifo_lock);
mutex_lock_interruptible(&fifo_lock);   // 拿不到锁就睡眠等待，可以被 Ctrl+C 打断
... 访问共享数据 ...
mutex_unlock(&fifo_lock);
```

内核里常用的两种锁：

| 锁 | 拿不到时 | 能用在哪 |
|---|---|---|
| `mutex`（互斥锁） | 睡眠 | 只能在进程上下文（`read`、`write`、`probe`） |
| `spinlock`（自旋锁） | 原地空转 | 哪里都能用，包括中断处理函数；但拿着它的时候不能睡眠 |

本章全是进程上下文，用 mutex。第 3 章的中断处理函数里要用 spinlock。

## 七、poll：同时等多个设备

一个程序要同时等串口、网络、按键，不能对每个都阻塞 `read`。`poll()` 让它一次等多个文件描述符，哪个有数据就返回哪个。

驱动的 `poll` 函数**不睡眠**，只做两件事：
1. 用 `poll_wait()` 告诉内核「状态变化时，我会唤醒这个等待队列」；
2. 返回**当前**的状态：可读就带 `EPOLLIN`，可写就带 `EPOLLOUT`。

真正的睡眠由内核的 `poll` 系统调用实现。这样驱动只写一份代码，就同时支持了 `poll`、`select`、`epoll` 三种接口。

## 八、ioctl：传「命令」而不是「数据」

`read` 和 `write` 传的是数据流。要让设备做一件事（清空、查状态、改配置），用 `ioctl`。命令号用宏生成，把「方向、大小、类型、编号」编码进一个 32 位数：

```c
#define FIFO_IOC_MAGIC 'F'
#define FIFO_IOC_CLEAR     _IO(FIFO_IOC_MAGIC, 0)                       // 不传数据
#define FIFO_IOC_GET_LEN   _IOR(FIFO_IOC_MAGIC, 1, __u32)               // 内核 → 用户，4 字节
#define FIFO_IOC_GET_STATS _IOR(FIFO_IOC_MAGIC, 2, struct fifo_stats)   // 内核 → 用户，一个结构体
```

这些定义放在一个**驱动和用户程序共用的头文件** [`fifo_ioctl.h`](fifo/fifo_ioctl.h) 里。结构体里用 `__u32` `__u64` 这种固定宽度的类型，保证 32 位和 64 位程序看到的布局一样。不认识的命令返回 `-ENOTTY`，这是约定。

## 九、动手

```bash
cd ~/jetson-drivers/01-char-device/fifo
make                      # 同时编译驱动 fifo.ko 和测试程序 test_fifo
sudo insmod fifo.ko
ls -l /dev/tutorial_fifo  # crw-rw-rw- 1 root root 10, 261 ...   主设备号 10 就是 misc

echo hello > /dev/tutorial_fifo
cat /dev/tutorial_fifo     # 打印 hello，然后阻塞住，因为缓冲区空了；按 Ctrl+C 退出
```

`cat` 卡住不动是正常的：它在等更多数据，这正是阻塞读。另开一个终端 `echo world > /dev/tutorial_fifo`，第一个终端就会打印出 world。

### 测试

[`test_fifo.c`](fifo/test_fifo.c) 里有 6 个 TODO，都是用户程序那一侧的系统调用。

```bash
make && ./test_fifo
```

我的答案的输出：

```
[PASS] write 写入 5 个字节
[PASS] FIFO_IOC_GET_LEN 返回 5
[PASS] read 读回 "hello"
[PASS] 非阻塞读空缓冲区：返回 -1 且 errno == EAGAIN
[PASS] 阻塞读等到了子进程写入的 "wake"
       （read 睡了 200 ms）
[PASS] 阻塞读确实睡了大约 200 ms（150~400）
[PASS] poll：空时超时返回 0，有数据后返回 1 且带 POLLIN
[PASS] 写满：第一次只写进 4096 字节，第二次返回 EAGAIN
       （stats: written=4106 read=9 len=4096 capacity=4096）
[PASS] FIFO_IOC_GET_STATS：len 4096，capacity 4096
```

还没写 TODO 时，「阻塞读」那两项本来就会通过，因为那部分代码是写好的，用来演示。

做完卸载：`sudo rmmod fifo`。

## 十、进阶练习（可选）

1. 去掉 `fifo_read()` 里的 `mutex_unlock()`（睡眠前不放锁），重新跑测试，看第 4 项会怎样。用 `cat /proc/<pid>/stack` 看卡住的进程停在哪。
2. 增加一个 `FIFO_IOC_SET_SIZE` 命令，运行中改变容量。提示：要换成动态分配的 `kfifo_alloc()`。
3. 把 misc 设备改成完整的 `cdev` + `class_create` + `device_create` 写法，注册 4 个次设备号，每个是独立的 FIFO。

## 本章要记住的

1. 驱动通过 `file_operations` 把自己挂到 `/dev` 下的文件上，应用程序的系统调用最终调用到这些函数。
2. 用户地址不能直接用，要 `copy_to_user` / `copy_from_user`。
3. 睡眠用等待队列；睡之前一定要先放锁；用 `_interruptible` 版本，让 `Ctrl+C` 能打断。
