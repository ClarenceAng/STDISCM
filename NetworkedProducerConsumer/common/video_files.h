#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>

// the file types a producer will upload
inline bool isVideoFile(const std::filesystem::path& path) {
    static const std::set<std::string> extensions = {".mp4", ".m4v", ".mov", ".mkv", ".webm", ".avi",
                                                     ".wmv", ".flv", ".mpg", ".mpeg", ".ts", ".3gp"};
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extensions.count(extension) > 0;
}
