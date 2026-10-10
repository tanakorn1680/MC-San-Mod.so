#pragma once
// Test-loop logging for MCBridge. Works the same way as ProMenu's breadcrumbs, plus a running log.
//   Crumb()  last 14 steps, file rewritten on EVERY call -> survives a crash a moment later
//            -> .../files/MCBridge/last_action.txt   (previous run: previous_session.txt)
//   Log()    everything, appended, capped at 512 KB   -> .../files/MCBridge/session.log
//            (previous run: previous_session.log)
// Both also go to logcat with tag "MCBridge".  Call them on events, never once per frame.
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Diag {

static const int kLines = 14, kLen = 160;
static char g_lines[kLines][kLen];
static int  g_n = 0;
static long g_logBytes = 0;
static const long kLogCap = 512 * 1024;
static const char* kDirs[2] = {
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/MCBridge",
    "/sdcard/Android/data/com.rockstargames.gtasa/files/MCBridge",
};
static int g_dir = -1;                                   // index of the directory that works

static const char* Dir() {
    if (g_dir >= 0) return kDirs[g_dir];
    for (int d = 0; d < 2; d++) {
        mkdir(kDirs[d], 0777);
        struct stat st;
        if (stat(kDirs[d], &st) == 0 && S_ISDIR(st.st_mode)) { g_dir = d; return kDirs[d]; }
    }
    return nullptr;
}

static void Path(char* out, size_t cap, const char* file) {
    const char* d = Dir();
    snprintf(out, cap, "%s/%s", d ? d : "/data/local/tmp", file);
}

static void Echo(const char* s) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "MCBridge", "%s", s);
#else
    fprintf(stderr, "[MCBridge] %s\n", s);
#endif
}

static void Stamp(char* out, size_t cap) {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, cap, "[%7.2f] ", (double)ts.tv_sec + ts.tv_nsec * 1e-9);
}

// call once at start: the files of the previous (maybe crashed) run are kept under another name
static void KeepPreviousSession() {
    char a[256], b[256];
    Path(a, sizeof(a), "last_action.txt"); Path(b, sizeof(b), "previous_session.txt"); rename(a, b);
    Path(a, sizeof(a), "session.log");     Path(b, sizeof(b), "previous_session.log"); rename(a, b);
    g_logBytes = 0;
}

static void FlushCrumbs() {
    char path[256];
    Path(path, sizeof(path), "last_action.txt");
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    const int first = g_n > kLines ? g_n - kLines : 0;
    for (int i = first; i < g_n; i++) {
        const char* s = g_lines[i % kLines];
        if (write(fd, s, strlen(s)) < 0) break;
    }
    close(fd);
}

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    char line[kLen + 24];
    Stamp(line, sizeof(line));
    const size_t off = strlen(line);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - off - 2, fmt, ap);
    va_end(ap);
    Echo(line);
    if (g_logBytes >= kLogCap) return;
    char path[256]; Path(path, sizeof(path), "session.log");
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    const size_t n = strlen(line);
    line[n] = '\n';
    if (write(fd, line, n + 1) > 0) g_logBytes += (long)n + 1;
    close(fd);
}

static void Crumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Crumb(const char* fmt, ...) {
    char* line = g_lines[g_n % kLines];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, kLen - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > kLen - 3) n = kLen - 3;
    line[n] = '\n'; line[n + 1] = 0;
    g_n++;
    FlushCrumbs();
    Log("%.*s", n, line);                                // crumbs are also kept in the long log
}

} // namespace Diag
