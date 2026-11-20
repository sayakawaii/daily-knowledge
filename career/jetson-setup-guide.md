# Jetson Orin Nano Super 装机指南：从 JetPack 6.2 升级到 JetPack 7.2.1

写于 2026-11-18，版本信息按当天的英伟达官方文档核对。配合职业转型课第 2 课（TensorRT 部署）和第 3 课（ROS2 小车）使用。

> 2026-11-20 已按本指南装完。实际结果和本文有几处不同（自动升到 JetPack 7.2.2、TensorRT 变成 11.4 等），见文末「实际装机记录」。

**在自己家的网络或手机热点下操作**，不要用公司网络：要下载好几 GB 的东西，而且私人设备不该接公司内网。

## 你这块板子的情况（2026-11-18 检查）

| 项目 | 现状 |
| --- | --- |
| 系统 | R36.4.3 = JetPack 6.2，Ubuntu 22.04，CUDA 12.6，TensorRT 10.3（卖家预装） |
| 载板 | 官方 P3768-0000 |
| 模组 | P3767-0003，Orin Nano 8GB 量产模组，Super 配置。官方 Jetson Linux r39.2 文档列明支持装在 P3768 载板上 |
| 硬盘 | 256 GB NVMe，系统在上面 |
| 固件 | R36.x，满足 JetPack 7.2 要求的「JetPack 6 代固件」，可以直接用 U 盘装 |
| 登录 | 用户名、密码都是 `nvidia`（重装后会换成你自己设的） |

装完以后的目标版本：JetPack 7.2.1 / Jetson Linux r39.2.1，Ubuntu 24.04，内核 6.8，CUDA 13.2.2，cuDNN 9.20，TensorRT 10.16.2，Python 3.12。

**重装会清空整块硬盘**。卖家预装的 ultralytics、jetson-inference、ollama 都会被删掉，里面没有需要保留的东西。

---

## 第 0 步：准备（在 Windows 电脑上做）

1. 一个**空 U 盘**（写入时会被清空），直接插在板子上用，不要经过 USB 集线器。
2. 下载 **Jetson ISO r39.2.1**：
   - 下载页：<https://developer.nvidia.com/embedded/jetpack/downloads>，找 JetPack 7.2.1 下的「Jetson ISO (r39.2.1)」
   - 直链（可能要登录英伟达开发者账号）：<https://developer.nvidia.com/downloads/embedded/l4t/r39_release_v2.1/iso/jetsoninstaller-r39.2.1-2026-08-07-18-30-47-arm64.iso>
3. 下载并安装 **balenaEtcher**：<https://etcher.balena.io/#download-etcher>
4. 准备显示器（DP 转 HDMI 线）、USB 键盘鼠标、网线（有的话比 Wi-Fi 稳）。

官方安装说明（遇到和本指南不一致时以它为准）：<https://docs.nvidia.com/jetson/orin-nano-devkit/user-guide/latest/quick_start.html>

## 第 1 步：写 U 盘

打开 balenaEtcher → **Flash from file** 选 ISO → **Select target** 选 U 盘 → **Flash!**

不能把 ISO 文件直接拷进 U 盘，必须用 Etcher 写。

## 第 2 步：用 U 盘安装 JetPack 7.2.1

1. 板子断电。插上 U 盘、显示器、键盘鼠标、网线，**最后插电源**（通电自动开机）。
2. 屏幕出现 NVIDIA 标志时**按 Esc** → 用方向键选 **Boot Manager** → 回车 → 选你的 **U 盘** → 回车。
3. **出现 QSPI 固件更新（capsule update）提示时，30 秒内按 Y**。这是最容易错过的一步。
   - 更新分两轮，中间板子可能自己重启，正常，等它做完。
   - 如果错过了提示、安装继续往下走，后面会失败。那就断电重来，这次盯着按 Y。
