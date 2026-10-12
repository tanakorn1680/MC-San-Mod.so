// MCBridge 0.4 - GTA SA side of the GTA x Minecraft bridge (AML plugin, arm64).
//
// What this version does (GTA side only; the Minecraft side does not exist yet):
//   1. resolves game functions BY NAME through AML (aml->GetSym), no hard-coded offsets
//   2. runs everything on the game thread via a CClock::Update hook
//   3. opens a loopback TCP server (127.0.0.1:47800) and publishes the player position (see bridge_net.h)
//   4. executes test commands that arrive over the bridge, plus an optional one-shot self test
//   5. writes breadcrumbs + a session log to Android/data/com.rockstargames.gtasa/files/MCBridge/
//   6. since 0.3: its own in-game menu (green "MC" button): live status, progress of every milestone, test buttons,
//      log viewer + report file. Own ImGui + own touch handling (mcui.h), no dependency on ProMenu/menusan.
//      Turn it off with menu=0 in config.txt (the bridge itself keeps working).
//
//   7. NEW in 0.4: understands the Minecraft side (mcmod/): ID / E 0 player / S lines, shows the Minecraft player + the other
//      side's status in the menu, and an opt-in "mirror": a GTA ped that follows the Minecraft player's movement.
//
// Status: 0.2 and 0.3 were built with the real NDK and run in the game (logs / report.txt from the phone): menu, model probe,
// ped test and object test (gunbox) all worked. Not yet tested: running together with ProMenu.
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
#include "safemem.h"
#include "bridge_net.h"

MYMOD(net.mcgta.bridge, MCBridge, 0.4, MCGTA)
NEEDGAME(com.rockstargames.gtasa)

// ------------------------------------------------------------------ game access
static void* g_hGame = nullptr;
static uintptr_t GameSym(const char* name) { return aml->GetSym(g_hGame, name); }

template <typename T>
static bool ResolveSym(T& out, const char* name) {            // used by rw_backend.h
    const uintptr_t a = GameSym(name);
    out = reinterpret_cast<T>(a);
    return a != 0;
}

#include "rw_backend.h"      // own copy of the RenderWare ImGui renderer
#include "mcstatus.h"        // progress model (live evidence -> milestone states)
#include "mcui.h"            // the in-game menu

// What the menu shows. Written by the game thread (and the network counters are copied in per frame), read when drawing.
static Mc::Evidence g_ev;

// CVector is only ever passed by const& here. ProMenu notes that CVector is passed by pointer in this game build,
// and by-value returns (FindPlayerCoors) are unverified, so by-value calls/returns are deliberately NOT used.
struct CVec { float x, y, z; };

typedef void* (*FindPlayerPed_t)(int);
typedef void* (*FindPlayerVehicle_t)(int, bool);
typedef void* (*AddPed_t)(int pedType, unsigned modelIndex, const CVec&, bool wander);
typedef void* (*GetModelInfo_t)(const char*, int*);
typedef void  (*RequestModel_t)(int, int);
typedef void  (*LoadAllRequested_t)(bool);
typedef void* (*ObjectCreate_t)(int, bool);
typedef void  (*WorldAdd_t)(void*);

static FindPlayerPed_t     g_findPlayerPed = nullptr;
static FindPlayerVehicle_t g_findPlayerVehicle = nullptr;
static AddPed_t            g_addPed = nullptr;
static GetModelInfo_t      g_getModelInfo = nullptr;
static RequestModel_t      g_requestModel = nullptr;
static LoadAllRequested_t  g_loadAll = nullptr;
static ObjectCreate_t      g_objectCreate = nullptr;
static WorldAdd_t          g_worldAdd = nullptr;
static int*   g_pGameState = nullptr;      // gGameState: 7 = front-end menu, 9 = playing (per ProMenu)
static char*  g_pMobileMenu = nullptr;     // gMobileMenu: int at +0x24 != 0 -> pause menu open (per ProMenu)

template <typename T> static bool Res(T& out, const char* sym, int& found, int& total) {
    total++;
    const uintptr_t a = GameSym(sym);
    out = reinterpret_cast<T>(a);
    if (a) found++; else Diag::Log("symbol MISSING: %s", sym);
    return a != 0;
}

