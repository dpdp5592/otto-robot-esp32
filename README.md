# otto-robot-esp32

该工程是从 `xiaozhi-esp32` 收束出的 `otto-robot` 最小可烧录版本。

目标：

- 仅保留 `otto-robot` 板级源码
- 仅保留 `boards/common` 公共板级依赖
- 保留 `esp32s3` 下可完成编译、烧录和串口监视的最小工程结构

当前板级目录仅包含：

- `main/boards/otto-robot`
- `main/boards/common`

## 文档入口

- [docs/README.md](./docs/README.md)
- [docs/getting-started.md](./docs/getting-started.md)
- [docs/board-and-openclaw.md](./docs/board-and-openclaw.md)

## 前提

- 已安装 ESP-IDF `v5.5.2`
- 已执行 `source ~/esp/esp-idf/export.sh`
- 已连接 Otto 开发板
- 已在 Ubuntu 22.04 + ESP-IDF `v5.5.2` 环境完成一次完整 `idf.py set-target esp32s3` + `idf.py -DBOARD_NAME=otto-robot build` 验证

也可以先执行：

```bash
bash scripts/check_prereqs.sh
```

如果本地曾经保留过旧的 `managed_components/` 或 `build/` 缓存，建议先清理后再重新编译：

```bash
rm -rf managed_components build sdkconfig
```

## 最短烧录路径

```bash
cd /path/to/otto-robot-esp32
idf.py set-target esp32s3
idf.py fullclean
idf.py menuconfig
```

在 `menuconfig` 中建议确认：

- `Xiaozhi Assistant -> Board Type -> ottoRobot`
- `Xiaozhi Assistant -> Default OTA URL -> 改成实际 OTA 地址`

然后执行：

```bash
idf.py -DBOARD_NAME=otto-robot build
idf.py -p /dev/ttyACM0 flash monitor
```

退出串口监视器：

```bash
Ctrl+]
```

## 关键文件

- [main/boards/otto-robot/config.h](./main/boards/otto-robot/config.h)
- [main/boards/otto-robot/config.json](./main/boards/otto-robot/config.json)
- [main/boards/otto-robot/README.md](./main/boards/otto-robot/README.md)
- [main/Kconfig.projbuild](./main/Kconfig.projbuild)
- [main/CMakeLists.txt](./main/CMakeLists.txt)
- [sdkconfig.defaults.esp32s3](./sdkconfig.defaults.esp32s3)

## 与 OpenClaw 相关的板级能力

该精简工程保留了 Otto 当前用于 OpenClaw 联动的本地控制能力，包括：

- 板载 WebSocket 控制端
- `self.otto.pose`
- `self.otto.save_home`
- Otto 本地 MCP 工具响应能力

相关实现位于：

- [main/boards/otto-robot/websocket_control_server.cc](./main/boards/otto-robot/websocket_control_server.cc)
- [main/boards/otto-robot/otto_controller.cc](./main/boards/otto-robot/otto_controller.cc)

## 说明

该目录不是通用多板型仓库，不再以保留所有开发板为目标。
如果需要其它开发板，应回到原始 `xiaozhi-esp32` 仓库。