4. 出现安装菜单（GRUB）时，选 **Install Jetson ISO r39.2.1**。
5. 选择安装目标：**NVMe 硬盘**（约 238 GB 那个），确认。这会清空它，看清楚再确认。
6. 屏幕上会滚几分钟白色文字。网上报告的总耗时从 15 分钟到更久不等。
7. 提示重启时，**先拔掉 U 盘**，再重启。不拔的话会又进安装程序。

## 第 3 步：首次开机设置

按向导走：
- 语言选 **English**（报错信息好搜）
- 时区选 **Shanghai**
- 用户名用小写英文（下文用 `ming` 举例，换成你自己的）
- **密码设强一点**，不要再用 `nvidia`

进桌面后按 `Ctrl+Alt+T` 打开终端，后面的命令都在这里输。

## 第 4 步：基础配置

### 4.1 确认时间正确

```bash
timedatectl
```

`System clock synchronized: yes` 且时间正确就行。时间不对，后面下载会报证书错误。如果不同步：

```bash
sudo timedatectl set-ntp true
```

### 4.2 （可选）换国内软件源

如果 `apt update` 很慢，可以把 Ubuntu 源换成清华的 ARM 镜像：

```bash
sudo cp /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources.bak
sudo sed -i 's|http://ports.ubuntu.com/ubuntu-ports|https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports|g' /etc/apt/sources.list.d/ubuntu.sources
```

先 `cat /etc/apt/sources.list.d/ubuntu.sources` 看一眼，确认里面原来写的是 `ports.ubuntu.com`。英伟达自己的源（`repo.download.nvidia.com`）不要改。

### 4.3 装 JetPack 组件（CUDA、cuDNN、TensorRT）

U 盘装的只是最小系统，**不含 CUDA 和 TensorRT**，要用 apt 装：

```bash
sudo apt update
sudo apt dist-upgrade -y
sudo reboot
```

重启后：

```bash
sudo apt update
sudo apt install -y nvidia-jetpack
```

官方说这一步视网速约一小时。不要装 Ubuntu 自带的 `nvidia-cuda-toolkit`，那不是给 Jetson 用的。

### 4.4 验证

```bash
cat /etc/nv_tegra_release            # 应显示 R39 (release), REVISION: 2.1
cat /proc/device-tree/compatible | tr '\0' '\n'; echo   # 应含 p3768-0000+p3767-0003-super
ls /usr/local/ | grep cuda            # 应看到 cuda-13.x
dpkg -l | grep -E "tensorrt |libnvinfer10" | awk '{print $2, $3}'   # 应是 10.16.x
python3 --version                      # 应是 3.12.x
```

把 CUDA 加到环境变量（目录名以上面 `ls` 的结果为准）：

```bash
echo 'export CUDA_HOME=/usr/local/cuda' >> ~/.bashrc
echo 'export PATH=$CUDA_HOME/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc
nvcc --version
```

### 4.5 功耗模式和监控工具

```bash
sudo nvpmodel -q          # 查看当前模式
sudo nvpmodel -m 2        # 切到 MAXN_SUPER（最快）。注意：Super 配置下 0 是 15W，1 是 25W，2 才是 MAXN_SUPER
sudo jetson_clocks        # 锁定最高频率（测性能前用；重启后失效）
```

切换模式时可能提示重启，按提示做。

监控工具 jtop：

```bash
sudo apt install -y python3-pip
sudo pip3 install -U jetson-stats --break-system-packages
sudo reboot
```

重启后运行 `jtop` 能看到 CPU、GPU、内存、温度、功耗。（Ubuntu 24.04 默认不让 pip 往系统 Python 里装东西，所以要加 `--break-system-packages`；jetson-stats 是系统服务，官方就是这么装的。）

## 第 5 步：SSH 和 Cursor 远程连接

### 5.1 板子上

```bash
sudo apt install -y openssh-server
hostname -I               # 记下 IP，比如 192.168.1.23
```

