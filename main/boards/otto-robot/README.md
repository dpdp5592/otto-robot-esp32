# otto-robot

该目录包含 `otto-robot` 板级实现。

## 包含内容

- 舵机动作控制
- Otto MCP 工具
- 本地 WebSocket 控制端
- Otto 表情显示
- 电源管理与板级引脚配置

## 关键配置

- [config.h](./config.h)
  - 舵机引脚
  - 音频引脚
  - LCD 引脚
  - `ROBOT_LAN_BASE_URL`
  - `OTTO_WS_CONTROL_ENABLED`
  - `OTTO_WS_CONTROL_PORT`
  - `OTTO_WS_CONTROL_TOKEN`
- [config.json](./config.json)
  - 当前目标芯片：`esp32s3`
  - 当前构建名：`otto-robot`

## 烧录参考

```bash
cd /path/to/otto-robot-esp32
idf.py set-target esp32s3
idf.py fullclean
idf.py menuconfig
idf.py -DBOARD_NAME=otto-robot build
idf.py -p /dev/ttyACM0 flash monitor
```

在 `menuconfig` 中应确认：

- `Xiaozhi Assistant -> Board Type -> ottoRobot`
- `Xiaozhi Assistant -> Default OTA URL -> 实际 OTA 地址`

## 与 OpenClaw 相关的保留项

该目录保留了 Otto 当前与 OpenClaw 联动所需的板级能力：

- `ws://设备IP:8080/ws` 控制入口
- token 鉴权支持
- `self.otto.pose`
- `self.otto.save_home`
- 基于 MCP 的动作调用与响应
