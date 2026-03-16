# otto-robot-esp32 使用说明

本文档面向需要在本地编译、烧录和调试 `otto-robot` 固件的开发者。

## 1. 项目定位

`otto-robot-esp32` 是从原始 `xiaozhi-esp32` 工程中收束出的单板型版本。

该版本的目标很明确：

- 只服务于 `otto-robot`
- 只保留 `main/boards/otto-robot`
- 只保留 `main/boards/common`
- 保留 Otto 当前与 OpenClaw 联动所需的板级控制能力

## 2. 目录结构

- `main/boards/otto-robot`
  - Otto 板级实现
- `main/boards/common`
  - 公共板级依赖
- `main/Kconfig.projbuild`
  - 项目配置入口
- `main/CMakeLists.txt`
  - 构建入口
- `sdkconfig.defaults.esp32s3`
  - `esp32s3` 默认配置

## 3. 环境准备

建议运行环境：

- Ubuntu 22.04 或相近 Linux 发行版
- ESP-IDF `v5.5.2`
- Python 3
- USB 串口驱动已可正常识别设备

安装基础依赖：

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf cmake ninja-build ccache \
  libffi-dev libssl-dev dfu-util libusb-1.0-0 python3 python3-pip \
  python3-venv
sudo usermod -aG dialout "$USER"
```

## 4. 安装 ESP-IDF 5.5.2

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32s3
```

新开终端后加载环境：

```bash
source ~/esp/esp-idf/export.sh
idf.py --version
```

## 5. 编译前检查

进入工程目录：

```bash
cd /path/to/otto-robot-esp32
```

设置目标芯片并清理旧缓存：

```bash
idf.py set-target esp32s3
idf.py fullclean
```

进入配置界面：

```bash
idf.py menuconfig
```

建议确认以下两项：

- `Xiaozhi Assistant -> Board Type -> ottoRobot`
- `Xiaozhi Assistant -> Default OTA URL -> 实际 OTA 地址`

示例：

- `http://<server-ip>:8002/xiaozhi/ota/`

## 6. 编译

```bash
idf.py -DBOARD_NAME=otto-robot build
```

如果编译顺利，产物会在 `build/` 目录下生成。

## 7. 烧录

先查看串口设备：

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

若设备显示为 `/dev/ttyACM0`，则执行：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

退出串口监视器：

```bash
Ctrl+]
```

## 8. 常见问题

### 8.1 `idf.py` 不存在

原因通常是 ESP-IDF 环境未加载。

处理方式：

```bash
source ~/esp/esp-idf/export.sh
```

### 8.2 串口权限不足

处理方式：

```bash
sudo usermod -aG dialout "$USER"
```

之后重新登录终端会话。

### 8.3 串口不存在

优先检查：

- USB 线缆是否支持数据传输
- Otto 是否正确上电
- 设备是否已进入烧录模式

## 9. 说明

该工程是单板型精简版本，不以支持所有原始开发板为目标。
如果需要回到多板型能力，应使用原始 `xiaozhi-esp32` 仓库。