建议在路由器后台给板子做 DHCP 地址保留，IP 就不会变。

### 5.2 Windows 上（Git Bash）

重装后板子的主机密钥变了，先删掉旧记录，再把公钥放上去（替换用户名和 IP）：

```bash
ssh-keygen -R 10.242.28.15
ssh-copy-id -i ~/.ssh/id_rsa.pub ming@192.168.1.23
ssh ming@192.168.1.23 'echo ok'   # 不要密码就能输出 ok 就成了
```

### 5.3 Cursor 里

`Ctrl+Shift+P` → `Remote-SSH: Connect to Host` → 输入 `ming@192.168.1.23` → 打开文件夹 `~/projects`（先 `mkdir -p ~/projects`）。

连上后新开一个对话，告诉我「板子装好了」，我就能直接在板子上干活。Cursor 在板子上的后台服务会占几百 MB 内存，跑大模型时如果紧张可以先断开。

## 第 6 步：第 2 课要用的 Python 环境（PyTorch、Ultralytics）

JetPack 7.2 的坑：**直接 `pip install ultralytics` 装上的 PyTorch 不能用**（不是为 CUDA 13 + Orin 编的），要手动装对的版本。

### 6.1 建虚拟环境

```bash
sudo apt install -y python3.12-venv python3-dev build-essential cmake git libopenblas-dev
python3 -m venv ~/venvs/yolo --system-site-packages
source ~/venvs/yolo/bin/activate
pip install -U pip wheel setuptools
```

`--system-site-packages` 让虚拟环境能用 JetPack 自带的 TensorRT Python 接口。以后每次开终端先 `source ~/venvs/yolo/bin/activate`。

（可选）pip 慢的话换清华源：`pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple`。下面 PyTorch 那一步用的是 PyTorch 官方源，不受影响。

### 6.2 装 Ultralytics，再换成正确的 PyTorch

```bash
pip install ultralytics
pip uninstall -y torch torchvision
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu130 --extra-index-url https://pypi.org/simple
pip install nvidia-cudnn-cu13 nvidia-nccl-cu13 nvidia-cusparselt-cu13 nvidia-nvshmem-cu13
```

- 第 3 行是 Ultralytics 官方指南给 JetPack 7.2 的命令。
- 第 4 行是部分 JetPack 7.2 安装指南额外要求的四个库，缺了可能导致 torch 导入报错。装上无害。
- 如果 cu130 版本不行，英伟达论坛管理员给 Orin + JetPack 7.2 推荐的是 cu132：`pip install torch torchvision --index-url https://download.pytorch.org/whl/cu132`（必要时加 `--pre`）。
- 运行时出现「Found GPU ... compute capability 8.7」之类的警告可以忽略。

### 6.3 ONNX Runtime（GPU 版）

```bash
pip install https://github.com/ultralytics/assets/releases/download/v0.0.0/onnxruntime_gpu-1.24.0-cp312-cp312-linux_aarch64.whl
```

### 6.4 验证 GPU 真的能算

`torch.cuda.is_available()` 返回 True 不算数，要真跑一次运算：

```bash
python -c "import torch; print(torch.__version__, torch.version.cuda); x=torch.randn(1024,1024,device='cuda'); print((x@x).sum().item()); print('GPU OK')"
python -c "import tensorrt; print('TensorRT', tensorrt.__version__)"
```

如果报 `no kernel image is available for execution on the device`，说明装错了 PyTorch，回 6.2 重装。

### 6.5 跑通第 2 课第 1 周（需要联网下载模型和示例图）

```bash
mkdir -p ~/projects/yolo && cd ~/projects/yolo
yolo predict model=yolo11n.pt source='https://ultralytics.com/images/bus.jpg'
yolo export model=yolo11n.pt format=engine half=True
sudo nvpmodel -m 2 && sudo jetson_clocks
yolo benchmark model=yolo11n.pt data=coco8.yaml imgsz=640 half=True device=0
```

