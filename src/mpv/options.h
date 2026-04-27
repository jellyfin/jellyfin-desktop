#pragma once

#include <string>
#include <vector>

constexpr const char* kHwdecDefault = "no";

inline std::vector<std::string> hwdecOptions() {
    return {
        "auto", "no",
#ifdef __linux__
        "vaapi", "nvdec", "vulkan",
#endif
#ifdef _WIN32
        "d3d11va", "nvdec", "vulkan",
#endif
#ifdef __APPLE__
        "videotoolbox", "vulkan",
#endif
    };
}

inline bool isValidHwdec(const std::string& value) {
    for (const auto& o : hwdecOptions())
        if (o == value) return true;
    return false;
}