static bool Finite(float v) { return v - v == 0.f; }

// entity world position: matrix position (+0x18 -> +0x30) or placement (+0x8); every read is fault-proof (copied from ProMenu)
static bool EntityPos(const void* e, float* out) {
    char hdr[0x20];
    if (!SafeMem::Read(e, hdr, sizeof(hdr))) return false;
    const char* m = nullptr;
    memcpy(&m, hdr + 0x18, sizeof(m));
    float p[3];
    if (m) { if (!SafeMem::Read(m + 0x30, p, sizeof(p))) return false; }
    else   { memcpy(p, hdr + 0x8, sizeof(p)); }
    if (!Finite(p[0]) || !Finite(p[1]) || !Finite(p[2])) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return true;
}
static bool PlayerPos(float* out) {
    if (!g_findPlayerPed) return false;
    void* ped = g_findPlayerPed(0);
    if (!ped) return false;
    void* veh = g_findPlayerVehicle ? g_findPlayerVehicle(-1, true) : nullptr;
    if (veh && EntityPos(veh, out)) return true;
    return EntityPos(ped, out);
}
// write a position into an entity that has just been created (same two layouts EntityPos reads)
static bool SetEntityPos(void* e, const float* p) {
    float cur[3];
    if (!EntityPos(e, cur)) return false;                 // proves the pointer and layout are readable first
    char* base = static_cast<char*>(e);
    char* m = nullptr;
    memcpy(&m, base + 0x18, sizeof(m));
    if (m) memcpy(m + 0x30, p, sizeof(float) * 3); else memcpy(base + 0x8, p, sizeof(float) * 3);
    return true;
}

// ------------------------------------------------------------------ config (Android/data/.../MCBridge/config.txt)
struct McConfig {
    int  port = 47800;
    int  selftestPed = 1;          // spawn one civilian next to the player once
    int  selftestObj = 0;          // create one object (riskier, off by default)
    int  selftestDelay = 600;      // game frames in "playing" state before the self test
    int  menu = 1;                 // 1 = show the in-game MCBridge menu (green MC button), 0 = no menu at all
    char objName[48] = "gunbox";   // model NAME for SPAWN_OBJ / object self test (looked up by the game itself)
    char probe[160] = "gunbox,wood_crate,CJ_Chair,hotdog";   // names to look up in the model table (guesses; the log shows which exist)
};
static McConfig g_cfg;

static void LoadConfig() {
    char path[256]; Diag::Path(path, sizeof(path), "config.txt");
    FILE* f = fopen(path, "r");
    if (!f) {                                            // first run: write the defaults so the file can be edited on the phone
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "port=%d\nselftest_ped=%d\nselftest_obj=%d\nselftest_delay=%d\nmenu=%d\nobj_name=%s\nprobe=%s\n",
                    g_cfg.port, g_cfg.selftestPed, g_cfg.selftestObj, g_cfg.selftestDelay, g_cfg.menu, g_cfg.objName, g_cfg.probe);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char* v = eq + 1;
        for (char* c = v; *c; c++) if (*c == '\r' || *c == '\n') *c = 0;
        if (!strcmp(line, "port")) g_cfg.port = atoi(v);
        else if (!strcmp(line, "selftest_ped")) g_cfg.selftestPed = atoi(v);
        else if (!strcmp(line, "selftest_obj")) g_cfg.selftestObj = atoi(v);
        else if (!strcmp(line, "selftest_delay")) g_cfg.selftestDelay = atoi(v);
        else if (!strcmp(line, "menu")) g_cfg.menu = atoi(v);
        else if (!strcmp(line, "obj_name")) snprintf(g_cfg.objName, sizeof(g_cfg.objName), "%s", v);
        else if (!strcmp(line, "probe")) snprintf(g_cfg.probe, sizeof(g_cfg.probe), "%s", v);
    }
    fclose(f);
}

// ------------------------------------------------------------------ test actions (game thread only)
static void TestResult(int* result, char* note, size_t cap, int ok, const char* why) {
    *result = ok;
    snprintf(note, cap, "%s", ok ? "" : why);
}

