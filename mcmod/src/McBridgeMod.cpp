// MCBridge, Minecraft side (v0.1): a LeviLauncher native mod ("type": "preload-native", see manifest.json).
//
// What it does:
//   1. finds Actor-tick code in libminecraftpe.so with a byte signature (our own, made from the player's own libminecraftpe.so,
//      see docs/findings.md) and hooks it through the preloader (pl::memory::hook)
//   2. in that hook reads the local player's position (Actor + 0x208 -> state-vector component -> Vec3), every 2nd tick,
//      with a fault-proof read, and sanity-checks it
//   3. sends it to the GTA SA plugin over the loopback bridge (mcnet.h): "ID minecraft <ver>", "E 0 player x y z yaw",
//      and a short status line "S ..." so the GTA-side menu can show what this side is doing
//   4. writes its own log to <mod dir>/data/mc_session.log  (previous run: mc_previous.log)
//
// Not done yet (and shown as NOT STARTED in the GTA menu): blocks (B lines), other entities, yaw/pitch, any effect in Minecraft.
// Status: logic of the network part is tested on Linux against the real GTA-side server (tools/test_bridge_e2e.cpp); the hook part
// has been syntax-checked against the real preloader SDK only. It has NOT been built with the NDK or run inside Minecraft yet.
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "mcnet.h"

namespace {

constexpr const char* kModVersion = "0.1";
constexpr const char* kModule = "libminecraftpe.so";

// OUR OWN signature of the function that runs once per game tick for the local player (everything but each instruction's top
// byte is wildcarded, so register numbers / immediates / branch targets may differ between builds).  Made from the player's
// libminecraftpe.so (the Minecraft build whose version string contains 1.26.52); it matches exactly one place in that file.
// If a Minecraft update changes the function's shape the signature stops matching: the log and the GTA menu then say sig=0.
constexpr const char* kSigNormalTick =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 "
    "? ? ? D5 ? ? ? AA ? ? ? F9 ? ? ? F8 ? ? ? 39 ? ? ? 34 ? ? ? B9";

// Actor + 0x208 holds a pointer to the state-vector component whose first 12 bytes are the position (x, y, z floats).  The same
// offset is used by the open-source BedrockTools mod for this Minecraft generation, and libminecraftpe.so contains a tiny
// function "ldr x8,[x0,#0x208]; ldp s0,s1,[x8]; ldr s2,[x8,#8]; ret" (= Actor::getPosition) that reads exactly this.
constexpr size_t kOffStateVector = 0x208;

// ------------------------------------------------------------------------------------------------ fault-proof reads
// A wrong offset must never crash Minecraft: the kernel copies for us (write() from the suspect address into a pipe fails with
// EFAULT instead of raising SIGSEGV).
int g_pipe[2] = {-1, -1};
std::mutex g_pipeMu;

bool safeRead(const void* src, void* dst, size_t n) {
    if (!src || !dst || n == 0 || n > 4096 || reinterpret_cast<uintptr_t>(src) < 4096) return false;
    std::lock_guard<std::mutex> lock(g_pipeMu);
    if (g_pipe[0] < 0) {
        int fds[2];
        if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return false;
        g_pipe[0] = fds[0]; g_pipe[1] = fds[1];
    }
    const ssize_t w = write(g_pipe[1], src, n);
    if (w != static_cast<ssize_t>(n)) {                              // not readable: empty the pipe and report failure
        char tmp[256];
        while (read(g_pipe[0], tmp, sizeof(tmp)) > 0) {}
        return false;
    }
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(g_pipe[0], static_cast<char*>(dst) + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------------ log
std::mutex g_logMu;
FILE* g_logFile = nullptr;
pl::log::Logger* g_logger = nullptr;
std::time_t g_t0 = 0;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    char msg[300];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMu);
    if (g_logFile) {
        fprintf(g_logFile, "[%4lds] %s\n", static_cast<long>(std::time(nullptr) - g_t0), msg);
        fflush(g_logFile);
    }
    if (g_logger) g_logger->info("{}", msg);
}

// ----------------------------------------------------------------------------------------------------- hook state
using TickFn = void (*)(void*);
TickFn g_origTick = nullptr;
void* g_hookTarget = nullptr;
bool g_sigOk = false, g_hookOk = false;

std::atomic<unsigned long long> g_ticks{0}, g_badReads{0}, g_oddValues{0}, g_otherTicks{0}, g_published{0};
std::atomic<void*> g_self{nullptr};            // the one object whose ticks we follow (the first one that ticks)
float g_lastPos[3] = {0, 0, 0};
std::mutex g_posMu;

bool saneVec(const float* p) {
    for (int i = 0; i < 3; i++) if (!std::isfinite(p[i])) return false;
    return std::fabs(p[0]) < 3.0e7f && std::fabs(p[2]) < 3.0e7f && std::fabs(p[1]) < 1.0e5f;
}

void updateStatus() {
    char pos[64] = "-";
    {
        std::lock_guard<std::mutex> lock(g_posMu);
        if (g_published.load() > 0) snprintf(pos, sizeof(pos), "%.0f,%.0f,%.0f", g_lastPos[0], g_lastPos[1], g_lastPos[2]);
    }
    McNet::SetStatus("v%s sig=%d hook=%d ticks=%llu pub=%llu bad=%llu odd=%llu other=%llu pos=%s", kModVersion, g_sigOk ? 1 : 0,
                     g_hookOk ? 1 : 0, g_ticks.load(), g_published.load(), g_badReads.load(), g_oddValues.load(), g_otherTicks.load(), pos);
}

void onTick(void* self) {
    const unsigned long long n = g_ticks.fetch_add(1) + 1;
    void* expected = nullptr;
    g_self.compare_exchange_strong(expected, self);          // lock onto the first object that ticks
    if (g_self.load() != self) { g_otherTicks.fetch_add(1); return; }
    if (n == 1) logf("first tick of object %p", self);

    if (n % 2 == 0) {                                        // ~10 Hz at 20 ticks per second
        void* comp = nullptr;
        float p[3];
        if (!safeRead(static_cast<const char*>(self) + kOffStateVector, &comp, sizeof(comp)) || !comp || !safeRead(comp, p, sizeof(p))) {
            if (g_badReads.fetch_add(1) == 0) logf("position read failed (self=%p comp=%p): offset 0x%zx may be wrong for this build", self, comp, kOffStateVector);
        } else if (!saneVec(p)) {
            if (g_oddValues.fetch_add(1) == 0) logf("odd position values %g %g %g (ignored)", p[0], p[1], p[2]);
        } else {
            {
                std::lock_guard<std::mutex> lock(g_posMu);
                g_lastPos[0] = p[0]; g_lastPos[1] = p[1]; g_lastPos[2] = p[2];
            }
            if (g_published.fetch_add(1) == 0) logf("first position %.2f %.2f %.2f", p[0], p[1], p[2]);
            McNet::Publish(p[0], p[1], p[2], 0.f);           // yaw/pitch: not read yet (offset not verified)
        }
    }
    if (n % 40 == 0) updateStatus();                         // every ~2 s
}

void tickDetour(void* self) {
    if (g_origTick) g_origTick(self);                        // the game's own tick first: we never change its behaviour
    onTick(self);
}

// ------------------------------------------------------------------------------------------------------------ mod
class McBridgeMod {
public:
    static McBridgeMod& instance() { static McBridgeMod m; return m; }

