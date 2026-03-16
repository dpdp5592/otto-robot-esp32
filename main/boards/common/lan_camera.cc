#include "lan_camera.h"

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <stdexcept>
#include <utility>

#include "board.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "system_info.h"

namespace {
constexpr const char* kTag = "LanCamera";
constexpr size_t kReadChunkSize = 1024;
constexpr int kCaptureRetries = 3;
constexpr int kHttpTimeoutMs = 5000;

bool IsJpegDataValid(const std::vector<uint8_t>& data) {
    if (data.size() < 4) {
        return false;
    }
    return data.front() == 0xFF && data[1] == 0xD8 &&
           data[data.size() - 2] == 0xFF && data.back() == 0xD9;
}
}  // namespace

LanCamera::LanCamera(std::string base_url)
    : base_url_(NormalizeBaseUrl(std::move(base_url))) {}

std::string LanCamera::NormalizeBaseUrl(std::string base_url) {
    if (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return base_url;
}

void LanCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool LanCamera::Capture() {
    jpeg_data_.clear();
    std::string url = base_url_ + "/capture";

    for (int attempt = 1; attempt <= kCaptureRetries; ++attempt) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(kHttpTimeoutMs);
        if (!http->Open("GET", url)) {
            ESP_LOGW(kTag, "Attempt %d: failed to open URL: %s", attempt, url.c_str());
            continue;
        }

        int status_code = http->GetStatusCode();
        if (status_code != 200) {
            ESP_LOGW(kTag, "Attempt %d: unexpected status code: %d", attempt, status_code);
            http->Close();
            continue;
        }

        size_t content_length = http->GetBodyLength();
        std::vector<uint8_t> data;
        if (content_length > 0) {
            data.resize(content_length);
            size_t total_read = 0;
            while (total_read < content_length) {
                int ret = http->Read(reinterpret_cast<char*>(data.data()) + total_read,
                                     content_length - total_read);
                if (ret < 0) {
                    ESP_LOGW(kTag, "Attempt %d: read failed", attempt);
                    break;
                }
                if (ret == 0) {
                    break;
                }
                total_read += static_cast<size_t>(ret);
            }
            ESP_LOGI(kTag, "Attempt %d: read %u/%u bytes", attempt,
                     static_cast<unsigned>(total_read),
                     static_cast<unsigned>(content_length));
            data.resize(total_read);
        } else {
            uint8_t buffer[kReadChunkSize];
            while (true) {
                int ret = http->Read(reinterpret_cast<char*>(buffer), sizeof(buffer));
                if (ret < 0) {
                    ESP_LOGW(kTag, "Attempt %d: read failed (chunked)", attempt);
                    break;
                }
                if (ret == 0) {
                    break;
                }
                data.insert(data.end(), buffer, buffer + ret);
            }
            ESP_LOGI(kTag, "Attempt %d: read %u bytes (chunked)", attempt,
                     static_cast<unsigned>(data.size()));
        }

        http->Close();

        if (!data.empty() && IsJpegDataValid(data)) {
            jpeg_data_ = std::move(data);
            break;
        }

        ESP_LOGW(kTag, "Attempt %d: invalid JPEG data, retrying", attempt);
    }

    if (jpeg_data_.empty()) {
        ESP_LOGE(kTag, "Failed to capture valid JPEG after %d attempts", kCaptureRetries);
        return false;
    }

    if (auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay())) {
        void* preview_data = heap_caps_malloc(jpeg_data_.size(), MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, jpeg_data_.data(), jpeg_data_.size());
            try {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, jpeg_data_.size()));
            } catch (const std::exception& e) {
                ESP_LOGW(kTag, "Preview image decode failed: %s", e.what());
                heap_caps_free(preview_data);
            }
        } else {
            ESP_LOGW(kTag, "Failed to allocate preview image buffer");
        }
    }

    return true;
}

bool LanCamera::SetHMirror(bool enabled) {
    (void)enabled;
    return false;
}

bool LanCamera::SetVFlip(bool enabled) {
    (void)enabled;
    return false;
}

std::string LanCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }
    if (jpeg_data_.empty()) {
        throw std::runtime_error("No camera frame captured");
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");

    if (!http->Open("POST", explain_url_)) {
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    http->Write(reinterpret_cast<const char*>(jpeg_data_.data()), jpeg_data_.size());

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        http->Close();
        throw std::runtime_error("Failed to upload photo, status code: " +
                                 std::to_string(http->GetStatusCode()));
    }
    std::string result = http->ReadAll();
    http->Close();
    return result;
}
