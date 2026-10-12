#pragma once
// MCBridge progress model: what the plugin can PROVE, at runtime, about each milestone of the GTA x Minecraft bridge.
// Pure C++ (no ImGui, no AML, no game headers) so it can be unit-tested on any Linux box.
//
// Rules this file follows (the project rules):
//   * a milestone is "done" only when live evidence says so (a counter, a result, a connected socket) - never a guess
//   * milestones that have no code in this build are fixed to "not started" until the code AND a phone log exist
//   * "earlier run" marks are read from progress.txt, which only ever records things that were proven live on this phone
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Mc {

enum MsId {
    kMsPlugin = 0,   // plugin loaded, game symbols resolved, CClock::Update hooked
    kMsServer,       // loopback TCP server is listening
    kMsPos,          // player position can be read
    kMsPed,          // test: spawn a pedestrian
    kMsObj,          // test: spawn an object
    kMsMcConn,       // a client that identified itself as Minecraft is connected
    kMsMcEnt,        // entity (E) lines received from that Minecraft client
    kMsMcBlk,        // block (B) lines received from that Minecraft client
    kMsMirror,       // opt-in: the Minecraft player is mirrored by a GTA ped that moves with it (position only)
    kMsDraw,         // no code yet: draw Minecraft blocks in the GTA world
    kMsSteve,        // no code yet: Steve / mobs as peds that interact with cars and pedestrians
    kMsCam,          // no code yet: camera + effects two-way sync
    kMsCount
};

enum State { kUntested = 0, kWaiting, kNotStarted, kFailed, kDone };

static const char* const kMsName[kMsCount] = {
    "GTA plugin loaded", "Bridge server listening", "Read player position", "Spawn a pedestrian (test)",
    "Spawn an object (test)", "Minecraft connected to the bridge", "Receive Minecraft entity data (E lines)",
    "Receive Minecraft block data (B lines)", "Mirror the Minecraft player as a GTA ped (movement only)",
    "Draw Minecraft blocks in the GTA world",
    "Steve and mobs as GTA peds (interact with cars/peds)", "Camera and effects two-way sync",
};

static const char* StateName(State s) {
    switch (s) {
        case kDone: return "DONE";
        case kWaiting: return "WAITING";
        case kNotStarted: return "NOT STARTED";
        case kFailed: return "FAILED";
        default: return "UNTESTED";
    }
}

struct Probe { char name[28]; int id; };          // id < 0 -> the game has no model with that name

static_assert(sizeof(kMsName) / sizeof(kMsName[0]) == kMsCount, "kMsName must have one entry per milestone");

// Everything the menu shows comes from this one struct, filled by mcbridge.cpp.
struct Evidence {
    int loaded = 0, hookInstalled = 0, symFound = 0, symTotal = 0;
    int gameState = -1, paused = 0;                // gGameState (7 = front-end menu, 9 = playing), pause menu open
    int port = 0, serverStarted = 0, listening = 0, bindErrno = 0;
    int connected = 0;
    unsigned long accepted = 0, rx = 0, tx = 0, dropped = 0;
    char clientName[32] = "";                      // what the connected client said in its "ID <name> <version>" line
    int clientIsMinecraft = 0;
    char clientVersion[24] = "";                   // version from the same ID line
    char mcStatus[112] = "";                       // last "S ..." line of the Minecraft side (hook installed? ticks? ...)
    unsigned long mcPosCount = 0;                  // "E 0 player ..." lines from Minecraft
    float mcPos[3] = {0.f, 0.f, 0.f};              // last Minecraft player position (x east, y up, z south)
    int mirrorOn = 0;                              // menu toggle (default off)
    int mirrorState = -1;                          // -1 not run, 0 failed, 1 ok (last move was written and read back)
    unsigned long mirrorMoves = 0;
    char mirrorNote[96] = "";
    unsigned long entMsgs = 0, blkMsgs = 0;        // E / B lines from a client that identified as Minecraft
    unsigned long otherDataMsgs = 0;               // E / B lines from any other client (test tools, fake data)
    unsigned long posReads = 0;
    float pos[3] = {0.f, 0.f, 0.f};
    int selfTestRan = 0;
    int pedTest = -1, objTest = -1;                // -1 not run, 0 failed, 1 ok
    char pedNote[96] = "", objNote[96] = "";       // why a test failed
    char objName[48] = "";                         // model name the object test uses (config obj_name)
    int nProbe = 0;
    Probe probe[8];
};

// Requests from the menu to the game thread (set by the menu, consumed in OnTick).
struct Requests { volatile int probe = 0, ped = 0, obj = 0, mirror = 0; };
static Requests g_req;

struct Row { int id; State st; bool earlier; char detail[128]; };

// milestones proven RIGHT NOW (bit i = milestone i)
static uint32_t LiveBits(const Evidence& e) {
    uint32_t b = 0;
    if (e.loaded && e.hookInstalled && e.symFound > 0) b |= 1u << kMsPlugin;
    if (e.listening) b |= 1u << kMsServer;
    if (e.posReads > 0) b |= 1u << kMsPos;
    if (e.pedTest == 1) b |= 1u << kMsPed;
    if (e.objTest == 1) b |= 1u << kMsObj;
    if (e.connected && e.clientIsMinecraft) b |= 1u << kMsMcConn;
    if (e.entMsgs > 0) b |= 1u << kMsMcEnt;
    if (e.blkMsgs > 0) b |= 1u << kMsMcBlk;
    if (e.mirrorState == 1 && e.mirrorMoves > 0) b |= 1u << kMsMirror;
    return b;
}

// Lines that describe the Minecraft side (ID / E / B / S).  Returns true when the line was one of those, so the caller can
// log it; commands like PING / SPAWN_* are not handled here.  Test tools and fake data are told apart by their ID line.
static bool ApplyDataLine(Evidence& e, const char* line) {
    if (!strncmp(line, "ID ", 3)) {
        const char* n = line + 3;
        size_t len = strcspn(n, " ");
        if (len >= sizeof(e.clientName)) len = sizeof(e.clientName) - 1;
        memcpy(e.clientName, n, len); e.clientName[len] = 0;
        const char* v = n + strcspn(n, " ");
        while (*v == ' ') v++;
        snprintf(e.clientVersion, sizeof(e.clientVersion), "%s", v);
        e.clientIsMinecraft = (strncmp(e.clientName, "minecraft", 9) == 0) ? 1 : 0;
        return true;
    }
    if ((line[0] == 'E' || line[0] == 'B') && line[1] == ' ') {
        if (!e.clientIsMinecraft) { e.otherDataMsgs++; return true; }        // never counts as Minecraft
        if (line[0] == 'B') { e.blkMsgs++; return true; }
        e.entMsgs++;
        int id = -1; char type[16] = ""; float x = 0, y = 0, z = 0;
        if (sscanf(line + 2, "%d %15s %f %f %f", &id, type, &x, &y, &z) == 5 && id == 0 && !strcmp(type, "player") &&
            x == x && y == y && z == z && x > -3e7f && x < 3e7f && z > -3e7f && z < 3e7f && y > -1e5f && y < 1e5f) {
            e.mcPos[0] = x; e.mcPos[1] = y; e.mcPos[2] = z; e.mcPosCount++;
        }
        return true;
    }
    if (line[0] == 'S' && line[1] == ' ') { snprintf(e.mcStatus, sizeof(e.mcStatus), "%s", line + 2); return true; }
    return false;
}

// Fills rows[kMsCount]; returns how many milestones are done (this run or proven in an earlier run).
static int Evaluate(const Evidence& e, uint32_t ever, Row* rows) {
    const uint32_t live = LiveBits(e);
    int done = 0;
    for (int i = 0; i < kMsCount; i++) {
        Row& r = rows[i];
        r.id = i; r.st = kUntested; r.earlier = false; r.detail[0] = 0;
        char* d = r.detail; const size_t n = sizeof(r.detail);
        switch (i) {
            case kMsPlugin:
                if (live & (1u << i)) { r.st = kDone; snprintf(d, n, "symbols %d/%d, CClock::Update hook ok", e.symFound, e.symTotal); }
                else if (e.loaded) { r.st = kFailed; snprintf(d, n, "hook not installed (symbols %d/%d)", e.symFound, e.symTotal); }
                else { r.st = kWaiting; snprintf(d, n, "loading"); }
                break;
            case kMsServer:
                if (e.listening) { r.st = kDone; snprintf(d, n, "listening on 127.0.0.1:%d", e.port); }
                else if (e.bindErrno) { r.st = kFailed; snprintf(d, n, "bind failed, errno %d (port %d)", e.bindErrno, e.port); }
                else { r.st = kWaiting; snprintf(d, n, "%s", e.serverStarted ? "starting" : "not started"); }
                break;
            case kMsPos:
                if (live & (1u << i)) { r.st = kDone; snprintf(d, n, "%.1f %.1f %.1f (%lu reads)", e.pos[0], e.pos[1], e.pos[2], e.posReads); }
                else { r.st = kWaiting; snprintf(d, n, "no successful read yet"); }
                break;
            case kMsPed:
            case kMsObj: {
                const int t = (i == kMsPed) ? e.pedTest : e.objTest;
                if (t == 1) { r.st = kDone; snprintf(d, n, "created and its position read back ok"); }
                else if (t == 0) { const char* why = (i == kMsPed) ? e.pedNote : e.objNote;
                    r.st = kFailed; snprintf(d, n, "%s", why[0] ? why : "the game call failed"); }
                else { r.st = kUntested; snprintf(d, n, "not run in this session"); }
                break;
            }
            case kMsMcConn:
                if (live & (1u << i)) { r.st = kDone; snprintf(d, n, "client \"%s\" connected (%lu connection(s))", e.clientName, e.accepted); }
                else if (e.connected && e.clientName[0]) { r.st = kWaiting; snprintf(d, n, "client \"%s\" is connected, but it is not Minecraft", e.clientName); }
                else if (e.connected) { r.st = kWaiting; snprintf(d, n, "a client is connected but has not sent an ID line"); }
                else if (e.accepted > 0) { r.st = kWaiting; snprintf(d, n, "no client now (%lu earlier connection(s))", e.accepted); }
                else { r.st = kWaiting; snprintf(d, n, "no client has connected yet"); }
                break;
            case kMsMcEnt:
                if (e.entMsgs > 0) {
                    r.st = kDone;
                    if (e.mcPosCount > 0) snprintf(d, n, "%lu E line(s); player at %.1f %.1f %.1f", e.entMsgs, e.mcPos[0], e.mcPos[1], e.mcPos[2]);
                    else snprintf(d, n, "%lu E line(s) from Minecraft", e.entMsgs);
                }
                else {
                    r.st = kWaiting;
                    if (e.otherDataMsgs) snprintf(d, n, "none from Minecraft (%lu from test clients)", e.otherDataMsgs);
                    else snprintf(d, n, "none received");
                }
                break;
            case kMsMcBlk:
                if (e.blkMsgs > 0) { r.st = kDone; snprintf(d, n, "%lu B line(s) from Minecraft", e.blkMsgs); }
                else { r.st = kWaiting; snprintf(d, n, "none received"); }
                break;
            case kMsMirror:
                if (e.mirrorState == 1 && e.mirrorMoves > 0) { r.st = kDone; snprintf(d, n, "proxy ped moved %lu time(s), position read back ok", e.mirrorMoves); }
                else if (e.mirrorState == 0) { r.st = kFailed; snprintf(d, n, "%s", e.mirrorNote[0] ? e.mirrorNote : "the proxy ped could not be moved"); }
                else if (e.mirrorOn) { r.st = kWaiting; snprintf(d, n, "%s", e.mcPosCount ? "starting" : "waiting for the Minecraft player position"); }
                else { r.st = kUntested; snprintf(d, n, "off (needs the Minecraft player position first)"); }
                break;
            default:   // kMsDraw, kMsSteve, kMsCam: no code in this build. Change only when the code exists AND a phone log proves it.
                r.st = kNotStarted; snprintf(d, n, "no code in this build");
                break;
        }
        r.earlier = (r.st != kDone) && i < kMsDraw && (ever & (1u << i)) != 0;
        if (r.st == kDone || r.earlier) done++;
    }
    return done;
}

// ---- progress.txt: milestones proven live on this phone in any run ("ever" bits) + number of runs ----
static uint32_t LoadEver(const char* path, unsigned* runs) {
    uint32_t bits = 0; unsigned r = 0;
    if (FILE* f = fopen(path, "r")) {
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            unsigned v = 0;
            if (sscanf(line, "ever=%x", &v) == 1) bits = v;
            else if (sscanf(line, "runs=%u", &v) == 1) r = v;
        }
        fclose(f);
    }
    bits &= (1u << kMsDraw) - 1u;                  // only live-provable milestones are ever recorded
    if (runs) *runs = r;
    return bits;
}
static void SaveEver(const char* path, uint32_t bits, unsigned runs) {
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "ever=%x\nruns=%u\n", (unsigned)bits, runs);
        fclose(f);
    }
}

} // namespace Mc
