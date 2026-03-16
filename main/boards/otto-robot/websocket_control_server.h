#ifndef WEBSOCKET_CONTROL_SERVER_H
#define WEBSOCKET_CONTROL_SERVER_H

#include <esp_http_server.h>
#include <cJSON.h>
#include <string>
#include <map>

class WebSocketControlServer {
public:
    explicit WebSocketControlServer(const std::string& auth_token = "");
    ~WebSocketControlServer();

    bool Start(int port = 8080);
    
    void Stop();

    size_t GetClientCount() const;

private:
    httpd_handle_t server_handle_;
    std::map<int, httpd_req_t*> clients_;
    std::string auth_token_;

    static esp_err_t ws_handler(httpd_req_t *req);
    
    void HandleMessage(httpd_req_t *req, const char* data, size_t len);
    void AddClient(httpd_req_t *req);
    void RemoveClient(httpd_req_t *req);
    bool IsAuthorized(httpd_req_t* req) const;
    bool SendTextToClient(int sock_fd, const std::string& message) const;
    static void AsyncSendTask(void* arg);

    struct AsyncSendContext {
        httpd_handle_t server_handle;
        int sock_fd;
        std::string message;
    };

    static WebSocketControlServer* instance_;
};

#endif // WEBSOCKET_CONTROL_SERVER_H