static bool ActSpawnPed() {
    float p[3];
    if (!g_addPed || !PlayerPos(p)) {
        Diag::Crumb("spawn_ped: no AddPed or no player position");
        TestResult(&g_ev.pedTest, g_ev.pedNote, sizeof(g_ev.pedNote), 0, "no AddPed function or no player position");
        return false;
    }
    const CVec at = { p[0] + 3.f, p[1], p[2] };
    Diag::Crumb("spawn_ped: calling AddPed at %.1f %.1f %.1f", at.x, at.y, at.z);
    void* ped = g_addPed(4 /*PEDTYPE_CIVMALE*/, 0 /*model 0 = player model, always loaded*/, at, false);
    Diag::Crumb("spawn_ped: AddPed returned %p", ped);
    float q[3];
    const bool posOk = ped && EntityPos(ped, q);
    if (posOk) Diag::Crumb("spawn_ped: new ped is at %.1f %.1f %.1f", q[0], q[1], q[2]);
    TestResult(&g_ev.pedTest, g_ev.pedNote, sizeof(g_ev.pedNote), posOk ? 1 : 0,
               !ped ? "AddPed returned null" : "ped created but its position could not be read");
    return ped != nullptr;
}

static void ActProbeModels(const char* list) {
    if (!g_getModelInfo) { Diag::Log("probe: GetModelInfo not available"); return; }
    char buf[160]; snprintf(buf, sizeof(buf), "%s", list);
    g_ev.nProbe = 0;
    for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        int id = -1;
        void* info = g_getModelInfo(tok, &id);
        Diag::Log("probe model '%s' -> info=%p id=%d", tok, info, id);
        if (g_ev.nProbe < 8) {                                   // keep the result for the menu (id < 0 = not found)
            Mc::Probe& pr = g_ev.probe[g_ev.nProbe++];
            snprintf(pr.name, sizeof(pr.name), "%s", tok);
            pr.id = (info && id >= 0) ? id : -1;
        }
    }
}

static bool ActSpawnObject(const char* name) {
    float p[3];
    char* note = g_ev.objNote; const size_t nc = sizeof(g_ev.objNote); int* res = &g_ev.objTest;
    if (!g_getModelInfo || !g_requestModel || !g_loadAll || !g_objectCreate || !g_worldAdd || !PlayerPos(p)) {
        Diag::Crumb("spawn_obj: missing game function or player position");
        TestResult(res, note, nc, 0, "missing game function or player position");
        return false;
    }
    int id = -1;
    void* info = g_getModelInfo(name, &id);
    Diag::Crumb("spawn_obj: '%s' info=%p id=%d", name, info, id);
    if (!info || id < 0) { TestResult(res, note, nc, 0, "model name not found in the game's model table"); return false; }
    Diag::Crumb("spawn_obj: RequestModel %d", id);
    g_requestModel(id, 2);                               // 2 = GAME_REQUIRED in DK22Pac plugin-sdk (assumed, not yet verified here)
    g_loadAll(false);
    Diag::Crumb("spawn_obj: CObject::Create");
    void* obj = g_objectCreate(id, false);
    Diag::Crumb("spawn_obj: Create returned %p", obj);
    if (!obj) { TestResult(res, note, nc, 0, "CObject::Create returned null"); return false; }
    float at[3] = { p[0] + 3.f, p[1] + 3.f, p[2] };
    if (!SetEntityPos(obj, at)) {
        Diag::Crumb("spawn_obj: could not set position (layout?)");
        TestResult(res, note, nc, 0, "object created but its position could not be set");
        return false;
    }
    Diag::Crumb("spawn_obj: CWorld::Add");
    g_worldAdd(obj);
    Diag::Crumb("spawn_obj: done");
    float back[3];
    TestResult(res, note, nc, EntityPos(obj, back) ? 1 : 0, "object added but its position could not be read back");
    return true;
}

