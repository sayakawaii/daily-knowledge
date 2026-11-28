# 打开 192.168.1.5（4080 主机）WSL 的 SSH

SSH 开在 **WSL** 里，端口 2222（避开 Windows 自带 SSH 的 22），以后训练也在 WSL 里跑。

## 第 1 步：看 Windows 版本

按 `Win+R`，输入 `winver`。下面的步骤要求 **Windows 11**（第 3 步的 `mirrored` 网络模式需要）。如果是 Windows 10，先停下，做法不一样。

## 第 2 步：在 WSL 的 Ubuntu 终端里粘贴

依次是：放公钥、启用 systemd、装 SSH 服务、把端口改成 2222。

```bash
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo 'ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQClJDE8pxBWb/9gcdO0PqVX6Bx6La/fLY5MT5yPSPzQcIHBdKUaz36X+FKnhDk/F/ME85tpH1jxk6B4ARWS9qrY6R0Xile+j7eb1JcuxpP1pDmTZW7NUrEEtEQtR38MenhVJfjsLJ4fJDw73tGHSTRawzSul65lwJt77j5MTQF002jAQRF9v8u5RxnebwFDadf79lIguTjVnItdtveqwHJjNpAyEtvx7x7OmgAXCBYmHE8F9tGKz2ZRZ7MsSgXNjheGoN7llXZPCmwLT42oGKglhCaNphMOFTpTkQ0vkGQe0tO6MDU+R5I1//sHqFBKDvFZHIwbjOLUnXwBLf7oq05bZ8QQhjKmyPSmXmq7o1UgPudNh/zeAsOs2AyJmvt6yZ/WgX2utaaEiPRJhFq4ur9o78TIq2d7vBgwW47PuItJyquTonUActwiLZr0vKI1f/TeiZXF/OsnReJKVXVQDyV1m2ipmix9PFjKRzDUDHv1eBXTuUMuI+3GViZmF9l1Jxk= laptop' >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
grep -q "systemd=true" /etc/wsl.conf 2>/dev/null || printf "[boot]\nsystemd=true\n" | sudo tee -a /etc/wsl.conf
sudo apt update && sudo apt install -y openssh-server
sudo sed -i 's/^#\?Port .*/Port 2222/' /etc/ssh/sshd_config
sudo systemctl disable --now ssh.socket; sudo systemctl enable --now ssh.service
whoami; nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv
```
这里是输出结果
❯ whoami; nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv
mingheh
name, memory.total [MiB], driver_version
NVIDIA GeForce RTX 4080, 16376 MiB, 621.40

## 第 3 步：在 Windows 上用管理员身份打开 PowerShell 粘贴

依次是：WSL 直接使用主机的局域网地址、WSL 空闲时不自动关闭、防火墙放行 2222 端口、允许外部连进 WSL。

```powershell
Add-Content $env:USERPROFILE\.wslconfig "[wsl2]`nnetworkingMode=mirrored`nvmIdleTimeout=-1"
New-NetFirewallRule -DisplayName "WSL SSH 2222" -Direction Inbound -Protocol TCP -LocalPort 2222 -Action Allow
Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow
wsl --shutdown
```

## 第 4 步：重新打开一次 WSL 终端，然后自测

WSL 会带着 SSH 一起启动。在同一台电脑的 PowerShell 里：

```powershell
ssh -p 2222 你的WSL用户名@localhost
```

能连上就说明 SSH 服务正常（这一步会要密码，正常）。主机每次重启后，也要先打开一次 WSL 终端。

## 做完后

把第 2 步最后两行的输出（WSL 用户名和显卡信息）发给导师。之后由导师远程把 Jetson 的别名（`jetson-home`、`jetson-lan`、`jetson-usb`）和 `jetson_off` 配到这台电脑的 WSL 和 Windows 上。
