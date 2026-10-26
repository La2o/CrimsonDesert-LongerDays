#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace longerdays::log
{
    namespace
    {
        FILE*            g_file = nullptr;
        CRITICAL_SECTION g_lock;
    }

    void Open(const std::wstring& path)
    {
        InitializeCriticalSection(&g_lock);
        g_file = _wfopen(path.c_str(), L"w");
    }

    void Write(const char* fmt, ...)
    {
        if (!g_file) return;
        SYSTEMTIME t;
        GetLocalTime(&t);

        EnterCriticalSection(&g_lock);
        std::fprintf(g_file, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_file, fmt, args);
        va_end(args);
        std::fputc('\n', g_file);
        std::fflush(g_file);
        LeaveCriticalSection(&g_lock);
    }

    void Close()
    {
        if (!g_file) return;
        EnterCriticalSection(&g_lock);
        std::fclose(g_file);
        g_file = nullptr;
        LeaveCriticalSection(&g_lock);
    }
}