// ------------------------------------------------------------------ per-frame work
static unsigned g_playFrames = 0;
static bool g_playingNow = false;               // gGameState == 9 and no pause menu (updated every tick)
static bool g_selfTestDone = false;
static int  g_wantPed = 0, g_wantObj = 0;
static char g_wantObjName[48];
static unsigned long g_mcMsgs = 0;
static unsigned long g_lastAccepted = 0;

// Network counters -> evidence. A new connection (or a dropped one) forgets who the client was, until it sends its ID line.
static void SyncBridgeEvidence() {
    const Bridge::Stats s = Bridge::GetStats();
    g_ev.listening = s.listening ? 1 : 0;
    g_ev.connected = s.connected ? 1 : 0;
    g_ev.bindErrno = s.bindErrno;
    g_ev.accepted = s.accepted; g_ev.rx = s.rx; g_ev.tx = s.tx; g_ev.dropped = s.dropped;
    if (s.accepted != g_lastAccepted || !s.connected) {
        g_lastAccepted = s.accepted;
        g_ev.clientName[0] = 0;
        g_ev.clientIsMinecraft = 0;
    }
}

static void HandleLine(const char* line) {
    if (!strcmp(line, "PING")) { Bridge::Say("PONG"); return; }
    if (!strncmp(line, "ID ", 3)) {                                       // "ID <name> <version>": who is on the other end
        Mc::ApplyDataLine(g_ev, line);
        Diag::Log("bridge: client identified as '%s' version '%s'%s", g_ev.clientName, g_ev.clientVersion,
                  g_ev.clientIsMinecraft ? "" : " (not Minecraft)");
        Bridge::Say("OK ID");
        return;
    }
    if (line[0] == 'S' && line[1] == ' ') {                               // status text of the Minecraft side
        const bool changed = strcmp(g_ev.mcStatus, line + 2) != 0;
        Mc::ApplyDataLine(g_ev, line);
        static int logged = 0;
        if (changed && ++logged <= 30) Diag::Log("mc status: %s", g_ev.mcStatus);
        return;
    }
    if (!strcmp(line, "SPAWN_PED")) { if (!g_playingNow) { Bridge::Say("ERR SPAWN_PED not playing"); return; } g_wantPed = 1; return; }
    if (!strncmp(line, "SPAWN_OBJ ", 10)) {
        if (!g_playingNow) { Bridge::Say("ERR SPAWN_OBJ not playing"); return; }
        snprintf(g_wantObjName, sizeof(g_wantObjName), "%s", line + 10); g_wantObj = 1; return;
    }
    if ((line[0] == 'E' || line[0] == 'B') && line[1] == ' ') {            // draft Minecraft data: only counted for now
        Mc::ApplyDataLine(g_ev, line);                                     // test tools / fake data never count as Minecraft
        if (++g_mcMsgs <= 20) Diag::Log("mc->gta: %s", line);
        return;
    }
    Diag::Log("bridge: unknown line '%s'", line);
    Bridge::Say("ERR unknown");
}

// ---- mirror of the Minecraft player (opt-in from the menu).  Position only: a ped created with the same AddPed call the ped test
// uses is moved every tick to  anchor + (minecraft position - minecraft anchor).  Axis mapping (an assumption, flip it here if
// it looks mirrored in the game): Minecraft x = east, y = up, z = south;  GTA x = east, y = north, z = up.
static void*  g_mirPed = nullptr;
static char   g_mirVtbl[8];                    // first 8 bytes of the proxy ped at creation: its vtable pointer
static float  g_mirGta[3], g_mirMc[3];
static bool   g_mirAnchored = false;
static unsigned long g_mirLastCount = 0;

static void MirrorFail(const char* why) {
    g_ev.mirrorState = 0;
    snprintf(g_ev.mirrorNote, sizeof(g_ev.mirrorNote), "%s", why);
    g_ev.mirrorOn = 0;                         // never keep poking at something that failed
    Diag::Crumb("mirror: FAILED: %s", why);
}

