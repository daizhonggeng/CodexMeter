# 故障排查

这份文档按启动链路整理：主机 daemon、BLE、USB、宠物资源、Codex 数据源和固件构建。

## 先看状态

启动 daemon 后，默认控制页在：

```text
http://127.0.0.1:3490/
```

优先看这些字段：

- `Mode`：当前是 `ble`、`serial`、`usb-update` 还是 `local`。
- `BLE`：是否已连接，或最近一次 BLE 错误。
- `Pet update`：宠物资源是否已同步，或是否在等待 USB。
- `Serial`：当前自动识别到的串口。
- `Last error`：最近一次失败原因。

macOS 后台服务日志默认在：

```bash
tail -f ~/Library/Logs/CodexMeter/sync.out.log
tail -f ~/Library/Logs/CodexMeter/sync.err.log
```

Windows 前台运行时直接看 PowerShell 输出。

## daemon 启不起来

1. 确认 Python 版本可用：

   ```bash
   python3 --version
   ```

   Windows 可以用：

   ```powershell
   py -3 --version
   ```

2. 重新安装主机依赖：

   ```bash
   python3 -m pip install -r daemon/requirements.txt
   ```

   Windows：

   ```powershell
   .\run-windows.ps1 -InstallDeps
   ```

3. 手动跑一次 daemon，确认能读取 Codex：

   ```bash
   python3 daemon/codexmeter_daemon.py --once --cwd "$PWD"
   ```

如果这里提示找不到 `codex`，在 macOS LaunchAgent 场景下可以通过 `CODEX_BIN` 指定 Codex Desktop 内置 CLI 路径后重新安装服务。

## 额度显示失败

daemon 通过 `codex app-server` 读取账号和额度。常见原因：

- Codex Desktop 或 Codex CLI 没有登录。
- `codex` 不在 PATH 里。
- Codex app-server 协议变更。
- 当前系统权限阻止后台服务访问 Codex CLI。

排查命令：

```bash
which codex
codex --version
python3 daemon/codexmeter_daemon.py --usage-once
```

如果手动命令能读到额度，但后台服务不行，优先检查 LaunchAgent 使用的 `CODEX_BIN` 和日志。

## BLE 找不到设备

1. 确认设备已刷入当前固件，并且串口启动日志里出现 `BLE: advertising`。
2. 让设备重新上电，等待 3 到 5 秒后再启动 daemon。
3. macOS 上检查蓝牙权限，必要时重新授权终端或 Python。
4. Windows 上优先使用 `run-windows.ps1`，它会默认启用 BLE 并自动识别 USB 串口。
5. 如果 BLE 一直失败，但 USB 已连接，daemon 会尝试串口实时镜像；控制页里 `Mode` 或 `Serial` 可以确认是否进入 USB 兜底。

设备侧可以通过串口发：

```text
BLE_STATUS
```

如果需要清掉旧配对记录，可以发：

```text
BLE_CLEAR
```

然后重启 daemon 和设备。

## USB 串口识别不到

macOS / Linux：

```bash
ls /dev/cu.usbmodem* /dev/tty.usbmodem* 2>/dev/null
```

Windows：

```powershell
[System.IO.Ports.SerialPort]::getportnames()
```

也可以显式指定端口：

```bash
python3 daemon/codexmeter_daemon.py --watch --ble --sync-pet-sprite --serial-port /dev/cu.usbmodem1101
```

Windows：

```powershell
.\run-windows.ps1 -SerialPort COM3
```

如果串口能看到但写入失败，拔插 USB 后再试；刷机后第一次串口打开可能需要等待设备重启完成。

## 宠物资源没有写入

当控制页显示 `waiting_for_usb` 时，说明当前宠物 atlas 还没有写入设备缓存。

处理方式：

1. 保持 daemon 运行。
2. 插上 USB。
3. 等控制页 `Pet update` 从 `waiting_for_usb` 变为 `updating`，然后变为 `synced`。
4. 如果没有自动开始，点控制页里的“写入当前宠物”。

手动确认当前宠物目录：

```bash
python3 daemon/codexmeter_daemon.py --once --sync-pet-sprite --cwd "$PWD"
```

如果报 `pet.json not found` 或 `spritesheet not found`，检查 Codex 宠物是否已安装，以及 `--pet-dir` 是否指向包含 `pet.json` 和 `spritesheet.webp` 的目录。

## 设备输出不更新

优先确认三件事：

1. daemon 是否处于 `--watch` 模式。
2. `--cwd` 是否是当前 Codex 工作目录。
3. Codex 本地 `state_5.sqlite` 和 rollout 日志是否能被当前用户读取。

手动测试：

```bash
python3 daemon/codexmeter_daemon.py --once --cwd "$PWD" --output-text "hello"
```

如果这能生成 payload，但设备没有变化，问题多半在 BLE 或 USB 传输层。看控制页 `BLE`、`Serial` 和 `Last error`。

## 最近会话列表不对

最近会话来自 Codex 本地数据库里的 `threads` 表。daemon 会优先找当前 `--cwd` 的线程，再退回最近未归档线程。

如果列表不符合预期：

- 确认 daemon 的 `--cwd` 和正在使用的项目目录一致。
- 在设备上重新打开会话列表，或短按 BOOT 触发下一会话。
- 重启 daemon，让它重新读取本地数据库。

## 截图失败

截图走 USB 串口，不走 BLE。

macOS / Linux：

```bash
./screenshot.sh screenshots/capture.png /dev/cu.usbmodem1101
```

Windows：

```powershell
.\screenshot-windows.ps1 -Output screenshots\capture.png -Port COM3
```

失败时先确认没有另一个 daemon 正占用串口。需要时先停止后台服务或关闭前台 runner。

## 固件编译失败

先确认 PlatformIO 可用：

```bash
pio --version
```

重新编译：

```bash
pio run -d firmware -e waveshare_lcd_349
```

如果提示依赖下载失败，通常是网络或 PlatformIO package cache 问题。清理后重试：

```bash
rm -rf firmware/.pio
pio run -d firmware -e waveshare_lcd_349
```

如果是头文件找不到，确认正在使用仓库里的 `firmware/platformio.ini`，不要依赖作者本机的 PlatformIO 路径。

## 仍然无法定位

请同时保留下面信息，方便复现：

- 操作系统和 Python 版本。
- `pio --version`。
- daemon 启动命令。
- 控制页 `/api/status` 输出。
- 最近 100 行 daemon 日志。
- 设备串口里 `BLE_STATUS`、`TOUCH_STATUS` 或 `PET_ANIM_STATUS` 的输出。
