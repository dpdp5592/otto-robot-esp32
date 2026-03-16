#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/ledc.h>
#include <esp_http_server.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "application.h"
#include "codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "lamp_controller.h"
#include "lan_http_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "otto_emoji_display.h"
#include "power_manager.h"
#include "settings.h"
#include "system_reset.h"
#include "wifi_board.h"
#include "esp_video.h"
#include "websocket_control_server.h"

#define TAG "OttoRobot"

extern void InitializeOttoController(const HardwareConfig& hw_config);

namespace {
constexpr size_t kUploadMaxBytes = 2 * 1024 * 1024;
constexpr size_t kRecvBufChunk = 2048;
constexpr int kImageServerPort = 80;

void* ps_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

const uint8_t* find_subseq(const uint8_t* haystack, size_t hay_len, const uint8_t* needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || hay_len < needle_len) {
        return nullptr;
    }
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return nullptr;
}

struct MultipartResult {
    const uint8_t* file_data = nullptr;
    size_t file_len = 0;
};

bool extract_boundary(const char* content_type, char* out, size_t out_len) {
    const char* b = strstr(content_type, "boundary=");
    if (!b) {
        return false;
    }
    b += strlen("boundary=");
    const char* end = nullptr;
    if (*b == '"') {
        b++;
        end = strchr(b, '"');
    } else {
        end = strpbrk(b, ";\r\n");
    }
    if (!end) {
        end = b + strlen(b);
    }
    size_t len = static_cast<size_t>(end - b);
    if (len == 0 || len >= out_len) {
        return false;
    }
    memcpy(out, b, len);
    out[len] = '\0';
    return true;
}

bool parse_multipart(const uint8_t* body, size_t len, const char* boundary, MultipartResult* out, char* err_msg, size_t err_len) {
    if (!body || !boundary || !out) {
        if (err_msg && err_len) {
            snprintf(err_msg, err_len, "invalid args");
        }
        return false;
    }

    memset(out, 0, sizeof(*out));

    char start_boundary[128];
    char inner_boundary[132];
    snprintf(start_boundary, sizeof(start_boundary), "--%s", boundary);
    snprintf(inner_boundary, sizeof(inner_boundary), "\r\n--%s", boundary);

    const uint8_t* p = find_subseq(body, len, reinterpret_cast<const uint8_t*>(start_boundary), strlen(start_boundary));
    if (!p) {
        if (err_msg && err_len) {
            snprintf(err_msg, err_len, "boundary not found");
        }
        return false;
    }
    p += strlen(start_boundary);
    if (p + 2 <= body + len && p[0] == '\r' && p[1] == '\n') {
        p += 2;
    }

    while (p < body + len) {
        const uint8_t* header_end = find_subseq(p, body + len - p, reinterpret_cast<const uint8_t*>("\r\n\r\n"), 4);
        if (!header_end) {
            break;
        }
        size_t headers_len = static_cast<size_t>(header_end - p);
        const uint8_t* data_start = header_end + 4;

        const uint8_t* next_boundary = find_subseq(data_start, body + len - data_start,
                                                  reinterpret_cast<const uint8_t*>(inner_boundary), strlen(inner_boundary));
        if (!next_boundary) {
            if (err_msg && err_len) {
                snprintf(err_msg, err_len, "next boundary not found");
            }
            return false;
        }
        size_t data_len = static_cast<size_t>(next_boundary - data_start);

        bool is_file = find_subseq(p, headers_len, reinterpret_cast<const uint8_t*>("name=\"file\""), 11) != nullptr;
        if (is_file) {
            out->file_data = data_start;
            out->file_len = data_len;
        }

        const uint8_t* boundary_start = next_boundary + 2;
        const uint8_t* after_boundary = boundary_start + 2 + strlen(boundary);
        if (after_boundary + 2 <= body + len && after_boundary[0] == '-' && after_boundary[1] == '-') {
            break;
        }
        if (after_boundary + 2 <= body + len && after_boundary[0] == '\r' && after_boundary[1] == '\n') {
            p = after_boundary + 2;
        } else {
            p = after_boundary;
        }
    }

    if (!out->file_data || out->file_len == 0) {
        if (err_msg && err_len) {
            snprintf(err_msg, err_len, "file field not found");
        }
        return false;
    }

    return true;
}

