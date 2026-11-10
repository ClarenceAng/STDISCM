#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// one fully received video waiting for a consumer thread
struct UploadJob {
    std::string sha256;
    std::string producerId;
    std::string fileName;
    std::filesystem::path tempPath;
    std::uint64_t sizeBytes = 0;
};