static void MirrorTick() {
    if (!g_ev.mirrorOn || g_ev.mcPosCount == 0) return;
    float me[3];
    if (!g_mirPed) {
        if (!g_addPed || !PlayerPos(me)) { MirrorFail("no AddPed function or no player position"); return; }
        const CVec at = { me[0] + 3.f, me[1], me[2] };
        g_mirPed = g_addPed(4 /*PEDTYPE_CIVMALE*/, 0, at, false);
        Diag::Crumb("mirror: AddPed returned %p", g_mirPed);
        if (!g_mirPed) { MirrorFail("AddPed returned null"); return; }
        if (!SafeMem::Read(g_mirPed, g_mirVtbl, sizeof(g_mirVtbl))) { g_mirPed = nullptr; MirrorFail("new ped is not readable"); return; }
        g_mirGta[0] = at.x; g_mirGta[1] = at.y; g_mirGta[2] = at.z;
        memcpy(g_mirMc, g_ev.mcPos, sizeof(g_mirMc));
        g_mirAnchored = true; g_mirLastCount = g_ev.mcPosCount;
        g_ev.mirrorState = -1; g_ev.mirrorMoves = 0; g_ev.mirrorNote[0] = 0;
        return;
    }
    if (g_ev.mcPosCount == g_mirLastCount) return;                        // nothing new from Minecraft
    g_mirLastCount = g_ev.mcPosCount;

    // is it still the same object?  (the game may remove peds: a changed vtable pointer or an unreadable page means yes)
    char vt[8];
    if (!SafeMem::Read(g_mirPed, vt, sizeof(vt)) || memcmp(vt, g_mirVtbl, sizeof(vt)) != 0) {
        g_mirPed = nullptr; g_mirAnchored = false;
        MirrorFail("the proxy ped is gone (the game removed it)");
        return;
    }
    float t[3] = { g_mirGta[0] + (g_ev.mcPos[0] - g_mirMc[0]),
                   g_mirGta[1] - (g_ev.mcPos[2] - g_mirMc[2]),
                   g_mirGta[2] + (g_ev.mcPos[1] - g_mirMc[1]) };
    if (PlayerPos(me)) {                                                  // keep it near the player, or the game would despawn it
        const float dx = t[0] - me[0], dy = t[1] - me[1];
        if (dx * dx + dy * dy > 100.f * 100.f) {                          // re-anchor 3 m beside the player
            g_mirGta[0] = me[0] + 3.f; g_mirGta[1] = me[1]; g_mirGta[2] = me[2];
            memcpy(g_mirMc, g_ev.mcPos, sizeof(g_mirMc));
            memcpy(t, g_mirGta, sizeof(t));
            Diag::Crumb("mirror: re-anchored near the player");
        }
    }
    float back[3];
    if (!SetEntityPos(g_mirPed, t) || !EntityPos(g_mirPed, back)) { MirrorFail("could not write/read the proxy ped position"); return; }
    if (fabsf(back[0] - t[0]) > 0.5f || fabsf(back[1] - t[1]) > 0.5f || fabsf(back[2] - t[2]) > 0.5f) {
        MirrorFail("the position did not stay where it was written"); return;
    }
    g_ev.mirrorState = 1; g_ev.mirrorMoves++;
    if (g_ev.mirrorMoves <= 3) Diag::Crumb("mirror: moved proxy ped to %.1f %.1f %.1f (minecraft %.1f %.1f %.1f)", t[0], t[1], t[2],
                                           g_ev.mcPos[0], g_ev.mcPos[1], g_ev.mcPos[2]);
}