const char* http_status_string(int status_code) {
    switch (status_code) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 413:
        return "413 Payload Too Large";
    case 415:
        return "415 Unsupported Media Type";
    case 500:
    default:
        return "500 Internal Server Error";
    }
}

esp_err_t send_json(httpd_req_t* req, const char* json, int status_code) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, http_status_string(status_code));
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static const char kOttoServoDebugPage[] = R"HTML(
<!doctype html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Otto Servo Debug</title>
    <style>
      :root {
        --bg: #0f1216;
        --panel: #151a21;
        --text: #e8edf3;
        --muted: #9aa6b2;
        --accent: #3fb4ff;
        --border: #232a33;
      }
      body {
        margin: 0;
        font-family: "JetBrains Mono", "Fira Code", monospace;
        background: radial-gradient(1200px 600px at 10% 0%, #1a2230, #0f1216);
        color: var(--text);
      }
      .wrap {
        max-width: 980px;
        margin: 24px auto;
        padding: 0 16px 40px;
      }
      h1 {
        margin: 16px 0 6px;
        font-size: 20px;
      }
      .note {
        color: var(--muted);
        font-size: 12px;
      }
      .panel {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 12px;
        padding: 16px;
        margin-top: 16px;
      }
      .row {
        display: grid;
        grid-template-columns: 120px 1fr 90px 70px;
        gap: 10px;
        align-items: center;
        padding: 6px 0;
        border-bottom: 1px dashed #202734;
      }
      .row:last-child { border-bottom: none; }
      label { color: var(--muted); font-size: 12px; }
      input[type="number"], input[type="text"] {
        width: 100%;
        background: #0d1117;
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 6px 8px;
      }
      input[type="range"] { width: 100%; }
      .btn {
        background: var(--accent);
        color: #05141f;
        border: none;
        border-radius: 10px;
        padding: 8px 12px;
        font-weight: 600;
        cursor: pointer;
      }
      .btn.secondary { background: #2a3340; color: var(--text); }
      .btn.ghost { background: transparent; border: 1px solid var(--border); color: var(--text); }
      .grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 12px;
      }
      textarea {
        width: 100%;
        min-height: 140px;
        background: #0d1117;
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 8px;
        font-family: inherit;
        font-size: 12px;
      }
      .status {
        display: flex;
        gap: 8px;
        align-items: center;
      }
      .dot {
        width: 8px; height: 8px; border-radius: 50%;
        background: #c0392b;
      }
      .dot.on { background: #2ecc71; }
      .controls {
        display: flex; gap: 8px; flex-wrap: wrap;
      }
      @media (max-width: 820px) {
        .row { grid-template-columns: 90px 1fr 70px 60px; }
        .grid { grid-template-columns: 1fr; }
      }
    </style>
  </head>
  <body>
    <div class="wrap">
      <h1>Otto 六舵机调试</h1>
      <div class="note">通过设备 WebSocket (ws://设备IP:端口/ws) 发送 MCP tools/call（若启用鉴权可带 ?token=xxx）。</div>

      <div class="panel">
        <div class="controls">
          <input id="host" type="text" placeholder="设备IP，如 192.168.1.23" />
          <input id="port" type="number" value="8080" min="1" max="65535" />
          <input id="token" type="text" placeholder="可选 token" />
          <button class="btn" id="btn-connect">连接</button>
          <button class="btn secondary" id="btn-disconnect">断开</button>
          <div class="status"><span class="dot" id="dot"></span><span id="status">未连接</span></div>
        </div>
      </div>

      <div class="panel">
        <div class="controls">
          <label>速度(ms)</label>
          <input id="speed" type="number" value="800" min="100" max="3000" />
          <button class="btn" id="btn-send-pose">发送姿态</button>
          <button class="btn secondary" id="btn-save-home">保存为 Home</button>
          <button class="btn ghost" id="btn-home">回到 Home</button>
          <button class="btn ghost" id="btn-stop">停止</button>
        </div>

        <div id="servo-list" style="margin-top:10px;"></div>
      </div>

      <div class="panel grid">
        <div>
          <div class="note">自定义序列（self.otto.servo_sequences 的 sequence 字符串）</div>
          <textarea id="sequence">
{"a":[{"s":{"ll":90,"rl":90,"lf":90,"rf":90,"lh":45,"rh":135},"v":800}]}
          </textarea>
          <div class="controls" style="margin-top:8px;">
            <button class="btn" id="btn-send-seq">发送序列</button>
          </div>
        </div>
        <div>
          <div class="note">日志</div>
          <textarea id="log" readonly></textarea>
        </div>
      </div>
    </div>

    <script>
      const servos = [
        { key: "left_leg", label: "左腿 (ll)" },
        { key: "right_leg", label: "右腿 (rl)" },
        { key: "left_foot", label: "左脚 (lf)" },
        { key: "right_foot", label: "右脚 (rf)" },
        { key: "left_hand", label: "左手 (lh)" },
        { key: "right_hand", label: "右手 (rh)" },
      ];

      const list = document.getElementById("servo-list");
      const logEl = document.getElementById("log");
      const dot = document.getElementById("dot");
      const statusEl = document.getElementById("status");

      let ws = null;
      let nextId = 1;

      function log(msg) {
        logEl.value += msg + "\\n";
        logEl.scrollTop = logEl.scrollHeight;
      }

      function setStatus(connected) {
        dot.className = connected ? "dot on" : "dot";
        statusEl.textContent = connected ? "已连接" : "未连接";
      }

      function buildUI() {
        servos.forEach((s) => {
          const row = document.createElement("div");
          row.className = "row";
          row.innerHTML = `
            <label>${s.label}</label>
            <input type="range" min="0" max="180" value="90" data-key="${s.key}">
            <input type="number" min="0" max="180" value="90" data-key="${s.key}">
            <label><input type="checkbox" data-use="${s.key}" checked> 使用</label>
          `;
          list.appendChild(row);
        });

        list.querySelectorAll("input[type=range]").forEach((range) => {
          range.addEventListener("input", () => {
            const num = list.querySelector(\`input[type=number][data-key="${range.dataset.key}"]\`);
            num.value = range.value;
          });
        });
        list.querySelectorAll("input[type=number]").forEach((num) => {
          num.addEventListener("input", () => {
            const range = list.querySelector(\`input[type=range][data-key="${num.dataset.key}"]\`);
            range.value = num.value;
          });
        });
      }

      function sendTool(name, args) {
        if (!ws || ws.readyState !== 1) {
          log("[ERR] WebSocket 未连接");
          return;
        }
        const msg = {
          jsonrpc: "2.0",
          method: "tools/call",
          params: { name, arguments: args || {} },
          id: nextId++,
        };
        ws.send(JSON.stringify(msg));
        log(\`[SEND] ${name} ${JSON.stringify(args || {})}\`);
      }

      document.getElementById("btn-connect").addEventListener("click", () => {
        const host = document.getElementById("host").value.trim();
        const port = document.getElementById("port").value.trim() || "8080";
        const token = document.getElementById("token").value.trim();
        if (!host) {
          log("[ERR] 请填写设备 IP");
          return;
        }
        const tokenQuery = token ? \`?token=${encodeURIComponent(token)}\` : "";
        const url = \`ws://${host}:${port}/ws${tokenQuery}\`;
        ws = new WebSocket(url);
        ws.onopen = () => { setStatus(true); log(\`[OK] 已连接 ${url}\`); };
        ws.onclose = () => { setStatus(false); log("[INFO] 连接已关闭"); };
        ws.onerror = () => { setStatus(false); log("[ERR] 连接错误"); };
        ws.onmessage = (evt) => { log(\`[RECV] ${evt.data}\`); };
      });

      document.getElementById("btn-disconnect").addEventListener("click", () => {
        if (ws) ws.close();
      });

      document.getElementById("btn-send-pose").addEventListener("click", () => {
        const args = { speed: Number(document.getElementById("speed").value || 800) };
        servos.forEach((s) => {
          const use = list.querySelector(\`input[type=checkbox][data-use="${s.key}"]\`).checked;
          const val = Number(list.querySelector(\`input[type=number][data-key="${s.key}"]\`).value);
          args[s.key] = use ? val : -1;
        });
        sendTool("self.otto.pose", args);
      });

      document.getElementById("btn-save-home").addEventListener("click", () => {
        sendTool("self.otto.save_home", {});
      });

      document.getElementById("btn-home").addEventListener("click", () => {
        sendTool("self.otto.action", { action: "home" });
      });

      document.getElementById("btn-stop").addEventListener("click", () => {
        sendTool("self.otto.stop", {});
      });

      document.getElementById("btn-send-seq").addEventListener("click", () => {
        let text = document.getElementById("sequence").value.trim();
        if (!text) {
          log("[ERR] 序列为空");
          return;
        }
        sendTool("self.otto.servo_sequences", { sequence: text });
      });

      document.getElementById("host").value = location.hostname || "";
      buildUI();
      setStatus(false);
    </script>
  </body>
</html>
)HTML";

static esp_err_t DebugPageHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kOttoServoDebugPage, HTTPD_RESP_USE_STRLEN);
}
} // namespace

class OttoRobot : public WifiBoard {
private:
    struct WsControlConfig {
        bool enabled;
        int port;
        std::string token;
    };

    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    WebSocketControlServer* ws_control_server_;
    httpd_handle_t image_server_;
    HardwareConfig hw_config_;
    AudioCodec* audio_codec_;
    i2c_master_bus_handle_t i2c_bus_;
    EspVideo *camera_;
    bool has_camera_;
    
    bool DetectHardwareVersion() {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_2_BIT,
            .timer_num = LEDC_TIMER,
            .freq_hz = CAMERA_XCLK_FREQ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&ledc_timer);
        if (ret != ESP_OK) {
            return false;
        }
        
        ledc_channel_config_t ledc_channel = {
            .gpio_num = CAMERA_XCLK,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 2,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ledc_channel);
        if (ret != ESP_OK) {
            return false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
            .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        
        ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
            return false;
        }
        const uint8_t camera_addresses[] = {0x30, 0x3C, 0x21, 0x60};
        bool camera_found = false;
        
        for (size_t i = 0; i < sizeof(camera_addresses); i++) {
            uint8_t addr = camera_addresses[i];
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100000,
            };
            
            i2c_master_dev_handle_t dev_handle;
            ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle);
            if (ret == ESP_OK) {
                uint8_t reg_addr = 0x0A;
                uint8_t data[2];
                ret = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, 2, 200);
                if (ret == ESP_OK) {
                    camera_found = true;
                    i2c_master_bus_rm_device(dev_handle);
                    break;
                }
                i2c_master_bus_rm_device(dev_handle);
            }
        }
        
        if (!camera_found) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        }
        return camera_found;
    }
    
    void InitializePowerManager() {
        power_manager_ = new PowerManager(
            hw_config_.power_charge_detect_pin,
            hw_config_.power_adc_unit,
            hw_config_.power_adc_channel
        );
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = hw_config_.display_mosi_pin;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = hw_config_.display_clk_pin;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = hw_config_.display_cs_pin;
        io_config.dc_gpio_num = hw_config_.display_dc_pin;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = hw_config_.display_rst_pin;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new OttoEmojiDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeOttoController() {
        ::InitializeOttoController(hw_config_);
    }

    void InitializeTools() {
        static LanHttpController robot(ROBOT_LAN_BASE_URL);
    }
    
public:
    const HardwareConfig& GetHardwareConfig() const {
        return hw_config_;
    }
    
private:

    WsControlConfig LoadWsControlConfig() {
        WsControlConfig cfg;
        cfg.enabled = OTTO_WS_CONTROL_ENABLED != 0;
        cfg.port = OTTO_WS_CONTROL_PORT;
        cfg.token = OTTO_WS_CONTROL_TOKEN;

        Settings settings("otto_ws_control", false);
        cfg.enabled = settings.GetBool("enabled", cfg.enabled);

        int configured_port = settings.GetInt("port", cfg.port);
        if (configured_port > 0 && configured_port <= 65535) {
            cfg.port = configured_port;
        } else {
            ESP_LOGW(TAG, "Invalid otto_ws_control.port=%d, fallback to %d", configured_port, cfg.port);
        }

        cfg.token = settings.GetString("token", cfg.token);
        return cfg;
    }

    void InitializeWebSocketControlServer() {
        WsControlConfig cfg = LoadWsControlConfig();
        if (!cfg.enabled) {
            ESP_LOGI(TAG, "WebSocket control server disabled by config");
            return;
        }

        ws_control_server_ = new WebSocketControlServer(cfg.token);
        if (!ws_control_server_->Start(cfg.port)) {
            delete ws_control_server_;
            ws_control_server_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "WebSocket control server ready at ws://<device_ip>:%d/ws%s",
            cfg.port, cfg.token.empty() ? "" : " (token required)");
    }

    static esp_err_t UploadHandler(httpd_req_t* req) {
        auto* board = static_cast<OttoRobot*>(req->user_ctx);
        if (!board) {
            return ESP_FAIL;
        }
        return board->HandleUpload(req);
    }

    esp_err_t HandleUpload(httpd_req_t* req) {
        if (req->content_len <= 0) {
            return send_json(req, "{\"success\":false,\"message\":\"empty body\"}", 400);
        }
        if (req->content_len > static_cast<int>(kUploadMaxBytes)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "{\"success\":false,\"message\":\"file too large, max %u KB\"}",
                     static_cast<unsigned>(kUploadMaxBytes / 1024));
            return send_json(req, msg, 413);
        }

        size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
        if (ct_len == 0) {
            return send_json(req, "{\"success\":false,\"message\":\"missing Content-Type\"}", 400);
        }

        char* content_type = static_cast<char*>(malloc(ct_len + 1));
        if (!content_type) {
            return send_json(req, "{\"success\":false,\"message\":\"no memory\"}", 500);
        }
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, ct_len + 1) != ESP_OK) {
            free(content_type);
            return send_json(req, "{\"success\":false,\"message\":\"read Content-Type failed\"}", 400);
        }

        uint8_t* body = static_cast<uint8_t*>(ps_malloc(req->content_len));
        if (!body) {
            free(content_type);
            return send_json(req, "{\"success\":false,\"message\":\"no memory\"}", 500);
        }

        size_t received = 0;
        size_t remaining = req->content_len;
        while (remaining > 0) {
            int to_read = (remaining > kRecvBufChunk) ? kRecvBufChunk : static_cast<int>(remaining);
            int ret = httpd_req_recv(req, reinterpret_cast<char*>(body) + received, to_read);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (ret <= 0) {
                heap_caps_free(body);
                free(content_type);
                return send_json(req, "{\"success\":false,\"message\":\"recv failed\"}", 500);
            }
            received += static_cast<size_t>(ret);
            remaining -= static_cast<size_t>(ret);
        }

        const uint8_t* file_data = body;
        size_t file_len = req->content_len;

        if (strstr(content_type, "multipart/form-data") != nullptr) {
            char boundary[96] = {0};
            char parse_err[64] = {0};
            if (!extract_boundary(content_type, boundary, sizeof(boundary))) {
                heap_caps_free(body);
                free(content_type);
                return send_json(req, "{\"success\":false,\"message\":\"invalid multipart boundary\"}", 400);
            }
            MultipartResult mp;
            if (!parse_multipart(body, req->content_len, boundary, &mp, parse_err, sizeof(parse_err))) {
                char msg[160];
                snprintf(msg, sizeof(msg), "{\"success\":false,\"message\":\"multipart parse failed: %s\"}", parse_err);
                heap_caps_free(body);
                free(content_type);
                return send_json(req, msg, 400);
            }
            file_data = mp.file_data;
            file_len = mp.file_len;
        } else if (strstr(content_type, "image/") == nullptr) {
            heap_caps_free(body);
            free(content_type);
            return send_json(req, "{\"success\":false,\"message\":\"unsupported Content-Type\"}", 415);
        }

        free(content_type);

        if (file_len < 4) {
            heap_caps_free(body);
            return send_json(req, "{\"success\":false,\"message\":\"empty file\"}", 400);
        }

        bool is_jpeg = (file_len > 3 && file_data[0] == 0xFF && file_data[1] == 0xD8 && file_data[2] == 0xFF);
        bool is_png = (file_len > 7 && file_data[0] == 0x89 && file_data[1] == 0x50 && file_data[2] == 0x4E &&
                       file_data[3] == 0x47);
        if (!is_jpeg && !is_png) {
            heap_caps_free(body);
            return send_json(req, "{\"success\":false,\"message\":\"unsupported image format\"}", 415);
        }

        uint8_t* img_buf = static_cast<uint8_t*>(ps_malloc(file_len));
        if (!img_buf) {
            heap_caps_free(body);
            return send_json(req, "{\"success\":false,\"message\":\"no memory\"}", 500);
        }
        memcpy(img_buf, file_data, file_len);
        heap_caps_free(body);

        if (!display_) {
            heap_caps_free(img_buf);
            return send_json(req, "{\"success\":false,\"message\":\"display not ready\"}", 500);
        }

        try {
            auto image = std::make_unique<LvglAllocatedImage>(img_buf, file_len);
            display_->SetPreviewImage(std::move(image));
        } catch (...) {
            heap_caps_free(img_buf);
            return send_json(req, "{\"success\":false,\"message\":\"decode failed\"}", 415);
        }

        return send_json(req, "{\"success\":true,\"message\":\"image displayed\"}", 200);
    }

    void StartImageUploadServer() {
        if (image_server_ != nullptr) {
            return;
        }
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = kImageServerPort;
        config.stack_size = 8192;
        config.max_uri_handlers = 4;
        config.recv_wait_timeout = 5;
        config.send_wait_timeout = 5;

        if (httpd_start(&image_server_, &config) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start image upload server");
            image_server_ = nullptr;
            return;
        }

        httpd_uri_t upload_uri = {};
        upload_uri.uri = "/upload";
        upload_uri.method = HTTP_POST;
        upload_uri.handler = UploadHandler;
        upload_uri.user_ctx = this;
        httpd_register_uri_handler(image_server_, &upload_uri);

        httpd_uri_t root_uri = {};
        root_uri.uri = "/";
        root_uri.method = HTTP_GET;
        root_uri.handler = DebugPageHandler;
        root_uri.user_ctx = this;
        httpd_register_uri_handler(image_server_, &root_uri);

        httpd_uri_t otto_uri = {};
        otto_uri.uri = "/otto";
        otto_uri.method = HTTP_GET;
        otto_uri.handler = DebugPageHandler;
        otto_uri.user_ctx = this;
        httpd_register_uri_handler(image_server_, &otto_uri);

        ESP_LOGI(TAG, "Image upload server started on port %d", config.server_port);
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        InitializeWebSocketControlServer();
        StartImageUploadServer();
    }

    bool InitializeCamera() {
        if (!has_camera_ || i2c_bus_ == nullptr) {
            return false;
        }
        
        try {
            static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
                .data_width = CAM_CTLR_DATA_WIDTH_8,
                .data_io = {
                    [0] = CAMERA_D0,
                    [1] = CAMERA_D1,
                    [2] = CAMERA_D2,
                    [3] = CAMERA_D3,
                    [4] = CAMERA_D4,
                    [5] = CAMERA_D5,
                    [6] = CAMERA_D6,
                    [7] = CAMERA_D7,
                },
                .vsync_io = CAMERA_VSYNC,
                .de_io = CAMERA_HSYNC,
                .pclk_io = CAMERA_PCLK,
                .xclk_io = CAMERA_XCLK,
            };

            esp_video_init_sccb_config_t sccb_config = {
                .init_sccb = false,
                .i2c_handle = i2c_bus_,
                .freq = 100000,
            };

            esp_video_init_dvp_config_t dvp_config = {
                .sccb_config = sccb_config,
                .reset_pin = CAMERA_RESET,
                .pwdn_pin = CAMERA_PWDN,
                .dvp_pin = dvp_pin_config,
                .xclk_freq = CAMERA_XCLK_FREQ,
            };

            esp_video_init_config_t video_config = {
                .dvp = &dvp_config,
            };

            camera_ = new EspVideo(video_config);
            camera_->SetVFlip(true);
            return true;
        } catch (...) {
            camera_ = nullptr;
            return false;
        }
    }
    
    void InitializeAudioCodec() {
        if (hw_config_.audio_use_simplex) {
            audio_codec_ = new NoAudioCodecSimplex(
                hw_config_.audio_input_sample_rate,
                hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_spk_gpio_bclk,
                hw_config_.audio_i2s_spk_gpio_lrck,
                hw_config_.audio_i2s_spk_gpio_dout,
                hw_config_.audio_i2s_mic_gpio_sck,
                hw_config_.audio_i2s_mic_gpio_ws,
                hw_config_.audio_i2s_mic_gpio_din
            );
        } else {
            audio_codec_ = new NoAudioCodecDuplex(
                hw_config_.audio_input_sample_rate,
                hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_gpio_bclk,
                hw_config_.audio_i2s_gpio_ws,
                hw_config_.audio_i2s_gpio_dout,
                hw_config_.audio_i2s_gpio_din
            );
        }
    }

public:
    OttoRobot() : boot_button_(BOOT_BUTTON_GPIO),
                  image_server_(nullptr),
                  audio_codec_(nullptr),
                  i2c_bus_(nullptr),
                  camera_(nullptr),
                  has_camera_(false) {
        
        // 没有摄像头时禁用检测，避免启动时占用/干扰引脚
        has_camera_ = false;
        
        if (has_camera_) 
            hw_config_ = CAMERA_VERSION_CONFIG;
        else 
            hw_config_ = NON_CAMERA_VERSION_CONFIG;
        
        
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeAudioCodec();
        InitializeTools();
        
        if (has_camera_) {
            if (!InitializeCamera()) {
                has_camera_ = false;
            }
        }
        
        InitializeOttoController();
        ws_control_server_ = nullptr;
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec *GetAudioCodec() override {
        return audio_codec_;
    }

    virtual Display* GetDisplay() override { 
        return display_; 
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight* backlight = nullptr;
        if (backlight == nullptr) {
            backlight = new PwmBacklight(hw_config_.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        }
        return backlight;
    }
    
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual Camera *GetCamera() override { 
        return has_camera_ ? camera_ : nullptr; 
    }
};

DECLARE_BOARD(OttoRobot);