    bool load(pl::mod::ModContext& ctx) {
        g_logger = &ctx.logger();
        g_t0 = std::time(nullptr);
        std::error_code ec;
        dataDir_ = ctx.dataDir();
        std::filesystem::create_directories(dataDir_, ec);
        const auto log = dataDir_ / "mc_session.log";
        const auto prev = dataDir_ / "mc_previous.log";
        std::filesystem::remove(prev, ec);
        std::filesystem::rename(log, prev, ec);              // keep one previous run (ignore "no such file")
        g_logFile = std::fopen(log.c_str(), "w");
        logf("MCBridge (Minecraft side) %s: load, data dir %s", kModVersion, dataDir_.c_str());
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        // 1) hook (best effort: even without it the connection to GTA is still made so the GTA menu can show why)
        const uintptr_t addr = pl::memory::resolveSignature(kSigNormalTick, kModule);
        g_sigOk = addr != 0;
        if (!g_sigOk) {
            logf("signature NOT found in %s: this Minecraft build differs from the one the signature was made from", kModule);
        } else {
            g_hookTarget = reinterpret_cast<void*>(addr);
            const int rc = pl::memory::hook(g_hookTarget, reinterpret_cast<void*>(&tickDetour), reinterpret_cast<void**>(&g_origTick));
            g_hookOk = (rc == 0) && g_origTick != nullptr;
            logf("signature found at %p; hook %s (rc=%d)", g_hookTarget, g_hookOk ? "installed" : "FAILED", rc);
        }
        updateStatus();

        // 2) network (port 47800, or the number in <data dir>/port.txt)
        int port = 47800;
        if (FILE* f = std::fopen((dataDir_ / "port.txt").c_str(), "r")) {
            int v = 0;
            if (std::fscanf(f, "%d", &v) == 1 && v > 1023 && v < 65536) port = v;
            std::fclose(f);
        }
        const bool net = McNet::Start(port, kModVersion);
        logf("bridge client %s (127.0.0.1:%d)", net ? "started" : "FAILED to start", port);
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        logf("disable");
        if (g_hookOk && g_hookTarget) pl::memory::unhook(g_hookTarget, reinterpret_cast<void*>(&tickDetour));
        g_hookOk = false;
        McNet::Stop();
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        logf("unload");
        std::lock_guard<std::mutex> lock(g_logMu);
        if (g_logFile) { std::fclose(g_logFile); g_logFile = nullptr; }
        return true;
    }

private:
    std::filesystem::path dataDir_;
};

} // namespace

PL_REGISTER_MOD(McBridgeMod, McBridgeMod::instance())
