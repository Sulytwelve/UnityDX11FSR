#include "common.h"

namespace lillia {

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static std::mutex g_logMtx;

void log_set_file(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile != INVALID_HANDLE_VALUE) { CloseHandle(g_logFile); g_logFile = INVALID_HANDLE_VALUE; }
    if (path) {
        g_logFile = CreateFileW(path, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile != INVALID_HANDLE_VALUE)
            SetFilePointer(g_logFile, 0, nullptr, FILE_END);
    }
}

static const char* lvlStr(LogLevel lv) {
    switch (lv) {
    case LL_DEBUG: return "DEBUG";
    case LL_INFO:  return "INFO ";
    case LL_WARN:  return "WARN ";
    default:       return "ERROR";
    }
}

void log_write(LogLevel lv, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char head[64];
    int hn = sprintf_s(head, "[%02d:%02d:%02d.%03d][%s] ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, lvlStr(lv));
    char body[2048];
    va_list ap; va_start(ap, fmt);
    int bn = vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (bn < 0) bn = 0;
    DWORD written = 0;
    if (hn > 0) WriteFile(g_logFile, head, hn, &written, nullptr);
    if (bn > 0) WriteFile(g_logFile, body, bn, &written, nullptr);
    WriteFile(g_logFile, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(g_logFile);
}

}