引擎**必须在这块板子上导出**（第 2 课讲过）。FP16 推理接近官方的约 4.5 ms 就算过关。第 3 周要用的 COCO 验证集（5000 张图，约 0.8 GB）也建议提前下载。**不要用 `yolo val data=coco.yaml` 自动下载**：它会连 19 GB 的训练集一起下载。只下载验证集的做法见文末「实际装机记录」。

## 第 7 步：ROS 2 Jazzy（第 3 课要用）

### 7.1 加软件源

官方源（推荐，和 ROS 官方文档一致）：

```bash
sudo apt install -y software-properties-common curl
sudo add-apt-repository -y universe
export ROS_APT_SOURCE_VERSION=$(curl -s https://api.github.com/repos/ros-infrastructure/ros-apt-source/releases/latest | grep -F "tag_name" | awk -F'"' '{print $4}')
curl -L -o /tmp/ros2-apt-source.deb "https://github.com/ros-infrastructure/ros-apt-source/releases/download/${ROS_APT_SOURCE_VERSION}/ros2-apt-source_${ROS_APT_SOURCE_VERSION}.$(. /etc/os-release && echo ${UBUNTU_CODENAME:-${VERSION_CODENAME}})_all.deb"
sudo dpkg -i /tmp/ros2-apt-source.deb
sudo apt update
```

如果访问 GitHub 失败，用清华镜像代替上面整段：

```bash
sudo apt install -y curl gnupg2
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] https://mirrors.tuna.tsinghua.edu.cn/ros2/ubuntu noble main" | sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null
sudo apt update
```

（两种只选一种，不要都加。）

### 7.2 装 ROS 2 和项目二要用的包

```bash
sudo apt install -y ros-jazzy-desktop ros-dev-tools
sudo apt install -y ros-jazzy-navigation2 ros-jazzy-nav2-bringup ros-jazzy-slam-toolbox \
  ros-jazzy-ros2-control ros-jazzy-ros2-controllers ros-jazzy-robot-localization \
  ros-jazzy-teleop-twist-keyboard ros-jazzy-vision-msgs ros-jazzy-rplidar-ros
echo "source /opt/ros/jazzy/setup.bash" >> ~/.bashrc
source ~/.bashrc
printenv ROS_DISTRO      # 应输出 jazzy
```

`ros-jazzy-desktop` 带 RViz 可视化，体积较大，正好在家里装。如果第二条命令提示某个包找不到（`Unable to locate package`），把那个包从命令里删掉再执行，回头告诉我。

### 7.3 rosdep

```bash
sudo rosdep init
rosdep update
```

`rosdep update` 要访问 GitHub，国内常失败。失败了不影响上面装好的包，以后用到再说。

### 7.4 验证

开两个终端：

```bash
ros2 run demo_nodes_cpp talker
```

```bash
ros2 run demo_nodes_cpp listener
```

第二个终端能收到 `I heard: [Hello World: N]` 就成了。

### 7.5 Gazebo 仿真

第 3 课第 1、2 周的 Gazebo Harmonic 仿真比较吃资源，**建议装在另一台 Ubuntu 24.04 电脑（或虚拟机）上**，不必装在 Jetson 上。在那台电脑上按 7.1 加源后：`sudo apt install -y ros-jazzy-desktop ros-jazzy-ros-gz ros-jazzy-gz-ros2-control`。

## 第 8 步（可选，第 3 课第 7 周再装）：Isaac ROS 4.6

Isaac ROS 4.6 是支持 Orin + JetPack 7.2 + Jazzy 的版本。**不要装 5.0**，那一版改成了 Lyrical。

