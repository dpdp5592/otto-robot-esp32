# Otto 板级与 OpenClaw 联动说明

本文档说明 `otto-robot-esp32` 中保留的 Otto 板级能力，以及这些能力与 OpenClaw 联动的关系。

## 1. 板级目录

Otto 相关实现集中在：

- [main/boards/otto-robot/config.h](../main/boards/otto-robot/config.h)
- [main/boards/otto-robot/config.json](../main/boards/otto-robot/config.json)
- [main/boards/otto-robot/otto_robot.cc](../main/boards/otto-robot/otto_robot.cc)
- [main/boards/otto-robot/otto_controller.cc](../main/boards/otto-robot/otto_controller.cc)
- [main/boards/otto-robot/websocket_control_server.cc](../main/boards/otto-robot/websocket_control_server.cc)

## 2. 保留的硬件配置

当前 `otto-robot` 板级配置中保留了：

- 舵机引脚
- 麦克风与功放引脚
- LCD 引脚
- 电池检测引脚
- WebSocket 本地控制开关与端口

关键宏位于 [config.h](../main/boards/otto-robot/config.h)：

- `ROBOT_LAN_BASE_URL`
- `OTTO_WS_CONTROL_ENABLED`
- `OTTO_WS_CONTROL_PORT`
- `OTTO_WS_CONTROL_TOKEN`

## 3. 本地 WebSocket 控制端

该固件保留了 Otto 本地 WebSocket 控制端。

默认配置下：

- 地址：`ws://设备IP:8080/ws`
- 是否启用：由 `OTTO_WS_CONTROL_ENABLED` 控制
- 鉴权方式：由 `OTTO_WS_CONTROL_TOKEN` 控制

该入口主要用于：

- 本地调试 Otto 动作
- 通过 MCP/JSON-RPC 调 Otto 工具
- 与外部 OpenClaw/bridge 做局域网联动

## 4. 与 OpenClaw 联动直接相关的固件能力

当前精简工程保留了这几类能力：

- Otto 本地动作工具
- `self.otto.pose`
- `self.otto.save_home`
- 本地 WebSocket MCP 响应通道

这些能力使 Otto 可以被外部网关按工具调用方式控制，而不必把动作全部硬编码在固件外层。

## 5. 当前不在固件里直接写死的内容

该固件中没有直接写死以下 OpenClaw 运行态信息：

- OpenClaw 网关地址
- `pair_code`
- 智控台激活码

也就是说，该固件主要提供“可被控制的 Otto 本地能力”，而不负责服务端配对逻辑。

## 6. 推荐联调顺序

建议按以下顺序联调：

1. 先确认 Otto 可以独立烧录、启动、连网
2. 再确认 Otto 本地 WebSocket 控制端可访问
3. 再确认服务端 `xiaozhi-server` 链路正常
4. 最后再接 OpenClaw body bridge
