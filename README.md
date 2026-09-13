# lid-angle

`lid-angle` 是一个 macOS 命令行工具，用普通用户权限读取 Apple 内置的 lid-angle HID 传感器。角度表示屏幕相对键盘底座的开合角度：屏幕合上接近 `0°`，屏幕垂直于底座接近 `90°`；最大开合角度取决于具体机型。程序执行只调用 HID 读取接口，不写入传感器寄存器。

## 当前协议依据

当前 M2 Pro Mac14,9 上，`hidutil` 和 IOHID 枚举到的精确设备属性为：

```text
VendorID     0x05AC (Apple)
ProductID    0x8104
UsagePage    0x0020
Usage        0x008A
```

设备 ReportDescriptor 的 Report ID 1 字段为：Usage Page `0x20`、Usage `0x047F`、Logical Minimum `0`、Logical Maximum `360`、Report Size `9`、Report Count `1`、Variable Input。通过 `IOHIDDeviceGetReport` 请求 Feature Report 1 时，本机返回 3 字节：第 1 字节是 Report ID，后两字节是小端 9-bit 数值。例如 `01 6d 00` 解码为 `0x006d = 109°`。

程序启动时会再次解析 ReportDescriptor，并且只接受上述字段；返回长度、Report ID、保留位或范围异常时会报告错误。实现参考了 [samhenrigold/LidAngleSensor 的已核实源码](https://github.com/samhenrigold/LidAngleSensor/blob/f7e4e5cb46fe13a518091ce5d47f0ec2e3fecd80/LidAngleSensor/LidAngleSensor.swift) 的 HID 读取方式；该项目源码使用 Apache License 2.0，许可证文本随本目录提供。

ReportDescriptor 给出的是整数范围和字段分辨率。默认输出保留一位小数用于稳定的 CLI 格式，本接口输出步长为 1°；真实测量精度受硬件和系统实现影响，本工具未对此作标定承诺。

## 快速开始

准备好 Xcode Command Line Tools 后，从仓库获取源码并构建：

```sh
git clone https://github.com/yznn007/lid-angle.git
cd lid-angle
make
./lid-angle
```

构建完成后，在项目目录执行以下命令安装。持有本地预编译交付包时，也可在解压后的项目目录直接执行：

```sh
mkdir -p "$HOME/.local/bin"
install -m 0755 ./lid-angle "$HOME/.local/bin/lid-angle"
"$HOME/.local/bin/lid-angle" --once
```

在当前终端启用命令名：

```sh
export PATH="$HOME/.local/bin:$PATH"
lid-angle
```

需要每次打开终端都生效时，将上述 `export PATH` 行加入自己的 shell 配置。卸载命令为 `rm "$HOME/.local/bin/lid-angle"`。项目本身只生成和安装这个可执行文件。

## 构建

需要 macOS、Apple clang、IOKit 和 CoreFoundation 系统框架：

```sh
cd /path/to/lid-angle
make
./lid-angle --once
```

Makefile 默认将最低 macOS 部署目标设为 11.0；实测环境为 Apple M2 Pro（Mac14,9）、macOS 26.6.2。构建会在项目根目录生成 `lid-angle`，该产物由 `.gitignore` 忽略。本地预编译交付包中的程序为 Apple Silicon arm64 架构。

## 使用

```sh
./lid-angle                    # TTY 中每 100 ms 原行刷新
./lid-angle --interval 250     # 每 250 ms 读取
./lid-angle --once             # 输出一行纯数字，例如 109
./lid-angle --once | awk '{print $1}'
```

`--interval N` 接受 20 到 60000 的十进制整数毫秒。`--once` 可以和 `--interval` 一起使用，间隔参数在单次读取中仅保留统一的参数格式。输出重定向或进入管道时，每次成功采样输出一行纯数字并立即刷新；TTY 模式显示一位小数和度数符号。Ctrl-C、SIGTERM 和管道消费者提前退出都会结束读取并释放 HID 连接。

退出码：`0` 表示成功；`2` 表示参数错误；`3` 表示 HID 管理器或设备初始化失败；`4` 表示持续读取失败、设备失联、报告格式异常或输出失败。设备失联时，程序每 500 毫秒重新枚举并连接目标设备，持续失败约 5 秒后退出。重连周期独立于采样间隔；失联期间暂停数字输出，TTY 显示“读取失败”。

## 安装与卸载

默认安装到 `$HOME/.local/bin`，也可以指定带空格的路径。命令不会自动修改 PATH：

```sh
make install PREFIX="$HOME/.local"
make uninstall PREFIX="$HOME/.local"
make install PREFIX="/tmp/lid angle/prefix"
```

制作暂存目录时可使用 `DESTDIR`：

```sh
make install PREFIX="/opt/lid-angle" DESTDIR="/tmp/stage"
```

## 限制

本程序针对上述 Apple HID 标识和 ReportDescriptor 设计。其他机型、不同系统版本或系统驱动变化可能返回“设备不存在”或“报告格式异常”；程序会安全退出并给出 stderr 诊断。角度跟随、合盖睡眠和唤醒后的恢复需要在目标机器上由用户进行物理验证。当前二进制带有链接器生成的 ad-hoc 签名；Developer ID 签名、公证和其他机器的运行验证属于后续发布工作。
