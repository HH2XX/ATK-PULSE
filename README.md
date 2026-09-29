# ATK // PULSE

ATK // PULSE（程序文件名：`atk-tray.exe`）是一个面向 ATK 设备的个人自用 Windows 托盘工具。它在本机读取指定鼠标和耳机的电量，并在任务栏托盘图标、右键菜单和 OSD 中显示状态。

## 功能

- 托盘脉冲图标显示鼠标当前电量，并按电量状态使用荧光黄绿、琥珀色或红色提示。
- 针对作者使用的 ATK Mouse 8K Dongle 读取实时电量、DPI 档位和 X/Y DPI。
- 针对作者使用的 ATK N9 Ultra 耳机读取电量和充电状态。
- 右键菜单和 OSD 提供中文状态信息、重新连接、开机启动和退出功能。
- 设备查询在独立线程中运行，耳机查询低频执行，不会抢占 ATK HUB 的串口。
- 纯原生 Win32 程序，无运行时依赖，不上传设备数据。
- 使用独立的 ATK // PULSE 视觉界面：脉冲标记托盘图标、鼠标/耳机双行状态面板和高对比电量轨道。

## 下载和使用

推荐使用安装器：

```text
dist/atk-tray-setup.exe
```

安装器会将程序安装到当前用户目录：

```text
%LOCALAPPDATA%\Programs\ATK Tray
```

也可以直接运行便携版：

```text
bin/atk-tray.exe
```

程序启动后会出现在 Windows 任务栏右下角的托盘区域。左键点击图标显示 OSD：鼠标和耳机会分别显示电量、进度条及连接/充电状态；右键点击图标打开菜单。

开机自动启动默认随安装启用，也可以在托盘右键菜单中切换。程序会校验启动项是否指向当前文件；移动便携版后，请重新开启一次“开机自动启动”，以更新启动路径。若安全软件曾禁用该启动项，重新开启会清理旧的禁用状态。

## 适配范围和限制

本项目最初就是根据作者自己的设备、ATK HUB 行为和抓取到的协议编写的，并不是完整的 ATK 全型号通用驱动。发布包可以直接给其他用户试用，但不能保证所有 ATK 鼠标、接收器或耳机都能工作。

目前明确适配并实际验证过的设备：

| 设备 | 识别条件 | 已实现功能 |
| --- | --- | --- |
| ATK Mouse 8K Dongle | VID `0x373B`、PID `0x101B`、HID `COL05` | 鼠标电量、DPI 档位、X/Y DPI |
| ATK N9 Ultra | VID `0x373B`、PID `0x1204` 的 USB 串口接口 | 耳机电量、充电状态 |

程序还包含一条兼容旧式 ATK 鼠标的 HID 路径：VID 为 `0x373B`、存在 `COL06` 接口、Feature Report ID 为 `0x06` 且电量位于报告下标 `3` 时，可能可以读取鼠标电量。这个条件不是型号名单，也不代表 DPI 或其他功能一定兼容。

不匹配的设备会显示未连接或等待状态，不会修改设备配置。程序不会写入 DPI、灯效、固件、音量或其他设置。

## 给其他设备使用

如果你的设备协议不同，可以基于源码自行适配，建议先用设备管理器、HID 工具或 ATK HUB 日志确认 VID/PID、接口编号、报告 ID、字段位置和校验方式，再修改 `src/main.cpp`：

- 鼠标接收器识别和实时协议：`FindAtk8KReceiverPath`、`ReadAtk8KReceiverBattery`、`ReadAtk8KReceiverDpi`。
- 旧式鼠标 HID 电量：`FindAtkBatteryPath`、`ReadAtkBattery`。
- 耳机串口设备：`FindN9ComPort`、`ReadN9HeadsetBattery`。

修改后使用仓库中的 `build.bat` 重新编译。不要直接套用其他型号的字段偏移或命令；协议不确定时宁可显示 `--`，也不要把其他字段误报成电量。适配其他设备属于用户自行修改和验证的范围，本项目不承诺对未验证型号提供支持。

## 当前协议细节

### ATK 鼠标

ATK Mouse 8K Dongle（VID `0x373B`、PID `0x101B`）使用 `COL05` 的输出报告 ID `0x08` 发送实时电量请求，并校验回包中的电量字段；兼容旧式路径则读取 `COL06` 的 Feature Report ID `0x06`，报告下标 `3` 为电量字段，范围为 `0..100`。

对于 ATK Mouse 8K Dongle，程序还会直接读取 ATK HUB 使用的 EEPROM 查询协议，获取当前 DPI 档位和对应的 X/Y DPI，不依赖 HUB 界面显示。DPI 读取失败时会保留上一次有效值，不会显示猜测值。

不同 ATK 型号可能使用不同的 HID 报告结构。如果实时协议暂时没有收到有效回包，程序不会把旧的缓存值误报为实时电量，而是继续重试；程序不会修改鼠标配置。

### ATK N9 Ultra

| 项目 | 参数 |
| --- | --- |
| USB VID/PID | `0x373B / 0x1204` |
| 接口 | USB Composite Device 的串口接口 |
| 串口参数 | `9600 8N1`，无流控 |
| 请求 | 64 字节帧，开头为 `02 00 03` |
| 电量字段 | 回包下标 `4` 的低 7 位，范围 `0..100` |
| 充电状态 | 回包下标 `4` 的最高位为 `1` |

程序按照 VID/PID 自动寻找 COM 端口，不固定使用 COM4。当 ATK HUB 正在独占串口时，耳机显示为等待状态；串口释放后程序会自动重试。

## 故障排查

如果程序没有出现在托盘中，先确认任务管理器中没有已经运行的 `atk-tray.exe`。程序会将启动、设备发现、串口失败、线程退出和未处理异常记录到：

```text
%LOCALAPPDATA%\ATK Tray\atk-tray.log
```

如果耳机无法显示电量，请保持 ATK HUB 正常运行，确认设备以 USB 模式连接，并检查日志中是否提示串口被 ATK HUB 占用。右键菜单中的“重新连接设备”可以手动触发重新扫描。其他用户的耳机如果 VID/PID 或串口协议不同，需要先自行修改源码适配。

如果开机后程序没有启动，请先运行程序，在托盘右键菜单中关闭再重新开启“开机自动启动”。随后在任务管理器或安全软件的启动项管理中确认 `ATK-Tray` 已启用，且路径指向当前的 `atk-tray.exe`。

## 从源码构建

使用 Visual Studio 2022 Build Tools 的 x64 Developer Command Prompt：

```cmd
build.bat
```

主程序输出到 `bin/atk-tray.exe`。构建安装器：

```cmd
cd installer
build_installer.bat
```

安装器输出到 `dist/atk-tray-setup.exe`。

## 发布内容

普通用户只需要以下文件：

```text
atk-tray.exe
atk-tray-setup.exe
README.md
LICENSE
```

`src`、`res`、`installer`、构建脚本和其他开发文件属于源码仓库内容，不需要放进普通用户下载包。需要自行适配设备的用户应保留源码并从源码构建；只想直接使用的用户只需下载发布压缩包。

## 隐私和安全

ATK // PULSE 只在本机读取 HID Feature Report 和耳机电量串口回包，不上传设备数据，不采集遥测信息，也不会调用 ATK 账户或网络服务。程序不会修改 DPI、灯效、固件、音量或设备配置。

本项目是个人自用、社区分享的独立工具，不是 ATK 官方软件。ATK、ATK Gear 及相关产品名称归其权利人所有。未验证设备请自行备份、测试并承担修改风险。

## 许可证

本项目使用 MIT License，详见 [LICENSE](LICENSE)。
