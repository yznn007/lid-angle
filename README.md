# lid-angle

`lid-angle` 是一个 macOS 命令行工具，用普通用户权限实时读取 MacBook 内置的屏幕开合角度。程序按 HID Sensor Device Orientation 的标准用途自动寻找传感器，再从设备自己的 Report Descriptor 读取报告编号、字段位置、字段宽度和缩放比例，因此可以覆盖不同的供应商编号、报告布局和精度实现。

程序只读取 HID 报告，不写入传感器寄存器。屏幕合上时角度通常接近 `0°`，屏幕垂直于键盘底座时通常接近 `90°`，最大开合角度取决于具体机型。

## 工作方式

启动时，程序按以下顺序发现和读取传感器：

1. 通过 HID Sensor Device Orientation（Usage Page `0x20`、Usage `0x8A`）匹配设备，同时兼容设备属性使用 `PrimaryUsage` 命名的系统版本。
2. 解析匹配设备的 Report Descriptor，寻找角度字段，自动得到 Report ID、位偏移、有效位宽、逻辑范围和单位指数。
3. 优先使用带小数分辨率的角度字段，再尝试整数角度字段；同时兼容 Feature Report 和 Input Report。
4. 少数系统不提供 Report Descriptor 时，按标准角度报告编号进行安全探测，并继续校验返回值范围。

当前 Apple Silicon 设备常见的两个字段如下，具体设备仍以运行时描述符为准：

| 用途 | 常见报告 | 返回值 | 输出示例 |
| --- | --- | --- | --- |
| Sensor Tilt X (`0x047F`) | Report ID 1 | 整数角度 | `107` |
| Sensor Custom (`0x0545`) | Report ID 7 | 角度的百分之一 | `107.34` |

这种布局来自 HID 传感器报告描述符；项目也参考了 [Clamshell 的通用 HID 读取实现](https://github.com/danielradosa/clamshell/blob/main/Sources/Clamshell/Sensor/LidAngleSensor.swift)。

## 快速开始

准备好 Xcode Command Line Tools 后，从公开仓库构建：

```sh
git clone https://github.com/yznn007/lid-angle.git
cd lid-angle
make
./lid-angle
```

安装到用户目录：

```sh
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
lid-angle --once
```

卸载：

```sh
make uninstall PREFIX="$HOME/.local"
```

项目不会自动修改 shell 配置；需要长期使用时，将 `export PATH="$HOME/.local/bin:$PATH"` 加入自己的 shell 配置文件。

## 使用

```sh
./lid-angle                    # TTY 中持续刷新
./lid-angle --interval 250     # 每 250 ms 采样一次
./lid-angle --once             # 输出一次，例如 107.34
./lid-angle --once | awk '{print $1}'
```

`--interval N` 接受 20 到 60000 的十进制整数毫秒，默认值为 100。`--once` 输出一行纯数字，精度跟随传感器字段；重定向或进入管道时每次采样输出一行并立即刷新。TTY 模式显示角度和度数符号。

设备暂时失联时，程序每 500 ms 重新枚举并连接传感器，持续约 5 秒仍未恢复就退出。Ctrl-C、SIGTERM 和管道消费者提前退出都会释放 HID 连接。

退出码：

| 退出码 | 含义 |
| --- | --- |
| `0` | 读取成功或用户主动结束 |
| `2` | 参数错误 |
| `3` | HID 管理器或单次读取初始化失败 |
| `4` | 持续读取失败、设备失联、报告异常或输出失败 |

## 构建与部署

需要 macOS、Apple clang、IOKit 和 CoreFoundation 系统框架。Makefile 默认设置最低 macOS 部署版本为 11.0：

```sh
make clean
make CFLAGS='-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror'
./lid-angle --once
```

构建产物位于项目根目录，已被 `.gitignore` 忽略。编译器会按当前机器架构生成可执行文件；在 Intel Mac 上编译即可得到 x86_64 版本，在 Apple Silicon Mac 上编译即可得到 arm64 版本。

也可以制作暂存安装目录：

```sh
make install PREFIX="/opt/lid-angle" DESTDIR="/tmp/stage"
```

## 兼容范围与限制

工具适用于暴露 HID Sensor Device Orientation 用途并提供可读角度报告的 MacBook。Apple Silicon 设备和部分较新的 Intel MacBook 通常具备该传感器；实际可用性由机型、macOS 版本和系统驱动决定。程序不维护机型、Vendor ID 或 Product ID 白名单，设备报告变化时由描述符解析决定是否可以读取。

合盖、睡眠、唤醒和外接传感器场景需要在目标机器上进行物理验证。当前二进制使用链接器生成的 ad-hoc 签名；Developer ID 签名、公证和通用二进制发布属于后续发布工作。

## 许可证

本项目使用 Apache License 2.0，详见 [LICENSE](LICENSE)。