```bash
k="/usr/share/keyrings/nvidia-isaac-ros.gpg"
curl -fsSL https://isaac.download.nvidia.cn/isaac-ros/repos.key | sudo gpg --dearmor | sudo tee -a $k > /dev/null
f="/etc/apt/sources.list.d/nvidia-isaac-ros.list"
sudo touch $f
s="deb [signed-by=$k] https://isaac.download.nvidia.cn/isaac-ros/release-4.6 noble-jetpack main"
grep -qxF "$s" $f || echo "$s" | sudo tee -a $f
sudo apt-get update
sudo apt-get install -y isaac-ros-cli
```

（`isaac.download.nvidia.cn` 是英伟达的国内 CDN；海外用 `.com`。`release-4.6` 锁定小版本，不会被自动升到 5.0。）

然后选一种运行方式，官方推荐 Docker：

```bash
docker info | grep -E "Runtimes|Default Runtime"
docker run --rm --gpus all ubuntu:24.04 bash -lc 'echo "NVIDIA runtime OK"'
sudo usermod -aG docker $USER && newgrp docker
sudo isaac-ros init docker
```

U 盘装的系统一般已带 Docker 和 NVIDIA Container Toolkit；如果 `docker` 命令不存在，按官方 Jetson Docker 设置文档先装。之后的具体用法以 Isaac ROS 4.6 文档为准：<https://nvidia-isaac-ros.github.io/v/release-4.6/getting_started/index.html>。Docker 镜像很大，务必在家里网络下拉取。

## 第 9 步：装完后发给我检查

在板子上运行，把输出贴给我：

```bash
cat /etc/nv_tegra_release
cat /proc/device-tree/compatible | tr '\0' '\n'; echo
sudo nvpmodel -q
nvcc --version | tail -1
dpkg -l | grep -E "tensorrt |nvidia-jetpack " | awk '{print $2, $3}'
source ~/venvs/yolo/bin/activate && python -c "import torch, tensorrt; print(torch.__version__, torch.cuda.is_available(), tensorrt.__version__)"
printenv ROS_DISTRO
df -h / | tail -1
free -h | head -2
hostname -I
```

## 实际装机记录（2026-11-20）

### 装出来的版本

| 组件 | 实际版本 | 和上文的差异 |
| --- | --- | --- |
| Jetson Linux | r39.2.2（2026-11-13 发布） | U 盘装的是 r39.2.1，`apt dist-upgrade` 自动升到了 r39.2.2 |
| JetPack | `nvidia-jetpack 7.2.2-b56` | 上文写的是 7.2.1 |
| CUDA / cuDNN | 13.2 / 9.20 | 一致 |
| TensorRT | **11.4.1**（包名 `libnvinfer11`） | 上文写的是 10.16.2。7.2.2 的 `nvidia-tensorrt` 依赖 11.4，7.2.1 依赖 10.16.2 |
| PyTorch | 2.15.0+cu132，GPU 矩阵运算通过 | 按 cu130 源装，装上的是 cu132 版本，不影响使用 |
| ONNX Runtime | 1.24.0，可用 TensorRT / CUDA 加速 | 一致 |
| Ultralytics | 8.4.216 | 一致 |
| ROS 2 | Jazzy 桌面版加 7.2 节的全部包，talker/listener 通过 | `rosdep update` 直连 GitHub 失败，配好代理后通过 |
| 硬盘占用 | 44 GB / 233 GB | |

`compatible` 显示的是 `p3767-0005-super`，和旧系统显示的 `0003` 不一样，内存仍是 8 GB，不影响使用。

### TensorRT 11 和第 2 课

Ultralytics 目前要求 TensorRT 低于 11（`tensorrt-cu13>=10.15,<11`）。所以 6.5 节的 `yolo export format=engine` 会自动用 pip 去装一个 TensorRT 10，**看到这种情况就按 Ctrl+C 停掉**，不要让它装。TensorRT 11 也去掉了 `trtexec --fp16` 参数（报 `Unknown option: --fp16`），精度改为由 ONNX 里的数据类型决定。

决定保留 11.4，第 2 课改用下面的流程，已在板子上验证：