static void OnTick() {
    const int state = g_pGameState ? *g_pGameState : -1;
    const bool paused = g_pMobileMenu && *reinterpret_cast<const int*>(g_pMobileMenu + 0x24) != 0;
    g_playingNow = (state == 9) && !paused;
    g_ev.gameState = state; g_ev.paused = paused ? 1 : 0;

    // protocol lines (ID, PING, counters) are handled in every game state; spawn commands only while playing
    SyncBridgeEvidence();
    char line[Bridge::kLine];
    int guard = 0;
    while (guard++ < 16 && Bridge::PopInbox(line)) HandleLine(line);

    if (!g_playingNow) { g_playFrames = 0; return; }
    g_playFrames++;

    if (g_playFrames % 6 == 0) {                                          // ~10 Hz at 60 fps
        float p[3];
        if (PlayerPos(p)) {
            Bridge::SetPlayer(p[0], p[1], p[2]);
            g_ev.posReads++; g_ev.pos[0] = p[0]; g_ev.pos[1] = p[1]; g_ev.pos[2] = p[2];
        }
    }

    // test buttons of the in-game menu (the menu only queues them; they run here, on the game thread)
    if (Mc::g_req.probe) { Mc::g_req.probe = 0; Diag::Crumb("menu: model check"); ActProbeModels(g_cfg.probe); }
    if (Mc::g_req.ped)   { Mc::g_req.ped = 0;   Diag::Crumb("menu: spawn ped test"); ActSpawnPed(); }
    if (Mc::g_req.obj)   { Mc::g_req.obj = 0;   Diag::Crumb("menu: spawn object test (%s)", g_cfg.objName); ActSpawnObject(g_cfg.objName); }
    if (Mc::g_req.mirror) {
        Mc::g_req.mirror = 0;
        g_ev.mirrorOn = g_ev.mirrorOn ? 0 : 1;
        if (g_ev.mirrorOn) { g_ev.mirrorState = -1; g_ev.mirrorMoves = 0; g_ev.mirrorNote[0] = 0; g_mirLastCount = 0; }
        Diag::Crumb("menu: mirror %s", g_ev.mirrorOn ? "ON" : "off");
    }
    MirrorTick();

    if (g_wantPed) { g_wantPed = 0; Bridge::Say(ActSpawnPed() ? "OK SPAWN_PED" : "ERR SPAWN_PED"); }
    if (g_wantObj) { g_wantObj = 0; Bridge::Say(ActSpawnObject(g_wantObjName) ? "OK SPAWN_OBJ" : "ERR SPAWN_OBJ"); }

    if (!g_selfTestDone && g_playFrames >= (unsigned)g_cfg.selftestDelay) {
        g_selfTestDone = true;
        g_ev.selfTestRan = 1;
        Diag::Crumb("selftest: start (frame %u)", g_playFrames);
        float p[3];
        if (PlayerPos(p)) Diag::Crumb("selftest: player at %.2f %.2f %.2f", p[0], p[1], p[2]);
        else Diag::Crumb("selftest: PlayerPos FAILED");
        ActProbeModels(g_cfg.probe);
        if (g_cfg.selftestPed) ActSpawnPed();
        if (g_cfg.selftestObj) ActSpawnObject(g_cfg.objName);
        Diag::Crumb("selftest: finished");
    }
}

// ------------------------------------------------------------------ in-game menu plumbing (see mcui.h)
static bool   g_menuInited = false, g_menuFailed = false;
static char*  g_pRsGlobal = nullptr;     // RsGlobal (int width at +8, int height at +12)
static double g_lastT = 0.0;

static double NowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static bool FileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool ScreenSize(float& w, float& h) {
    if (!g_pRsGlobal) return false;
    const int* p = reinterpret_cast<const int*>(g_pRsGlobal + 8);
    w = (float)p[0];
    h = (float)p[1];
    return w > 0.f && h > 0.f;
}

static bool ContainsI(const char* hay, const char* needle) {
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

// Android ships a Thai font in its system font folders; pick the plain Sans/Regular one
static bool FindSystemThaiFont(char* out, size_t cap) {
    static const char* kDirs[] = {"/system/fonts", "/product/fonts", "/system/product/fonts", "/system_ext/fonts", "/vendor/fonts"};
    int best = -1;
    for (const char* dir : kDirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const char* nm = e->d_name;
            if (!ContainsI(nm, "thai")) continue;
            if (!ContainsI(nm, ".ttf") && !ContainsI(nm, ".otf") && !ContainsI(nm, ".ttc")) continue;
            int score = 1;
            if (ContainsI(nm, "sans")) score += 2;
            if (ContainsI(nm, "regular")) score += 3;
            if (ContainsI(nm, "serif") || ContainsI(nm, "bold") || ContainsI(nm, "light") || ContainsI(nm, "thin") ||
                ContainsI(nm, "black") || ContainsI(nm, "medium") || ContainsI(nm, "italic") || ContainsI(nm, "looped")) score -= 6;
            if (score > best) { best = score; snprintf(out, cap, "%s/%s", dir, nm); }
        }
        closedir(d);
    }
    return best > 0;
}

