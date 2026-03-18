# otto-robot-esp32

该工程是从 `xiaozhi-esp32` 收束出的 `otto-robot` 最小可烧录版本。

目标：

- 仅保留 `otto-robot` 板级源码
- 仅保留 `boards/common` 公共板级依赖
- 保留 `esp32s3` 下可完成编译、烧录和串口监视的最小工程结构

## 三仓库关系

`otto-robot-esp32` 是三仓库方案中的固件仓库。

完整方案由以下三个仓库组成：

1. `otto-robot-esp32`
   - Otto 固件
   - 提供本地动作、表情、MCP 控制能力
2. `otto-esp32-openclaw-server`
   - 服务端、智控台、body bridge
   - 负责配对、设备桥接与 OpenClaw 接口
3. `openclaw-otto-body-plugin`
   - OpenClaw 插件
   - 对外暴露 `otto_action`、`otto_stop`、`otto_get_status`、`otto_set_theme`、`otto_set_emotion`

建议阅读顺序：

1. 当前 README
2. [docs/getting-started.md](./docs/getting-started.md)
3. [docs/board-and-openclaw.md](./docs/board-and-openclaw.md)
4. `otto-esp32-openclaw-server` README
5. `openclaw-otto-body-plugin` README

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
- Otto 舵机动作调用入口
- Otto 舵机序列调用入口
- `self.otto.pose`
- `self.otto.action`
- `self.otto.servo_sequences`
- `self.otto.stop`
- `self.otto.save_home`
- Otto 本地 MCP 工具响应能力
- Otto 表情资源与表情显示能力

这意味着 OpenClaw 可以通过 Otto 本地 WebSocket MCP 通道直接下发：

- 单次姿态控制
- 预定义动作调用
- 自定义六舵机序列动作
- 停止与复位
- Home 姿态保存
- 表情/情绪显示联动

相关实现位于：

- [main/boards/otto-robot/websocket_control_server.cc](./main/boards/otto-robot/websocket_control_server.cc)
- [main/boards/otto-robot/otto_controller.cc](./main/boards/otto-robot/otto_controller.cc)
- [main/boards/otto-robot/otto_movements.cc](./main/boards/otto-robot/otto_movements.cc)
- [main/boards/otto-robot/otto_emoji_display.cc](./main/boards/otto-robot/otto_emoji_display.cc)

当前固件中与 OpenClaw 最直接相关的动作入口是：

- `self.otto.action`

其中已经实现的典型动作包括：

- `walk`
- `turn`
- `jump`
- `swing`
- `moonwalk`
- `showcase`
- `home`
- `hand_wave`
- `greeting`
- `magic_circle`

这意味着“前进 / 后退 / 太空步”这一类 OpenClaw 指令，不需要额外新增固件协议，而是通过 `self.otto.action` 的参数组合完成。

## 固件完成判定

当该仓库按文档完成烧录并与服务端联调后，应至少满足：

1. Otto 可联网并连接 `xiaozhi-server`
2. Otto 在线时可被服务端识别为 body 设备
3. 设备侧 MCP 工具中包含 `self.otto.action`、`self.otto.stop`、`self.otto.get_status`
4. 通过服务端桥接调用 `self.otto.action(action=\"walk\")` 时，设备可执行动作

## 说明

该目录不是通用多板型仓库，不再以保留所有开发板为目标。
如果需要其它开发板，应回到原始 `xiaozhi-esp32` 仓库。
