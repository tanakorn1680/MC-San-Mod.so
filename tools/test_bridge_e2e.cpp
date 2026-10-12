// End-to-end host test: the Minecraft-side client (mcmod/src/mcnet.h) against the GTA-side server (mod/src/bridge_net.h) and
// the GTA-side line parser (mod/src/mcstatus.h).   Build + run:  bash tools/run_tests.sh
#include <assert.h>
#include <math.h>
#include "bridge_net.h"
#include "mcstatus.h"
#include "../mcmod/src/mcnet.h"

static Mc::Evidence ev;
static unsigned long lastAccepted = 0;

// same logic as SyncBridgeEvidence() + HandleLine() in mcbridge.cpp (minus the game-only parts)
static void Pump() {
    const Bridge::Stats s = Bridge::GetStats();
    ev.listening = s.listening; ev.connected = s.connected; ev.accepted = s.accepted; ev.rx = s.rx; ev.tx = s.tx;
    if (s.accepted != lastAccepted || !s.connected) { lastAccepted = s.accepted; ev.clientName[0] = 0; ev.clientIsMinecraft = 0; }
    char line[Bridge::kLine];
    while (Bridge::PopInbox(line)) Mc::ApplyDataLine(ev, line);
}

int main() {
    const int port = 47000 + (getpid() % 1000);
    // 1) Minecraft side starts FIRST: it must keep retrying until the GTA side appears
    assert(McNet::Start(port, "0.1"));
    McNet::SetStatus("hook=1 ticks=0");
    McNet::Publish(10.f, 64.f, -5.f, 0.f);
    McNet::SleepMs(300);
    assert(!McNet::Snapshot().connected);

    // 2) GTA side comes up
    assert(Bridge::Start(port));
    Bridge::SetPlayer(2488.9f, -1666.9f, 13.3f);
    bool linked = false;
    for (int i = 0; i < 80 && !linked; i++) {                     // up to ~8 s (client retries every 2 s)
        Pump(); McNet::SleepMs(100);
        linked = ev.clientIsMinecraft && ev.mcPosCount > 0 && ev.mcStatus[0];
    }
    assert(linked);
    assert(!strcmp(ev.clientName, "minecraft") && !strcmp(ev.clientVersion, "0.1"));
    assert(ev.mcPos[0] == 10.f && ev.mcPos[1] == 64.f && ev.mcPos[2] == -5.f);
    assert(!strncmp(ev.mcStatus, "hook=1", 6));
    printf("linked: client '%s' v%s, player %.1f %.1f %.1f, status '%s'\n", ev.clientName, ev.clientVersion, ev.mcPos[0], ev.mcPos[1], ev.mcPos[2], ev.mcStatus);

    // 3) live updates: Minecraft moves, GTA moves
    const unsigned long before = ev.mcPosCount;
    McNet::Publish(11.5f, 64.f, -6.f, 45.f);
    Bridge::SetPlayer(2490.f, -1667.f, 13.3f);
    bool moved = false, gtaSeen = false;
    for (int i = 0; i < 40 && !(moved && gtaSeen); i++) {
        Pump(); McNet::SleepMs(50);
        moved = ev.mcPosCount > before && ev.mcPos[0] == 11.5f;
        const McNet::Stats st = McNet::Snapshot();
        gtaSeen = st.gtaSeq > 0 && fabsf(st.gtaPos[0] - 2490.f) < 0.01f;
    }
    assert(moved && gtaSeen);
    puts("live position exchange works in both directions");

    // 4) a test tool that is NOT minecraft must not count: emulate with raw lines on the server inbox path
    Mc::Evidence other; other.connected = 1;
    Mc::ApplyDataLine(other, "ID bridge_probe 1"); Mc::ApplyDataLine(other, "E 0 player 1 2 3 0");
    assert(!other.clientIsMinecraft && other.entMsgs == 0 && other.otherDataMsgs == 1);

    const McNet::Stats st = McNet::Snapshot();
    printf("minecraft side: connects=%llu tx=%llu rx=%llu sendFails=%llu last rx from GTA: '%s'\n",
           (unsigned long long)st.connects, (unsigned long long)st.tx, (unsigned long long)st.rx, (unsigned long long)st.sendFails, st.lastRx);
    assert(st.connects >= 1 && st.tx >= 3 && st.rx >= 2);
    Bridge::Stop(); McNet::Stop();
    puts("test_bridge_e2e: ALL OK");
    return 0;
}
