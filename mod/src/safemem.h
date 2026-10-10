#pragma once
// Fault-proof reads of game memory.
//
// The menu pokes at raw game structures (radar blips, ped/vehicle fields).  If one of those
// offsets or pointers is wrong for the running game build, a plain read kills the whole game
// with SIGSEGV.  SafeMem::Read asks the KERNEL to do the copy instead: write() from the
// suspect address into a pipe fails with EFAULT on an unmapped / unreadable page instead of
// raising a signal, so a bad pointer just makes the call return false.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

namespace SafeMem {

static int g_pipe[2] = {-1, -1};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static const size_t kChunk = 16384;               // far below the pipe's 64 KiB capacity

static void Drain() {                             // empty the pipe (only needed after a partial write)
    char tmp[256];
    for (;;) {
        const ssize_t r = read(g_pipe[0], tmp, sizeof(tmp));
        if (r <= 0) break;
    }
}

static bool OpenPipe() {
    if (g_pipe[0] >= 0) return true;
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return false;
    g_pipe[0] = fds[0]; g_pipe[1] = fds[1];
    return true;
}

// copy n bytes from `src` to `dst`; false (and dst unspecified) if any byte of src is not readable
static bool Read(const void* src, void* dst, size_t n) {
    if (!src || !dst) return false;
    if ((uintptr_t)src < 4096) return false;                       // null page and its neighbours
    pthread_mutex_lock(&g_mu);
    bool ok = OpenPipe();
    size_t done = 0;
    while (ok && done < n) {
        const size_t want = (n - done) < kChunk ? (n - done) : kChunk;
        const ssize_t w = write(g_pipe[1], (const char*)src + done, want);
        if (w != (ssize_t)want) { Drain(); ok = false; break; }    // EFAULT: not readable
        size_t got = 0;
        while (got < want) {
            const ssize_t r = read(g_pipe[0], (char*)dst + done + got, want - got);
            if (r <= 0) { ok = false; break; }
            got += (size_t)r;
        }
        if (!ok) { Drain(); break; }
        done += want;
    }
    pthread_mutex_unlock(&g_mu);
    return ok && done == n;
}

// read one value of type T located `off` bytes after `base`
template <typename T> static bool Get(const void* base, size_t off, T& out) {
    return Read((const char*)base + off, &out, sizeof(T));
}

// can n bytes at p be read?  (nothing is returned, only the answer)
static bool Readable(const void* p, size_t n) {
    char tmp[64];
    if (n > sizeof(tmp)) n = sizeof(tmp);                          // checking the first bytes is enough for "is this a pointer"
    return Read(p, tmp, n);
}

} // namespace SafeMem
