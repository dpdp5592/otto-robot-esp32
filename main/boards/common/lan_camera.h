#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"

class LanCamera : public Camera {
public:
    explicit LanCamera(std::string base_url);

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::string Explain(const std::string& question) override;

private:
    static std::string NormalizeBaseUrl(std::string base_url);

    std::string base_url_;
    std::string explain_url_;
    std::string explain_token_;
    std::vector<uint8_t> jpeg_data_;
};
