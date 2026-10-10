// MCBridge 0.2 - GTA SA side of the GTA x Minecraft bridge (AML plugin, arm64).
//
// What this version does (GTA side only; the Minecraft side does not exist yet):
//   1. resolves game functions BY NAME through AML (aml->GetSym), no hard-coded offsets
//   2. runs everything on the game thread via a CClock::Update hook (same hook point ProMenu uses)
//   3. opens a loopback TCP server (127.0.0.1:47800) and publishes the player position (see bridge_net.h)
//   4. executes test commands that arrive over the bridge, plus an optional one-shot self test
//   5. writes breadcrumbs + a session log to Android/data/com.rockstargames.gtasa/files/MCBridge/
//
// Status: written against the AML usage visible in ProMenu (menusan). NOT compiled with the real AML
// headers and NOT run in the game yet - the first CI build and the first log decide what is next.
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
#include "safemem.h"
#include "bridge_net.h"

MYMOD(net.mcgta.bridge, MCBridge, 0.2, MCGTA)
NEEDGAME(com.rockstargames.gtasa)

// ------------------------------------------------------------------ game access
static void* g_hGame = nullptr;
static uintptr_t GameSym(const char* name) { return aml->GetSym(g_hGame, name); }

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
            fprintf(f, "port=%d\nselftest_ped=%d\nselftest_obj=%d\nselftest_delay=%d\nobj_name=%s\nprobe=%s\n",
                    g_cfg.port, g_cfg.selftestPed, g_cfg.selftestObj, g_cfg.selftestDelay, g_cfg.objName, g_cfg.probe);
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
        else if (!strcmp(line, "obj_name")) snprintf(g_cfg.objName, sizeof(g_cfg.objName), "%s", v);
        else if (!strcmp(line, "probe")) snprintf(g_cfg.probe, sizeof(g_cfg.probe), "%s", v);
    }
    fclose(f);
}

// ------------------------------------------------------------------ test actions (game thread only)
static bool ActSpawnPed() {
    float p[3];
    if (!g_addPed || !PlayerPos(p)) { Diag::Crumb("spawn_ped: no AddPed or no player position"); return false; }
    const CVec at = { p[0] + 3.f, p[1], p[2] };
    Diag::Crumb("spawn_ped: calling AddPed at %.1f %.1f %.1f", at.x, at.y, at.z);
    void* ped = g_addPed(4 /*PEDTYPE_CIVMALE*/, 0 /*model 0 = player model, always loaded*/, at, false);
    Diag::Crumb("spawn_ped: AddPed returned %p", ped);
    float q[3];
    if (ped && EntityPos(ped, q)) Diag::Crumb("spawn_ped: new ped is at %.1f %.1f %.1f", q[0], q[1], q[2]);
    return ped != nullptr;
}

static void ActProbeModels(const char* list) {
    if (!g_getModelInfo) { Diag::Log("probe: GetModelInfo not available"); return; }
    char buf[160]; snprintf(buf, sizeof(buf), "%s", list);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        int id = -1;
        void* info = g_getModelInfo(tok, &id);
        Diag::Log("probe model '%s' -> info=%p id=%d", tok, info, id);
    }
}

static bool ActSpawnObject(const char* name) {
    float p[3];
    if (!g_getModelInfo || !g_requestModel || !g_loadAll || !g_objectCreate || !g_worldAdd || !PlayerPos(p)) {
        Diag::Crumb("spawn_obj: missing game function or player position"); return false;
    }
    int id = -1;
    void* info = g_getModelInfo(name, &id);
    Diag::Crumb("spawn_obj: '%s' info=%p id=%d", name, info, id);
    if (!info || id < 0) return false;
    Diag::Crumb("spawn_obj: RequestModel %d", id);
    g_requestModel(id, 2);                               // 2 = GAME_REQUIRED in DK22Pac plugin-sdk (assumed, not yet verified here)
    g_loadAll(false);
    Diag::Crumb("spawn_obj: CObject::Create");
    void* obj = g_objectCreate(id, false);
    Diag::Crumb("spawn_obj: Create returned %p", obj);
    if (!obj) return false;
    float at[3] = { p[0] + 3.f, p[1] + 3.f, p[2] };
    if (!SetEntityPos(obj, at)) { Diag::Crumb("spawn_obj: could not set position (layout?)"); return false; }
    Diag::Crumb("spawn_obj: CWorld::Add");
    g_worldAdd(obj);
    Diag::Crumb("spawn_obj: done");
    return true;
}

// ------------------------------------------------------------------ per-frame work
static unsigned g_playFrames = 0;
static bool g_selfTestDone = false;
static int  g_wantPed = 0, g_wantObj = 0;
static char g_wantObjName[48];
static unsigned long g_mcMsgs = 0;

static void HandleLine(const char* line) {
    if (!strcmp(line, "PING")) { Bridge::Say("PONG"); return; }
    if (!strcmp(line, "SPAWN_PED")) { g_wantPed = 1; return; }
    if (!strncmp(line, "SPAWN_OBJ ", 10)) { snprintf(g_wantObjName, sizeof(g_wantObjName), "%s", line + 10); g_wantObj = 1; return; }
    if ((line[0] == 'E' || line[0] == 'B') && line[1] == ' ') {            // draft Minecraft data: only counted for now
        if (++g_mcMsgs <= 20) Diag::Log("mc->gta: %s", line);
        return;
    }
    Diag::Log("bridge: unknown line '%s'", line);
    Bridge::Say("ERR unknown");
}

static void OnTick() {
    const int state = g_pGameState ? *g_pGameState : -1;
    const bool paused = g_pMobileMenu && *reinterpret_cast<const int*>(g_pMobileMenu + 0x24) != 0;
    if (state != 9 || paused) { g_playFrames = 0; return; }
    g_playFrames++;

    if (g_playFrames % 6 == 0) {                                          // ~10 Hz at 60 fps
        float p[3];
        if (PlayerPos(p)) Bridge::SetPlayer(p[0], p[1], p[2]);
    }

    char line[Bridge::kLine];
    int guard = 0;
    while (guard++ < 16 && Bridge::PopInbox(line)) HandleLine(line);

    if (g_wantPed) { g_wantPed = 0; Bridge::Say(ActSpawnPed() ? "OK SPAWN_PED" : "ERR SPAWN_PED"); }
    if (g_wantObj) { g_wantObj = 0; Bridge::Say(ActSpawnObject(g_wantObjName) ? "OK SPAWN_OBJ" : "ERR SPAWN_OBJ"); }

    if (!g_selfTestDone && g_playFrames >= (unsigned)g_cfg.selftestDelay) {
        g_selfTestDone = true;
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

DECL_HOOKv(ClockUpdate)
{
    ClockUpdate();                       // original first, then our work (same pattern as ProMenu)
    OnTick();
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
    Diag::Crumb("MCBridge 0.2 loading");
    LoadConfig();

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

    HOOKSYM(ClockUpdate, g_hGame, kClock);
    const bool ok = Bridge::Start(g_cfg.port);
    Diag::Crumb("hooks installed; bridge %s on 127.0.0.1:%d", ok ? "started" : "FAILED to start", g_cfg.port);
}