static bool InitMenu() {
    float w = 0, h = 0;
    if (!ScreenSize(w, h)) return false;       // RenderWare not ready yet, try again next frame
    const float sc = (w < h ? w : h) / 1080.f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;   // touch is handled by mcui.h
    io.MouseDrawCursor = false;

    // main font: MCBridge/font.ttf if the user put one there, else a common Android system font, else ImGui's built-in
    char own[256]; Diag::Path(own, sizeof(own), "font.ttf");
    const char* fonts[] = { own, "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf" };
    const char* fontUsed = "built-in";
    ImFont* font = nullptr;
    for (const char* path : fonts) {
        if (!FileExists(path)) continue;
        font = io.Fonts->AddFontFromFileTTF(path, 34.f * sc);
        if (font) { fontUsed = path; break; }
    }
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = 34.f * sc;
        io.Fonts->AddFontDefault(&cfg);
    }

    // Thai glyphs are merged into the same font, so one atlas serves both languages
    bool thaiOk = false;
    char thaiUsed[512] = "none";                       // lives as long as InitMenu, so the log line below prints the real path
    {
        static const ImWchar kThai[] = { 0x0E00, 0x0E7F, 0 };
        char own2[256]; Diag::Path(own2, sizeof(own2), "font_th.ttf");
        char sys[512] = "";
        const char* cands[3] = { own2, nullptr, nullptr };
        if (FindSystemThaiFont(sys, sizeof(sys))) cands[1] = sys;
        for (const char* path : cands) {
            if (!path || !FileExists(path)) continue;
            ImFontConfig tc;
            tc.MergeMode = true;
            if (io.Fonts->AddFontFromFileTTF(path, 34.f * sc, &tc, kThai)) { thaiOk = true; snprintf(thaiUsed, sizeof(thaiUsed), "%s", path); break; }
        }
    }

    McUI::Init(sc, thaiOk);
    Diag::Log("menu ready: screen %dx%d, font %s, thai font %s", (int)w, (int)h, fontUsed, thaiUsed);
    g_lastT = NowSec();
    return true;
}

static void MenuFrame() {
    if (g_menuFailed) return;
    if (!g_menuInited) {
        if (!g_hGame || !g_pGameState) { g_menuFailed = true; return; }
        if (!InitMenu()) return;
        g_menuInited = true;
    }

    SyncBridgeEvidence();
    const int state = *g_pGameState;
    const bool pauseMenu = g_pMobileMenu && *reinterpret_cast<const int*>(g_pMobileMenu + 0x24) != 0;
    g_ev.gameState = state; g_ev.paused = pauseMenu ? 1 : 0;
    const bool show = (state != 7) && !pauseMenu;      // same rule as ProMenu: not in the front-end menu, not in the pause menu
    McUI::SetVisible(show);
    if (!show) return;

    float w = 0, h = 0;
    if (!ScreenSize(w, h)) return;

    const double now = NowSec();
    float dt = (float)(now - g_lastT);
    g_lastT = now;
    if (dt < 1.f / 240.f) dt = 1.f / 240.f;
    if (dt > 0.1f) dt = 0.1f;

    if (!RWB::NewFrame(w, h, dt)) return;
    ImGui::NewFrame();
    McUI::Draw(w, h, dt, g_ev);
    ImGui::Render();
    RWB::RenderDrawData(ImGui::GetDrawData());
}

DECL_HOOKv(ClockUpdate)
{
    ClockUpdate();                       // original first, then our work
    OnTick();
}

DECL_HOOKv(DebugDisplayTextBuffer)
{
    DebugDisplayTextBuffer();            // original first, then the menu is drawn on top of the HUD
    MenuFrame();
}

DECL_HOOKv(AND_TouchEvent, int type, int idx, int x, int y)
{
    if (g_menuInited && McUI::OnTouch(type, idx, x, y)) return;   // the menu consumed it
    AND_TouchEvent(type, idx, x, y);
}

