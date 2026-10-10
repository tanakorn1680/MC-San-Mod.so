// Used only when AML's own mod/logger.cpp is not available in the build.
// Implements exactly the three Logger calls ProMenu uses, on top of Android logcat.
#include <mod/logger.h>
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>

static char g_tag[48] = "ProMenu";
static char g_storage[256];                      // methods below never touch `this`
Logger* logger = reinterpret_cast<Logger*>(g_storage);

void Logger::SetTag(const char* tag)
{
    snprintf(g_tag, sizeof(g_tag), "%s", tag ? tag : "ProMenu");
}

void Logger::Info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, g_tag, fmt, args);
    va_end(args);
}

void Logger::Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, g_tag, fmt, args);
    va_end(args);
}
