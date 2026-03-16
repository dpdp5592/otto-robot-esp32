#include "websocket_control_server.h"
#include "mcp_server.h"
#include "sdkconfig.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <sys/param.h>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>

static const char* TAG = "WSControl";

WebSocketControlServer* WebSocketControlServer::instance_ = nullptr;

WebSocketControlServer::WebSocketControlServer(const std::string& auth_token)
    : server_handle_(nullptr), auth_token_(auth_token) {
    instance_ = this;
}

WebSocketControlServer::~WebSocketControlServer() {
    Stop();
    instance_ = nullptr;
}

#if defined(CONFIG_HTTPD_WS_SUPPORT)
esp_err_t WebSocketControlServer::ws_handler(httpd_req_t *req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    
    if (req->method == HTTP_GET) {
        if (!instance_->IsAuthorized(req)) {
            ESP_LOGW(TAG, "Rejected unauthorized websocket client");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        instance_->AddClient(req);
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        instance_->RemoveClient(req);
        free(buf);
        return ESP_OK;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0 && buf != nullptr) {
            buf[ws_pkt.len] = '\0';
            instance_->HandleMessage(req, (const char*)buf, ws_pkt.len);
        }
    } else {
        ESP_LOGW(TAG, "Unsupported frame type: %d", ws_pkt.type);
    }
    
    free(buf);
    return ESP_OK;
}
#else
esp_err_t WebSocketControlServer::ws_handler(httpd_req_t *req) {
    (void)req;
    ESP_LOGW(TAG, "WebSocket support is disabled (CONFIG_HTTPD_WS_SUPPORT not set)");
    return ESP_FAIL;
}
#endif

#if defined(CONFIG_HTTPD_WS_SUPPORT)
void WebSocketControlServer::AsyncSendTask(void* arg) {
    std::unique_ptr<AsyncSendContext> ctx(static_cast<AsyncSendContext*>(arg));
    if (!ctx) {
        return;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(ctx->message.data()));
    frame.len = ctx->message.size();

    esp_err_t ret = httpd_ws_send_frame_async(ctx->server_handle, ctx->sock_fd, &frame);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send websocket response to fd=%d, err=%d", ctx->sock_fd, ret);
    }
}

bool WebSocketControlServer::IsAuthorized(httpd_req_t* req) const {
    if (auth_token_.empty()) {
        return true;
    }

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0 && query_len < 512) {
        std::string query(query_len + 1, '\0');
        if (httpd_req_get_url_query_str(req, query.data(), query.size()) == ESP_OK) {
            char token_value[256] = {0};
            if (httpd_query_key_value(query.c_str(), "token", token_value, sizeof(token_value)) == ESP_OK) {
                if (auth_token_ == token_value) {
                    return true;
                }
            }
        }
    }

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len > 0 && auth_len < 512) {
        std::string auth_value(auth_len + 1, '\0');
        if (httpd_req_get_hdr_value_str(req, "Authorization", auth_value.data(), auth_value.size()) == ESP_OK) {
            while (!auth_value.empty() && auth_value.back() == '\0') {
                auth_value.pop_back();
            }
            if (auth_value == auth_token_ || auth_value == ("Bearer " + auth_token_)) {
                return true;
            }
        }
    }

    return false;
}

bool WebSocketControlServer::SendTextToClient(int sock_fd, const std::string& message) const {
    if (server_handle_ == nullptr) {
        return false;
    }

    auto* context = new AsyncSendContext;
    context->server_handle = server_handle_;
    context->sock_fd = sock_fd;
    context->message = message;

    esp_err_t ret = httpd_queue_work(server_handle_, &WebSocketControlServer::AsyncSendTask, context);
    if (ret != ESP_OK) {
        delete context;
        ESP_LOGW(TAG, "Failed to queue websocket response, err=%d", ret);
        return false;
    }
    return true;
}
#else
void WebSocketControlServer::AsyncSendTask(void* arg) {
    (void)arg;
}

bool WebSocketControlServer::IsAuthorized(httpd_req_t* req) const {
    (void)req;
    return false;
}

bool WebSocketControlServer::SendTextToClient(int sock_fd, const std::string& message) const {
    (void)sock_fd;
    (void)message;
    return false;
}
#endif

bool WebSocketControlServer::Start(int port) {
#if defined(CONFIG_HTTPD_WS_SUPPORT)
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;

    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = ws_handler;
    ws_uri.user_ctx = nullptr;
    ws_uri.is_websocket = true;

    if (httpd_start(&server_handle_, &config) == ESP_OK) {
        httpd_register_uri_handler(server_handle_, &ws_uri);
        ESP_LOGI(TAG, "WebSocket server started on port %d", port);
        return true;
    }

    ESP_LOGE(TAG, "Failed to start WebSocket server");
    return false;
#else
    (void)port;
    ESP_LOGW(TAG, "WebSocket support is disabled (CONFIG_HTTPD_WS_SUPPORT not set)");
    return false;
#endif
}

void WebSocketControlServer::Stop() {
    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        clients_.clear();
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

void WebSocketControlServer::HandleMessage(httpd_req_t *req, const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        ESP_LOGE(TAG, "Invalid message: data is null or len is 0");
        return;
    }
    
    if (len > 4096) {
        ESP_LOGE(TAG, "Message too long: %zu bytes", len);
        return;
    }
    
    char* temp_buf = (char*)malloc(len + 1);
    if (temp_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memcpy(temp_buf, data, len);
    temp_buf[len] = '\0';
    
    cJSON* root = cJSON_Parse(temp_buf);
    free(temp_buf);
    
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    // 支持两种格式：
    // 1. 完整格式：{"type":"mcp","payload":{...}}
    // 2. 简化格式：直接是MCP payload对象
    
    cJSON* payload = nullptr;
    cJSON* type = cJSON_GetObjectItem(root, "type");
    
    int sock_fd = httpd_req_to_sockfd(req);
    auto reply_callback = [this, sock_fd](const std::string& payload) {
        if (!SendTextToClient(sock_fd, payload)) {
            ESP_LOGW(TAG, "Failed to deliver MCP response to fd=%d", sock_fd);
        }
    };

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        payload = cJSON_GetObjectItem(root, "payload");
        if (payload != nullptr) {
            cJSON_DetachItemViaPointer(root, payload);
            McpServer::GetInstance().ParseMessage(payload, reply_callback);
            cJSON_Delete(payload); 
        }
    } else {
        payload = cJSON_Duplicate(root, 1);
        if (payload != nullptr) {
            McpServer::GetInstance().ParseMessage(payload, reply_callback);
            cJSON_Delete(payload);
        }
    }
    
    if (payload == nullptr) {
        ESP_LOGE(TAG, "Invalid message format or failed to parse");
    }

    cJSON_Delete(root);
}

void WebSocketControlServer::AddClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    if (clients_.find(sock_fd) == clients_.end()) {
        clients_[sock_fd] = req;
        ESP_LOGI(TAG, "Client connected: %d (total: %zu)", sock_fd, clients_.size());
    }
}

void WebSocketControlServer::RemoveClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    clients_.erase(sock_fd);
    ESP_LOGI(TAG, "Client disconnected: %d (total: %zu)", sock_fd, clients_.size());
}

size_t WebSocketControlServer::GetClientCount() const {
    return clients_.size();
}
