#pragma once
// Minecraft-side network client of the GTA x Minecraft bridge.  Only POSIX + pthread (no preloader / game headers), so it
// can be tested on any Linux box against the GTA-side server in ../../mod/src/bridge_net.h (see tools/test_bridge_e2e.cpp).
//
// Protocol (text, one message per line, see bridge_net.h for the full list):
//   MC -> GTA   ID minecraft <version>                       first line after every (re)connect: who is on this end
//               E 0 player <x> <y> <z> <yaw>                 local player, at most ~10 Hz and only when it changed
//               S <free text>                                 status of this side (hook installed? ticks? ...), every ~2 s
//   GTA -> MC   HELLO <name> <version> | P <seq> <x> <y> <z> | PONG | ERR ...
// The network thread owns the socket.  The game thread only calls Publish() / SetStatus() (a mutex-protected copy), so a
// slow or dead GTA process can never stall Minecraft's tick.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace McNet {

static const int kLine = 192;

struct Stats {
    int connected;                       // 1 while a GTA bridge is connected
    uint64_t connects, tx, rx, sendFails;
    float gtaPos[3];                     // last "P" line from GTA
    uint32_t gtaSeq;
    char lastRx[kLine];                  // last line received from GTA (for the status / log)
};

static pthread_t g_thr;
static volatile int g_run = 0, g_started = 0;
static int g_port = 47800;
static char g_idLine[96];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_pos[4];                   // x y z yaw (latest published player state)
static uint32_t g_seq = 0, g_sentSeq = 0;
static char g_status[kLine - 4];
static Stats g_st;                       // guarded by g_mu except `connected`

static double NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}
static void SleepMs(int ms) {
    timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// ---- called from the game thread ------------------------------------------------------------------------------------
static void Publish(float x, float y, float z, float yaw) {
    pthread_mutex_lock(&g_mu);
    g_pos[0] = x; g_pos[1] = y; g_pos[2] = z; g_pos[3] = yaw;
    g_seq++;
    pthread_mutex_unlock(&g_mu);
}
static void SetStatus(const char* fmt, ...) {
    char tmp[sizeof(g_status)];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    pthread_mutex_lock(&g_mu);
    memcpy(g_status, tmp, sizeof(g_status));
    pthread_mutex_unlock(&g_mu);
}
static Stats Snapshot() {
    pthread_mutex_lock(&g_mu);
    Stats s = g_st;
    pthread_mutex_unlock(&g_mu);
    return s;
}

// ---- network thread -----------------------------------------------------------------------------------------------
static int Connect() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (sockaddr*)&a, sizeof(a));
    if (r != 0 && errno == EINPROGRESS) {
        pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, 1000) == 1) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            r = err ? -1 : 0;
        } else r = -1;
    }
    if (r != 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);                                       // back to blocking; sends are bounded by SO_SNDTIMEO
    timeval tv = { 0, 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static bool SendLine(int fd, const char* text) {
    char buf[kLine + 2];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    if (n <= 0) return false;
    if (n > kLine) { buf[kLine - 1] = '\n'; n = kLine; }
    int off = 0;
    while (off < n) {
        const ssize_t w = send(fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w <= 0) { pthread_mutex_lock(&g_mu); g_st.sendFails++; pthread_mutex_unlock(&g_mu); return false; }
        off += (int)w;
    }
    pthread_mutex_lock(&g_mu); g_st.tx++; pthread_mutex_unlock(&g_mu);
    return true;
}

static void OnRxLine(const char* line) {
    pthread_mutex_lock(&g_mu);
    g_st.rx++;
    snprintf(g_st.lastRx, sizeof(g_st.lastRx), "%s", line);
    if (line[0] == 'P' && line[1] == ' ') {
        unsigned seq = 0; float x, y, z;
        if (sscanf(line + 2, "%u %f %f %f", &seq, &x, &y, &z) == 4) {
            g_st.gtaPos[0] = x; g_st.gtaPos[1] = y; g_st.gtaPos[2] = z; g_st.gtaSeq = seq;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

static void* Run(void*) {
    char rx[512]; int rxn = 0;
    while (g_run) {
        const int fd = Connect();
        if (fd < 0) { for (int i = 0; i < 20 && g_run; i++) SleepMs(100); continue; }   // retry every 2 s
        pthread_mutex_lock(&g_mu); g_st.connected = 1; g_st.connects++; g_sentSeq = 0; pthread_mutex_unlock(&g_mu);
        rxn = 0;
        bool ok = SendLine(fd, g_idLine);
        double lastE = 0.0, lastS = -1e9;
        while (g_run && ok) {
            pollfd p = { fd, POLLIN, 0 };
            const int pr = poll(&p, 1, 50);
            if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
                const ssize_t r = recv(fd, rx + rxn, sizeof(rx) - 1 - (size_t)rxn, 0);
                if (r <= 0) break;                                                       // GTA side closed the socket
                rxn += (int)r;
                int start = 0;
                for (int i = 0; i < rxn; i++) {
                    if (rx[i] == '\n') { rx[i] = 0; if (i > start) OnRxLine(rx + start); start = i + 1; }
                }
                if (start > 0) { memmove(rx, rx + start, (size_t)(rxn - start)); rxn -= start; }
                if (rxn >= (int)sizeof(rx) - 1) rxn = 0;                                 // garbage without newline: drop it
            }
            const double now = NowMs();
            if (now - lastE >= 100.0) {
                lastE = now;
                float v[4]; uint32_t seq;
                pthread_mutex_lock(&g_mu); memcpy(v, g_pos, sizeof(v)); seq = g_seq; pthread_mutex_unlock(&g_mu);
                if (seq != 0 && seq != g_sentSeq) {
                    char line[kLine];
                    snprintf(line, sizeof(line), "E 0 player %.2f %.2f %.2f %.1f", v[0], v[1], v[2], v[3]);
                    ok = SendLine(fd, line);
                    g_sentSeq = seq;
                }
            }
            if (ok && now - lastS >= 2000.0) {
                lastS = now;
                char line[kLine], st[sizeof(g_status)];
                pthread_mutex_lock(&g_mu); memcpy(st, g_status, sizeof(st)); pthread_mutex_unlock(&g_mu);
                if (st[0]) { snprintf(line, sizeof(line), "S %s", st); ok = SendLine(fd, line); }
            }
        }
        close(fd);
        pthread_mutex_lock(&g_mu); g_st.connected = 0; pthread_mutex_unlock(&g_mu);
    }
    return nullptr;
}

// version: shown in the ID line ("ID minecraft <version>").  Safe to call once; later calls are ignored.
static bool Start(int port, const char* version) {
    if (g_started) return true;
    g_port = port;
    snprintf(g_idLine, sizeof(g_idLine), "ID minecraft %s", version);
    memset(&g_st, 0, sizeof(g_st));
    g_run = 1;
    if (pthread_create(&g_thr, nullptr, Run, nullptr) != 0) { g_run = 0; return false; }
    g_started = 1;
    return true;
}

static void Stop() {
    if (!g_started) return;
    g_run = 0;
    pthread_join(g_thr, nullptr);
    g_started = 0;
}

} // namespace McNet
