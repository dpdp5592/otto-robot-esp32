#ifndef __LAN_HTTP_CONTROLLER_H__
#define __LAN_HTTP_CONTROLLER_H__

#include <memory>
#include <string>
#include <utility>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio_codec.h"
#include "board.h"
#include "mcp_server.h"

#ifndef ROBOT_ACTION_DELAY_MS
#define ROBOT_ACTION_DELAY_MS 0
#endif

#ifndef ROBOT_DANCE_DELAY_MS
#define ROBOT_DANCE_DELAY_MS ROBOT_ACTION_DELAY_MS
#endif

#ifndef ROBOT_OBSTACLE_DELAY_MS
#define ROBOT_OBSTACLE_DELAY_MS 0
#endif

#ifndef ROBOT_SET_SPEED_DELAY_MS
#define ROBOT_SET_SPEED_DELAY_MS 0
#endif

#ifndef ROBOT_ACTION_MUTE_OUTPUT
#define ROBOT_ACTION_MUTE_OUTPUT 1
#endif

#ifndef ROBOT_ACTION_MUTE_INPUT
#define ROBOT_ACTION_MUTE_INPUT 0
#endif

class LanHttpController {
private:
    static constexpr int kDefaultTimeoutMs = 2000;
    std::string base_url_;

    static const char* Tag() {
        return "LanHttpController";
    }

    static std::string NormalizeBaseUrl(std::string base_url) {
        if (!base_url.empty() && base_url.back() == '/') {
            base_url.pop_back();
        }
        return base_url;
    }

    struct HttpTaskContext {
        std::string url;
        int timeout_ms;
    };

    static void HttpGetTask(void* arg) {
        std::unique_ptr<HttpTaskContext> ctx(static_cast<HttpTaskContext*>(arg));
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(ctx->timeout_ms);
        if (!http->Open("GET", ctx->url)) {
            ESP_LOGW(Tag(), "HTTP open failed: %s", ctx->url.c_str());
            vTaskDelete(nullptr);
            return;
        }
        int status_code = http->GetStatusCode();
        http->Close();
        if (status_code != 200) {
            ESP_LOGW(Tag(), "HTTP status %d for %s", status_code, ctx->url.c_str());
        }
        vTaskDelete(nullptr);
    }

    bool EnqueueGet(const std::string& path, int timeout_ms = kDefaultTimeoutMs) const {
        auto* ctx = new HttpTaskContext{base_url_ + path, timeout_ms};
        if (xTaskCreate(HttpGetTask, "lan_http_get", 4096, ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
            delete ctx;
            return false;
        }
        return true;
    }

    struct AudioHoldTaskContext {
        int delay_ms;
        bool prev_out;
        bool prev_in;
    };

    static void AudioHoldTask(void* arg) {
        std::unique_ptr<AudioHoldTaskContext> ctx(static_cast<AudioHoldTaskContext*>(arg));
        vTaskDelay(pdMS_TO_TICKS(ctx->delay_ms));
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            if (ROBOT_ACTION_MUTE_INPUT) {
                codec->EnableInput(ctx->prev_in);
            }
            if (ROBOT_ACTION_MUTE_OUTPUT) {
                codec->EnableOutput(ctx->prev_out);
            }
        }
        vTaskDelete(nullptr);
    }

    void TriggerAudioHold(int delay_ms) const {
        if (delay_ms <= 0) {
            return;
        }
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        bool prev_out = codec->output_enabled();
        bool prev_in = codec->input_enabled();
        if (ROBOT_ACTION_MUTE_OUTPUT) {
            codec->EnableOutput(false);
        }
        if (ROBOT_ACTION_MUTE_INPUT) {
            codec->EnableInput(false);
        }

        auto* ctx = new AudioHoldTaskContext{delay_ms, prev_out, prev_in};
        if (xTaskCreate(AudioHoldTask, "audio_hold", 4096, ctx, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
            delete ctx;
            if (ROBOT_ACTION_MUTE_INPUT) {
                codec->EnableInput(prev_in);
            }
            if (ROBOT_ACTION_MUTE_OUTPUT) {
                codec->EnableOutput(prev_out);
            }
        }
    }

    ReturnValue ActionWithDelay(const std::string& path, int delay_ms) const {
        bool ok = EnqueueGet(path);
        if (ok) {
            TriggerAudioHold(delay_ms);
        }
        return ok;
    }

public:
    explicit LanHttpController(std::string base_url)
        : base_url_(NormalizeBaseUrl(std::move(base_url))) {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.robot.obstacle_enable", "开启避障模式。触发词示例：避障/开启避障/进入避障模式", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/obstacle/enable", ROBOT_OBSTACLE_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.obstacle_disable", "关闭避障模式。触发词示例：关闭避障/退出避障/手动模式", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/obstacle/disable", ROBOT_OBSTACLE_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.forward", "小车前进（双电机）。触发词示例：小车前进/向前走/往前走/前进", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/both/cw", ROBOT_ACTION_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.backward", "小车后退（双电机）。触发词示例：小车后退/往后走/后退", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/both/ccw", ROBOT_ACTION_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.stop", "小车停止（双电机急停）。触发词示例：停止/急停/停下/别动", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/both/stop", ROBOT_ACTION_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.dance", "舵机舞动/招手。触发词示例：跳舞/舞动/招手", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ActionWithDelay("/servo/dance", ROBOT_DANCE_DELAY_MS);
            });

        mcp_server.AddTool("self.robot.set_speed", "设置速度百分比（0-100）。触发词示例：速度60/设置速度60/速度调到60",
            PropertyList({Property("value", kPropertyTypeInteger, 60, 0, 100)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int value = properties["value"].value<int>();
                return ActionWithDelay("/speed?value=" + std::to_string(value), ROBOT_SET_SPEED_DELAY_MS);
            });
    }
};

#endif // __LAN_HTTP_CONTROLLER_H__
