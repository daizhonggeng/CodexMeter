# CodexMeter

[English](README.en.md) | [第三方说明](THIRD_PARTY_NOTICES.md)

CodexMeter 是一个面向 Codex Desktop 的 ESP32-S3 桌面伴侣屏。它运行在
Waveshare ESP32-S3-Touch-LCD-3.49 上，把当前最值得抬头看一眼的信息放到
一块常亮的小屏里：额度、重置时间、当前会话输出、上下文窗口进度、宠物状态，
以及最近会话切换。

> 当前主机侧 daemon 已支持 `Windows` 和 `macOS`。Windows 下支持 BLE 同步、
> USB 串口自动识别、宠物资源写入、控制页和截图工具。

## 截图

| 实时同步 | 长回复 | 短回复 |
| --- | --- | --- |
| ![Live sync](screenshots/device-live.png) | ![Work log](screenshots/device-worklog.png) | ![Quick reply](screenshots/device-quick-answer.png) |

## 实拍

| 桌面实拍 | 会话列表 | 中文输出 |
| --- | --- | --- |
| ![Desk live](screenshots/real-photos/device-desk-live.png) | ![Desk sessions](screenshots/real-photos/device-desk-sessions.png) | ![Desk Chinese](screenshots/real-photos/device-desk-cn.png) |

## 当前已经实现的功能

### 1. Codex 额度显示

- 精确显示 5 小时额度百分比
- 精确显示周额度百分比
- 显示两个额度窗口各自的重置时间
- 支持设备端手动触发额度刷新
- 在额度刷新中和刷新完成后给出状态反馈

### 2. Codex 输出同步

- 从当前活跃会话提取最新的助手输出
- 将输出按设备屏幕宽度分行，显示在右侧消息气泡
- 支持短回复的大字号居中显示
- 支持长回复的多行显示
- 支持在消息区上下滑动查看未完全展示的内容
- 在状态栏中显示当前同步状态，例如 `SYNC`、`DONE`、`INPUT`、`ERROR`

### 3. 上下文窗口进度

- 读取 Codex 当前上下文输入 token 数
- 读取模型上下文窗口总 token 数
- 在右下角显示精确的 `已用 / 总量`
- 用进度条显示上下文占用比例
- 根据占用比例切换正常、提醒、警告颜色

### 4. 宠物同步与动画

- 自动读取当前选中的 Codex 宠物
- 读取本地宠物 `pet.json` 和 `spritesheet.webp`
- 首次切换宠物时通过 USB 写入设备
- 写入完成后，日常状态切换通过 BLE/串口指令完成
- 设备端支持 idle / running / waiting / review / failed 等动画状态
- 设备重启后会从本地缓存恢复上一次写入过的宠物动画资源

### 5. 最近会话切换

- 读取最近会话列表
- 设备端最多展示 8 个最近会话
- 点击消息区可以在“当前输出 / 最近会话列表”之间切换
- 在会话列表中上下滑动可以移动选中项
- 再次点击消息区可确认切换到当前选中会话
- 短按 BOOT 键可以直接切到下一个最近会话
- 长按 BOOT 键约 1.2 秒可以直接打开最近会话列表

### 6. 设备联动与调试能力

- 自定义 BLE GATT 服务用于实时同步
- USB 串口用于刷机、截图、宠物资源写入和调试
- 当 BLE 不可用但 USB 已连接时，daemon 会自动通过串口接管实时同步
- 设备侧的额度刷新、BOOT 切会话、会话列表请求在 USB 直连时也会回传主机
- 支持 framebuffer 截图导出
- 提供本地控制页查看 daemon 状态和日志

## 当前交互逻辑

### 触摸交互

| 区域 | 行为 |
| --- | --- |
| 左侧额度区 | 触发额度刷新 |
| 中间宠物区 | 单击一次开始跑动，再点一次回到 idle |
| 右侧消息区 | 单击打开最近会话列表；若列表已打开则确认切换 |
| 右侧消息区上滑/下滑 | 正常输出时滚动消息；会话列表打开时移动选中项 |

### 物理按键

| 输入 | 行为 |
| --- | --- |
| BOOT 短按 | 切换到下一个最近会话 |
| BOOT 长按约 1.2 秒 | 打开最近会话列表 |
| PWR 长按约 3 秒 | 关机 |

### 宠物同步规则

1. daemon 会持续检查当前选中的 Codex 宠物。
2. 如果设备里还没有这个宠物的 atlas，会提示插线更新。
3. 插上 USB 后，daemon 会把当前宠物资源写入设备缓存。
4. 之后只需要发送动画状态选择指令，不需要每次整包重传图片。

### USB 兜底同步规则

1. 正常情况下，设备日常实时同步优先走 BLE。
2. 如果 BLE 暂时不可用，但 USB 线仍然连接，daemon 会自动切到串口实时镜像模式。
3. 在串口实时镜像模式下，额度刷新、最近会话切换、列表请求等设备侧操作也会回传主机。
4. 一旦 BLE 恢复，日常状态同步可以继续走 BLE。

## 工作原理

### 主机侧 daemon

`daemon/codexmeter_daemon.py` 负责：

- 调用 `codex app-server` 读取账户额度信息
- 读取 Codex 本地数据库中的最近线程和当前选中线程
- 从 rollout 日志中提取最新助手输出和状态
- 读取当前选中的宠物目录
- 把额度、输出、上下文、会话列表、宠物状态组合成紧凑 payload
- 优先通过 BLE 发给设备，在 USB 在线时也支持串口实时镜像和事件回传

