#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace path_util {

inline std::string Utf8(const std::filesystem::path& path) {
  auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

inline std::FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(path.c_str(), wide_mode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace path_util
