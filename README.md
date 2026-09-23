# ATK Tray

ATK Tray 是面向 ATK 用户的 Windows 托盘电量显示工具。它在本机读取 ATK 鼠标和兼容耳机的电量，并在任务栏托盘图标、右键菜单和 OSD 中显示状态。

## 功能

- 托盘电池图标显示鼠标当前电量，并按电量状态使用绿色、黄色或红色提示。
- 自动识别 ATK 鼠标，读取 HID Feature Report 中的电量字段。
- 支持 ATK N9 Ultra 耳机，通过 ATK HUB 使用的 USB 串口协议读取耳机电量和充电状态。
- 右键菜单和 OSD 提供中文状态信息、重新连接、开机启动和退出功能。
- 设备查询在独立线程中运行，耳机查询低频执行，不会抢占 ATK HUB 的串口。
- 纯原生 Win32 程序，无运行时依赖，不上传设备数据。

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

程序启动后会出现在 Windows 任务栏右下角的托盘区域。左键点击图标显示 OSD，右键点击图标打开菜单。

## 当前协议适配

### ATK 鼠标

程序会自动寻找 Vendor ID 为 `0x373B`、具有 `COL06` 接口的 HID 设备，并读取 Feature Report ID `0x06`。报告下标 `3` 为电量字段，范围为 `0..100`。

不同 ATK 型号可能使用不同的 HID 报告结构。如果型号没有 `COL06` 接口，程序会等待并继续扫描，不会修改鼠标配置。

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

如果耳机无法显示电量，请保持 ATK HUB 正常运行，确认设备以 USB 模式连接，并检查日志中是否提示串口被 ATK HUB 占用。右键菜单中的“重新连接设备”可以手动触发重新扫描。

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

`src`、`res`、`installer`、构建脚本和探测工具属于开发文件，不需要放进普通用户下载包。探测工具不会随发布包提供。

## 隐私和安全

ATK Tray 只在本机读取 HID Feature Report 和耳机电量串口回包，不上传设备数据，不采集遥测信息，也不会调用 ATK 账户或网络服务。程序不会修改 DPI、灯效、固件、音量或设备配置。

本项目是社区开发的独立工具，不是 ATK 官方软件。ATK、ATK Gear 及相关产品名称归其权利人所有。

## 许可证

本项目使用 MIT License，详见 [LICENSE](LICENSE)。