```bash
source ~/venvs/yolo/bin/activate && cd ~/projects/yolo
yolo export model=yolo11n.pt format=onnx half=True device=0     # 导出 FP16 的 ONNX
trtexec --onnx=yolo11n.onnx --saveEngine=yolo11n_trt11_fp16.engine   # 不要加 --fp16
```

实测（25W 模式、没有运行 `jetson_clocks`）：FP16 引擎 GPU 计算时间中位 4.29 ms（约 230 次/秒）；FP32 引擎中位 7.70 ms。

如果以后想退回 TensorRT 10.16：英伟达源里还有 `nvidia-jetpack=7.2.1-b49` 和 `tensorrt=10.16.2.10-1+cuda13.2`，可以指定版本安装，再用 `apt-mark hold` 锁住。

### 装机过程踩到的坑

- **桌面右上角的 Wi-Fi 图标不见了**：网卡和驱动（`rtl8822ce`）都正常，只是没有连上。用命令连：`sudo nmcli con up "热点名" ifname wlP1p1s0`。已设成断开后无限重试：`sudo nmcli con mod "热点名" connection.autoconnect-retries 0`。
- **iPhone 热点扫不到**：iPhone 在一段时间没有新设备连接时会停止广播热点。打开「设置 → 个人热点」页面，并开启「最大兼容性」。
- **公司电脑会自动连回公司 Wi-Fi**：那是组策略推送的配置，改不了。改用 USB-C 线直连：线插在板子唯一的 USB-C 口上，电脑上会出现 `Remote NDIS` 网卡，地址是 `192.168.55.100`，然后 `ssh 用户名@192.168.55.1`。线一定要插紧，插不紧时会丢包，SSH 反复断开。
- **长时间的安装放到后台跑**：用 `sudo systemd-run --unit=名字 bash -c "脚本 > 日志 2>&1"`。SSH 断开也不影响，用 `tail 日志` 查看进度。
- **apt 实际读的是 `/etc/apt/sources.list`**：这次 U 盘装的系统里 `ubuntu.sources` 是空文件，换清华源要改 `/etc/apt/sources.list`。
- **访问 GitHub 要走代理**：板子上装了 mihomo（Clash Meta 内核），作为系统服务 `mihomo` 运行。配置在 `/etc/mihomo/config.yaml`（权限 600，含密钥，不要提交到仓库），只在本机监听 `127.0.0.1:7890`。VPS 只有 IPv6 地址，所以配置里要加 `ipv6: true`（mihomo 默认关闭）。地址库 `geoip.metadb` 要先在电脑上下载好再拷进去，并设 `geo-auto-update: false`，否则 mihomo 首次启动会去 GitHub 下载而卡住。规则是国内地址直连，其他走代理。
  - 命令行：`proxy_on` / `proxy_off`（写在 `~/.bashrc` 里）；git 访问 github.com 自动走代理。
  - 切换节点：`curl -X PUT -d '{"name":"VLESS-Reality"}' http://127.0.0.1:9090/proxies/Proxy`。实测 Hysteria2 更快（约 2.2 MB/s），设为默认。
- **虚拟环境里 NumPy 报错**（`A module that was compiled using NumPy 1.x cannot be run in NumPy 2.x`）：装 ROS 时带进来的系统 matplotlib 是旧版本，只能配 NumPy 1.x，而虚拟环境开了「使用系统包」，会用到它。解决：在虚拟环境里另装新版，`pip install -U matplotlib scipy`，系统那边不用动。
- **流量**：`nvidia-jetpack` 下载约 7 GB，系统升级约 1.7 GB，再加上 PyTorch、ROS 桌面版，总共约 10 GB。

### 已经提前准备好的东西（2026-11-20）

