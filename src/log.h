#pragma once
#include <string>

namespace longerdays::log
{
    // Opens (truncates) the log file. Safe to call once, before any Write.
    void Open(const std::wstring& path);
    // printf-style; each call is one timestamped, flushed line.
    void Write(const char* fmt, ...);
    void Close();
}

#define LOG(...) ::longerdays::log::Write(__VA_ARGS__)
