#pragma once

// File-path helpers. Paths inside the engine are UTF-8 std::strings; on Windows the C/C++ library
// wants wide paths (a user name like "José" would otherwise break temp-file paths), so everything
// that opens a file goes through these.
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sermon {

#ifdef _WIN32
inline std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w;
  w.resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}
inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s;
  s.resize(static_cast<size_t>(n));
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}
#endif

inline std::filesystem::path pathOf(const std::string& utf8) {
#ifdef _WIN32
  return std::filesystem::path(widen(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

inline void removeFile(const std::string& utf8) {
  std::error_code ec;
  std::filesystem::remove(pathOf(utf8), ec);
}

// Command-line arguments as UTF-8 (on Windows main() receives the ANSI code page, which mangles
// non-ASCII file names).
inline std::vector<std::string> utf8Args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  for (int i = 0; i < n; i++) out.push_back(narrow(w[i]));
  LocalFree(w);
#else
  for (int i = 0; i < argc; i++) out.emplace_back(argv[i]);
#endif
  return out;
}

// Pipes carry JSON lines: no CRLF translation.
inline void binaryStdio() {
#ifdef _WIN32
  _setmode(_fileno(stdin), 0x8000);   // _O_BINARY
  _setmode(_fileno(stdout), 0x8000);
#endif
}

}  // namespace sermon

// dr_wav / dr_mp3 file opening with the right path type for the platform.
#ifdef _WIN32
#define SERMON_WAV_OPEN(w, path) drwav_init_file_w(&(w), ::sermon::widen(path).c_str(), nullptr)
#define SERMON_WAV_CREATE(w, path, fmt) drwav_init_file_write_w(&(w), ::sermon::widen(path).c_str(), &(fmt), nullptr)
#define SERMON_MP3_OPEN(m, path) drmp3_init_file_w(&(m), ::sermon::widen(path).c_str(), nullptr)
#else
#define SERMON_WAV_OPEN(w, path) drwav_init_file(&(w), (path).c_str(), nullptr)
#define SERMON_WAV_CREATE(w, path, fmt) drwav_init_file_write(&(w), (path).c_str(), &(fmt), nullptr)
#define SERMON_MP3_OPEN(m, path) drmp3_init_file(&(m), (path).c_str(), nullptr)
#endif