- **功耗模式**：已切到 MAXN_SUPER（`nvpmodel -m 2`），不需要重启。测性能前还要运行 `sudo jetson_clocks`，重启后会失效。
- **锁定版本**：`apt-mark hold nvidia-jetpack nvidia-tensorrt tensorrt tensorrt-libs libnvinfer11 python3-libnvinfer`。要解锁用 `apt-mark unhold`。
- **模型**：`~/projects/yolo/` 下有 `yolo11n.pt`、`yolo11s.pt`、`yolo11m.pt`。
- **COCO 验证集**：在 `~/datasets/coco`，只有 val2017 的 5000 张图和标注，配置文件是 `~/datasets/coco-val.yaml`。做法：从 Ultralytics 的 GitHub 下载 `coco2017labels.zip`，从 `images.cocodataset.org` 下载 `val2017.zip`，解压后写一个只有 val 的 yaml（train 也指向 val）。用法：`yolo val model=yolo11n.pt data=~/datasets/coco-val.yaml device=0`。
- **精度基线**：yolo11n，PyTorch FP32，mAP50-95 = 0.389（官方 39.5，差距来自 Ultralytics 自带评估和 pycocotools 的算法不同）。每张图的耗时：前处理 0.8 ms，推理 8.8 ms，后处理 2.7 ms（batch=16）。
- **摄像头**：USB 摄像头（得力 1080P）在 `/dev/video0`，1280×720 MJPG 实测 29.8 帧/秒。IMX219 还没接；接上后要用 `sudo /opt/nvidia/jetson-io/jetson-io.py` 选对应的 IMX219 配置，再重启。
- **ROS 摄像头驱动**：`ros-jazzy-v4l2-camera`、`image-tools`、`rqt-image-view`、`image-transport-plugins`。
- **电脑上的 SSH 别名**：`~/.ssh/config` 里有 `jetson`（USB 线，`192.168.55.1`）和 `jetson-wifi`（热点，`172.20.10.7`）。在 Cursor 里用 Remote-SSH 连 `jetson` 就行。

## 常见问题

| 现象 | 原因和处理 |
| --- | --- |
| 开机直接进旧系统，不进 U 盘 | 没按 Esc 选 Boot Manager；重来，在 NVIDIA 标志出现时按 Esc |
| 安装到一半失败 | 多半错过了 QSPI 更新提示没按 Y；断电重来 |
| 装完重启又进了安装程序 | U 盘没拔；拔掉再重启 |
| `apt` 报证书错误、时间不对 | 时间没同步；`sudo timedatectl set-ntp true`，换个网络 |
| `apt install nvidia-jetpack` 很慢 | 正常，约一小时；可以先做 4.2 换 Ubuntu 源，英伟达源不要换 |
| torch 报 `no kernel image is available` | 装了错的 PyTorch；回 6.2 |
| `import torch` 报缺 `libcudnn` 之类 | 6.2 第 4 行那四个 nvidia 库没装 |
| SSH 报 `REMOTE HOST IDENTIFICATION HAS CHANGED` | 重装后主机密钥变了；`ssh-keygen -R <旧IP或新IP>` |
| `rosdep update` 失败 | 访问 GitHub 受限；不影响 apt 装的包，先跳过 |
| 推理比官方慢很多 | 功耗模式不对或没锁频；`sudo nvpmodel -m 2 && sudo jetson_clocks`；引擎要在本板导出 |

## 来源

英伟达 Jetson Orin Nano 开发套件用户指南（Quick Start、Hardware Layout、BSP Setup）、JetPack 7.2.1 下载页、Jetson Linux r39.2 开发者指南（支持的模组与载板表）、Jetson AGX Orin 用户指南的 JetPack 安装章节（`nvidia-jetpack`）、Ultralytics 的 NVIDIA Jetson 指南（JetPack 7.2 章节）、第三方 JetPack 7.2 PyTorch 安装说明、英伟达开发者论坛关于 JetPack 7.2 PyTorch 的帖子、ROS 2 Jazzy 官方安装文档、清华大学开源镜像站 ROS2 帮助页、Isaac ROS 4.6 入门指南。均于 2026-11-18 核对。
