#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <mutex>

namespace lillia {

enum LogLevel { LL_DEBUG, LL_INFO, LL_WARN, LL_ERROR };

void log_set_file(const wchar_t* path);
void log_write(LogLevel lv, const char* fmt, ...);
#define LOG_D(...) lillia::log_write(lillia::LL_DEBUG, __VA_ARGS__)
#define LOG_I(...) lillia::log_write(lillia::LL_INFO,  __VA_ARGS__)
#define LOG_W(...) lillia::log_write(lillia::LL_WARN,  __VA_ARGS__)
#define LOG_E(...) lillia::log_write(lillia::LL_ERROR, __VA_ARGS__)

}