### 设备侧固件

`firmware/src/main.cpp` 负责：

- 渲染 640x172 横向三栏界面
- 处理触摸输入和 BOOT / PWR 交互
- 解析 JSON payload
- 缓存宠物 atlas / animation
- 在本地完成宠物动画播放与 UI 更新

### 传输方式

- **BLE**：日常实时同步，发送额度、输出、上下文、会话列表、状态切换
- **USB 串口**：刷机、截图、宠物 atlas 写入、BLE 不可用时的实时镜像，以及设备事件回传

## 硬件信息

- 开发板：Waveshare ESP32-S3-Touch-LCD-3.49
- 面板逻辑布局：640 x 172 横向界面
- 实际面板方向：172 x 640 原生坐标，经固件映射后横向显示
- 触摸：电容触摸
- 供电与关机：板载 PMU 管理

## 快速开始

### 1. 安装主机依赖

```bash
python3 -m pip install -r daemon/requirements.txt
```

### 2. 编译固件

```bash
pio run -d firmware -e waveshare_lcd_349
```

### 3. USB 刷机

macOS / Linux：

```bash
./flash.sh /dev/cu.usbmodem1101
```

Windows：

```powershell
.\flash-windows.ps1 -Port COM3
```

不传 `-Port` 时由 PlatformIO 自动选择端口。

### 4. 启动 daemon

macOS / Linux：

```bash
python3 daemon/codexmeter_daemon.py \
  --watch \
  --ble \
  --sync-pet-sprite \
  --cwd "$PWD"
```

Windows：

```powershell
.\run-windows.ps1 -InstallDeps
```

默认会自动选择 USB 串口并启用 BLE。若要指定端口：

```powershell
.\run-windows.ps1 -SerialPort COM3
```

### 5. 安装 macOS 后台服务

```bash
./install.sh
```

## 常用命令

### 截图

macOS / Linux：

```bash
./screenshot.sh screenshots/capture.png /dev/cu.usbmodem1101
```

Windows：

```powershell
.\screenshot-windows.ps1 -Output screenshots\capture.png -Port COM3
```

不传 `-Port` 时自动选择优先级最高的 USB 串口。

### 控制页

默认控制页地址：

```text
http://127.0.0.1:3490/
```

控制页可以查看：

- daemon 当前运行模式
- 最近同步状态
- 当前串口
- 当前宠物
- 最近错误
- daemon 日志

## 关键参数

### daemon 参数

- `--serial-port /dev/cu.usbmodem1101`
- `--auto-serial-port "/dev/cu.usbmodem*"`，Windows 下默认是 `auto`，也可以传 `COM3` 或 `COM*`
- `--serial-baud 921600`
- `--control-port 3490`
- `--pet-dir PATH`
- `--pet-anim-frames codex`
- `--sync-pet-sprite`
- `--ble`
- `--watch`

### BLE 服务

| 通道 | UUID | 方向 |
| --- | --- | --- |
| Service | `6f1c0001-7f7d-4a2b-9b7a-3490c0d3e001` | service |
| RX | `6f1c0002-7f7d-4a2b-9b7a-3490c0d3e001` | 主机写入设备 |
| TX | `6f1c0003-7f7d-4a2b-9b7a-3490c0d3e001` | 设备 ack / nack notify |
| REQ | `6f1c0004-7f7d-4a2b-9b7a-3490c0d3e001` | 设备向主机发起刷新请求 |

当前广播名：

```text
CodexMeter
```

## 仓库结构
| 路径 | 说明 |
| --- | --- |
| `firmware/` | PlatformIO 固件工程 |
| `firmware/src/main.cpp` | 设备主 UI、触摸、串口协议、宠物缓存 |
| `firmware/src/ble.*` | BLE 同步服务 |
| `daemon/codexmeter_daemon.py` | 主机侧同步 daemon |
| `screenshots/` | README 使用的设备截图 |
| `flash.sh` | 串口刷机脚本 |
| `flash-windows.ps1` | Windows 串口刷机脚本 |
| `install.sh` | macOS LaunchAgent 安装脚本 |
| `screenshot.sh` | framebuffer 截图脚本 |
| `screenshot-windows.ps1` | Windows framebuffer 截图脚本 |
| `run-windows.ps1` | Windows 前台 daemon 启动脚本 |

## 致谢与说明

### 宠物形象来源

当前实机演示里使用的宠物形象来自：

- 名称：`Mikoto`
- 作者：`@legeling`
- 类型：动漫人物

安装命令：

```bash
curl -fsSL https://raw.githubusercontent.com/legeling/awesome-codex-pet/main/scripts/install-pet.sh | bash -s -- mikoto--lingxiaotian
```

### 项目想法来源

CodexMeter 的最初设备化想法受下面这个项目启发：

- [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)

## 当前状态说明

这是一个可以日常使用的个人硬件项目版本，重点已经放在：

- 设备端实时可读性
- 额度与输出同步准确性
- 宠物切换与设备缓存
- 小屏幕上的会话切换效率

后续如果继续扩展，比较自然的方向会是：

- 更完整的蓝牙配对 / 页面引导
- 设备端更多交互页
- 更丰富的宠物状态映射
- 更完整的异常诊断页

## 友情链接

- [LINUX DO - 新的理想型社区](https://linux.do/)