DECL_HOOKv(ShutdownRenderWare)
{
    if (g_menuInited) RWB::DestroyFontRaster();     // recreated automatically on the next frame
    ShutdownRenderWare();
}

// ------------------------------------------------------------------ AML entry
ON_MOD_PRELOAD()
{
    logger->SetTag("MCBridge");
}

ON_MOD_LOAD()
{
    logger->SetTag("MCBridge");
    Diag::KeepPreviousSession();
    Diag::Crumb("MCBridge 0.4 loading");
    LoadConfig();
    g_ev.port = g_cfg.port;
    snprintf(g_ev.objName, sizeof(g_ev.objName), "%s", g_cfg.objName);

    g_hGame = aml->GetLibHandle("libGTASA.so");
    if (!g_hGame) { Diag::Crumb("libGTASA.so not found, plugin disabled"); return; }

    const char* kClock = "_ZN6CClock6UpdateEv";
    if (!GameSym(kClock)) { Diag::Crumb("CClock::Update not found (wrong game version?), plugin disabled"); return; }

    int found = 0, total = 0;
    g_pGameState  = reinterpret_cast<int*>(GameSym("gGameState"));
    g_pMobileMenu = reinterpret_cast<char*>(GameSym("gMobileMenu"));
    if (!g_pGameState) { Diag::Crumb("gGameState not found, plugin disabled"); return; }
    Res(g_findPlayerPed,     "_Z13FindPlayerPedi", found, total);
    Res(g_findPlayerVehicle, "_Z17FindPlayerVehicleib", found, total);
    Res(g_addPed,            "_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorb", found, total);
    Res(g_getModelInfo,      "_ZN10CModelInfo12GetModelInfoEPKcPi", found, total);
    Res(g_requestModel,      "_ZN10CStreaming12RequestModelEii", found, total);
    Res(g_loadAll,           "_ZN10CStreaming22LoadAllRequestedModelsEb", found, total);
    Res(g_objectCreate,      "_ZN7CObject6CreateEib", found, total);
    Res(g_worldAdd,          "_ZN6CWorld3AddEP7CEntity", found, total);
    Diag::Log("game symbols resolved: %d/%d (gMobileMenu %s)", found, total, g_pMobileMenu ? "ok" : "missing");
    g_ev.symFound = found; g_ev.symTotal = total;

    HOOKSYM(ClockUpdate, g_hGame, kClock);
    g_ev.hookInstalled = 1;
    const bool ok = Bridge::Start(g_cfg.port);
    g_ev.serverStarted = ok ? 1 : 0;
    g_ev.loaded = 1;
    Diag::Crumb("hooks installed; bridge %s on 127.0.0.1:%d", ok ? "started" : "FAILED to start", g_cfg.port);

    // the in-game menu: only hooked when every game symbol it needs exists, so a wrong game version cannot crash the game
    if (!g_cfg.menu) { Diag::Crumb("menu: off (menu=0 in config.txt)"); return; }
    g_pRsGlobal = reinterpret_cast<char*>(GameSym("RsGlobal"));
    const char* kTouch    = "_Z14AND_TouchEventiiii";
    const char* kFrame    = "_ZN6CDebug22DebugDisplayTextBufferEv";
    const char* kShutdown = "_ZN5CGame18ShutdownRenderWareEv";
    const bool rwOk = RWB::Resolve();
    if (rwOk && g_pRsGlobal && GameSym(kTouch) && GameSym(kFrame) && GameSym(kShutdown)) {
        HOOKSYM(AND_TouchEvent,         g_hGame, kTouch);
        HOOKSYM(DebugDisplayTextBuffer, g_hGame, kFrame);
        HOOKSYM(ShutdownRenderWare,     g_hGame, kShutdown);
        Diag::Crumb("menu: hooks installed (green MC button)");
    } else {
        Diag::Crumb("menu: NOT installed, a game symbol is missing (RsGlobal %s, see the log above)", g_pRsGlobal ? "ok" : "missing");
    }
}
