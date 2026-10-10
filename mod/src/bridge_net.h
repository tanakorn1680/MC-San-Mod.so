#pragma once
// Loopback TCP bridge between the GTA SA process (this plugin) and the Minecraft process.
// Only POSIX + pthread: no AML / game headers, so it can be tested on any Linux box.
//
// Text protocol, one message per line ('\n'), max kLine-1 chars:
//   GTA -> MC   HELLO <name> <version>            sent once per new client
//               P <seq> <x> <y> <z>               player position, ~10 Hz, only when it changed
//               + any line the plugin queues with Say()   (PONG, OK ..., ERR ...)
//   MC  -> GTA  PING | SPAWN_PED | SPAWN_OBJ <model name>     (test commands)
//               E <id> <type> <x> <y> <z> <yaw>   entity state   (DRAFT, not wired to the game yet)
//               B <x> <y> <z> <blockId>           block change   (DRAFT, not wired to the game yet)
// Threads: one network thread (owns the sockets) + the game thread (SetPlayer / PopInbox / Say).
// The two rings are single-producer / single-consumer with release/acquire indexes.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace Bridge {

static const int kLine = 192;

template <int N> struct Ring {
    char buf[N][kLine];
    int head = 0, tail = 0;                       // consumer reads head, producer writes tail
    bool Push(const char* s) {
        const int t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        const int nx = (t + 1) % N;
        if (nx == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) return false;      // full
        snprintf(buf[t], kLine, "%s", s);
        __atomic_store_n(&tail, nx, __ATOMIC_RELEASE);
        return true;
    }
    bool Pop(char* out) {
        const int h = __atomic_load_n(&head, __ATOMIC_RELAXED);
        if (h == __atomic_load_n(&tail, __ATOMIC_ACQUIRE)) return false;       // empty
        memcpy(out, buf[h], kLine);
        __atomic_store_n(&head, (h + 1) % N, __ATOMIC_RELEASE);
        return true;
    }
};

static Ring<128> g_in;                            // MC -> GTA lines (network thread pushes, game thread pops)
static Ring<128> g_out;                           // GTA -> MC lines (game thread pushes, network thread sends)

static int g_port = 0, g_lfd = -1, g_cfd = -1;
static volatile int g_run = 0;
static pthread_t g_th;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_pos[3] = {0, 0, 0};
static bool g_posDirty = false;
static unsigned g_seq = 0;
static char g_rx[2048];
static int g_rxLen = 0;

// counters (written by the network thread, read for logging only)
static unsigned long g_rxLines = 0, g_txLines = 0, g_dropped = 0, g_accepted = 0;
static int g_bindErrno = 0;

static long long NowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void CloseClient() { if (g_cfd >= 0) { close(g_cfd); g_cfd = -1; } g_rxLen = 0; }

static bool SendLine(const char* s) {
    if (g_cfd < 0) return false;
    char b[kLine + 2];
    const int n = snprintf(b, sizeof(b), "%s\n", s);
    int off = 0;
    while (off < n) {
        const ssize_t w = send(g_cfd, b + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;  // slow client: drop the line
        if (w < 0 && errno == EINTR) continue;
        CloseClient(); return false;
    }
    g_txLines++;
    return true;
}

static bool OpenListener() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { g_bindErrno = errno; return false; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);                 // loopback only: nothing outside this phone can connect
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) { g_bindErrno = errno; close(fd); return false; }
    g_bindErrno = 0;
    g_lfd = fd;
    return true;
}

static void SplitLines() {
    int start = 0;
    for (int i = 0; i < g_rxLen; i++) {
        if (g_rx[i] != '\n') continue;
        int len = i - start;
        if (len > 0 && g_rx[start + len - 1] == '\r') len--;
        if (len > 0) {
            char line[kLine];
            if (len > kLine - 1) len = kLine - 1;
            memcpy(line, g_rx + start, len); line[len] = 0;
            if (g_in.Push(line)) g_rxLines++; else g_dropped++;
        }
        start = i + 1;
    }
    if (start > 0) { memmove(g_rx, g_rx + start, g_rxLen - start); g_rxLen -= start; }
    if (g_rxLen >= (int)sizeof(g_rx) - 1) { g_rxLen = 0; g_dropped++; }        // endless line: throw it away
}

static void* NetMain(void*) {
    long long lastP = 0, lastRetry = 0;
    while (g_run) {
        if (g_lfd < 0) {
            const long long t = NowMs();
            if (t - lastRetry < 1000) { usleep(50 * 1000); continue; }
            lastRetry = t;
            if (!OpenListener()) continue;
        }
        pollfd f[2]; int n = 0;
        f[n].fd = g_lfd; f[n].events = POLLIN; f[n].revents = 0; n++;
        if (g_cfd >= 0) { f[n].fd = g_cfd; f[n].events = POLLIN; f[n].revents = 0; n++; }
        poll(f, n, 40);

        if (f[0].revents & POLLIN) {
            const int c = accept(g_lfd, nullptr, nullptr);
            if (c >= 0) {
                CloseClient();
                g_cfd = c;
                int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
                g_accepted++;
                SendLine("HELLO gtasa-mcbridge 1");
            }
        }
        if (n > 1 && (f[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t r = recv(g_cfd, g_rx + g_rxLen, sizeof(g_rx) - 1 - g_rxLen, 0);
            if (r > 0) { g_rxLen += (int)r; SplitLines(); }
            else if (r == 0 || (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)) CloseClient();
        }

        char line[kLine];
        while (g_out.Pop(line)) SendLine(line);                  // lines queued by the game thread (consumed even with no client)

        const long long t = NowMs();
        if (g_cfd >= 0 && t - lastP >= 100) {
            lastP = t;
            float p[3]; unsigned seq = 0; bool send = false;
            pthread_mutex_lock(&g_mu);
            if (g_posDirty) { memcpy(p, g_pos, sizeof(p)); seq = ++g_seq; g_posDirty = false; send = true; }
            pthread_mutex_unlock(&g_mu);
            if (send) { snprintf(line, sizeof(line), "P %u %.3f %.3f %.3f", seq, p[0], p[1], p[2]); SendLine(line); }
        }
    }
    if (g_lfd >= 0) { close(g_lfd); g_lfd = -1; }
    CloseClient();
    return nullptr;
}

// ---- API ----------------------------------------------------------------------------------
static bool Start(int port) {
    if (g_run) return true;
    g_port = port; g_run = 1;
    if (pthread_create(&g_th, nullptr, NetMain, nullptr) != 0) { g_run = 0; return false; }
    pthread_detach(g_th);
    return true;
}
static void Stop() { g_run = 0; }
static void SetPlayer(float x, float y, float z) {                // game thread
    pthread_mutex_lock(&g_mu);
    g_pos[0] = x; g_pos[1] = y; g_pos[2] = z; g_posDirty = true;
    pthread_mutex_unlock(&g_mu);
}
static bool PopInbox(char* out /* kLine bytes */) { return g_in.Pop(out); }      // game thread
static void Say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Say(const char* fmt, ...) {                           // game thread -> client
    char b[kLine];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    g_out.Push(b);
}
static bool Connected() { return g_cfd >= 0; }

} // namespace Bridge
