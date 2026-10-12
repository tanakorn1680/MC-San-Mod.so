// Host test (any Linux box) for the GTA-side progress model:  g++ -std=c++17 -I../mod/src test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
#include "mcstatus.h"
#include <assert.h>
using namespace Mc;

static int rows(const Evidence& e, uint32_t ever, Row* r) { return Evaluate(e, ever, r); }

int main() {
    Row r[kMsCount];
    Evidence e; e.port = 47800;
    assert(rows(e, 0, r) == 0);                                   // nothing loaded: nothing done
    assert(r[kMsDraw].st == kNotStarted && r[kMsSteve].st == kNotStarted && r[kMsCam].st == kNotStarted);

    e.loaded = e.hookInstalled = 1; e.symFound = e.symTotal = 8; e.serverStarted = e.listening = 1;
    e.posReads = 40; e.pedTest = 1;
    assert(rows(e, 0, r) == 4);                                   // plugin, server, position, ped

    // a test tool connects and sends fake E lines: it must NOT count as Minecraft
    e.connected = 1; e.accepted = 1;
    assert(ApplyDataLine(e, "ID bridge_probe 1"));
    assert(!e.clientIsMinecraft);
    assert(ApplyDataLine(e, "E 0 player 1 2 3 0"));
    assert(e.entMsgs == 0 && e.otherDataMsgs == 1 && e.mcPosCount == 0);
    assert(rows(e, 0, r) == 4 && r[kMsMcConn].st == kWaiting && r[kMsMcEnt].st == kWaiting);

    // the real Minecraft side
    assert(ApplyDataLine(e, "ID minecraft 0.1"));
    assert(e.clientIsMinecraft && !strcmp(e.clientName, "minecraft") && !strcmp(e.clientVersion, "0.1"));
    assert(ApplyDataLine(e, "E 0 player 100.5 70.0 -20.25 90.0"));
    assert(e.entMsgs == 1 && e.mcPosCount == 1 && e.mcPos[0] == 100.5f && e.mcPos[1] == 70.f && e.mcPos[2] == -20.25f);
    assert(ApplyDataLine(e, "E 7 zombie 1 2 3 0"));               // other entities count as E lines but are not the player
    assert(e.entMsgs == 2 && e.mcPosCount == 1);
    assert(ApplyDataLine(e, "E 0 player nan 1 2 0") && e.mcPosCount == 1);          // garbage never becomes a position
    assert(ApplyDataLine(e, "E 0 player 1e9 1 2 0") && e.mcPosCount == 1);
    assert(ApplyDataLine(e, "B 1 2 3 4") && e.blkMsgs == 1);
    assert(ApplyDataLine(e, "S hook=1 ticks=12") && !strcmp(e.mcStatus, "hook=1 ticks=12"));
    assert(!ApplyDataLine(e, "PING") && !ApplyDataLine(e, "SPAWN_PED"));              // commands are not data lines
    assert(rows(e, 0, r) == 7 && r[kMsMcConn].st == kDone && r[kMsMcEnt].st == kDone && r[kMsMcBlk].st == kDone);

    // mirror milestone
    assert(r[kMsMirror].st == kUntested);
    e.mirrorOn = 1; assert(rows(e, 0, r) == 7 && r[kMsMirror].st == kWaiting);
    e.mirrorState = 1; e.mirrorMoves = 3; assert(rows(e, 0, r) == 8 && r[kMsMirror].st == kDone);
    e.mirrorState = 0; snprintf(e.mirrorNote, sizeof(e.mirrorNote), "the proxy ped is gone"); e.mirrorMoves = 0;
    assert(rows(e, 0, r) == 7 && r[kMsMirror].st == kFailed);
    printf("mirror failure text: %s\n", r[kMsMirror].detail);

    // failures + earlier-run memory
    Evidence f; f.loaded = f.hookInstalled = 1; f.symFound = 3; f.symTotal = 8; f.serverStarted = 1; f.bindErrno = 98; f.pedTest = 0;
    snprintf(f.pedNote, sizeof(f.pedNote), "AddPed returned null");
    rows(f, 0, r); assert(r[kMsServer].st == kFailed && r[kMsPed].st == kFailed);
    Evidence n; n.loaded = n.hookInstalled = 1; n.symFound = n.symTotal = 8; n.listening = n.serverStarted = 1; n.port = 47800;
    const uint32_t all = (1u << kMsDraw) - 1u;                    // every live-provable milestone proven in earlier runs
    assert(rows(n, all, r) == kMsDraw && r[kMsMcConn].earlier && r[kMsMirror].earlier && r[kMsMcConn].st == kWaiting);

    SaveEver("/tmp/progress_test.txt", 0xffff, 7); unsigned runs = 0;
    const uint32_t b = LoadEver("/tmp/progress_test.txt", &runs);
    assert(b == all && runs == 7);                                // bits for milestones with no code are dropped
    puts("test_logic: ALL OK");
    return 0;
}
