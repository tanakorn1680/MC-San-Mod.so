#pragma once
// MCBridge's own in-game menu: Status / Progress / Tests / Log / Settings.
// Independent of every other mod (ProMenu included): its own ImGui context, its own floating button, its own touch
// handling. ImGui is used for DRAWING ONLY - touches come from the game's AND_TouchEvent and are never fed into
// ImGui's mouse state, so do NOT use ImGui widgets that need it (InputText, buttons, ...): they would draw but never react.
// The touch scheme is the one ProMenu uses in the game: the render side publishes the tappable rectangles, the touch
// side hit-tests them and queues "tap" ids, the render side reads the queue at the start of the next frame.
//
// Needs before it is included: diag.h (Diag::Path / Diag::Dir / Diag::Log).
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "imgui.h"
#include "mcstatus.h"

namespace McUI {

static const char* kVersion = "MCBridge 0.4";

// =============================================================== state
enum { kTabStatus = 0, kTabProgress, kTabTests, kTabLog, kTabSettings, kTabCount };
static const char* kTabEn[kTabCount] = {"Status", "Progress", "Tests", "Log", "Settings"};
static const char* kTabTh[kTabCount] = {"สถานะ", "ความคืบหน้า", "ทดสอบ", "ล็อก", "ตั้งค่า"};
static const char* const kMsNameTh[] = {
    "ปลั๊กอินโหลดในเกม GTA แล้ว", "เซิร์ฟเวอร์สะพานเปิดรอรับ", "อ่านตำแหน่งผู้เล่นได้", "สร้างคนเดินถนน (ทดสอบ)",
    "สร้างวัตถุ (ทดสอบ)", "Minecraft ต่อเข้าสะพานแล้ว", "รับข้อมูลเอนทิตี (บรรทัด E) จาก Minecraft",
    "รับข้อมูลบล็อก (บรรทัด B) จาก Minecraft", "ให้ตัวละคร GTA เดินตามผู้เล่น Minecraft (เฉพาะการเคลื่อนที่)",
    "วาดบล็อก Minecraft ในโลก GTA",
    "Steve และม็อบเป็นตัวละคร GTA โต้ตอบกับรถ/คนเดินถนน", "ซิงก์กล้องและเอฟเฟกต์สองทาง",
};
// what is missing / what to do, shown while a milestone is not done
static const char* const kHintEn[] = {
    "Check that libMCBridge64.so is in the AML mods folder and the game version has the hook symbols.",
    "The port may be in use by another app: change port= in MCBridge/config.txt.",
    "Start playing (not the front-end menu), then wait a moment.",
    "Open the Tests tab and run it while playing.",
    "Open the Tests tab and run it while playing. Riskier than the pedestrian test.",
    "Needs a Minecraft-side plugin that connects to this port and sends an ID line: ID minecraft <version>.",
    "Waits for E lines sent by the connected Minecraft client.",
    "Waits for B lines sent by the connected Minecraft client.",
    "Turn it on in the Tests tab once the Minecraft player position is arriving. Opt-in, off by default.",
    "No code yet. Needs real block data from the Minecraft side first.",
    "No code yet.",
    "No code yet.",
};
static const char* const kHintTh[] = {
    "เช็คว่าไฟล์ libMCBridge64.so อยู่ในโฟลเดอร์ mods ของ AML และเกมเวอร์ชันนี้มี symbol ที่ต้องใช้ครบ",
    "พอร์ตอาจถูกแอปอื่นใช้อยู่ ลองเปลี่ยน port= ใน MCBridge/config.txt",
    "เข้าเล่นเกมก่อน (ไม่ใช่หน้าเมนูหลัก) แล้วรอสักครู่",
    "เปิดแท็บ ทดสอบ แล้วกดตอนกำลังเล่นอยู่",
    "เปิดแท็บ ทดสอบ แล้วกดตอนกำลังเล่นอยู่ เสี่ยงกว่าการสร้างคนเดินถนน",
    "ต้องมีปลั๊กอินฝั่ง Minecraft ที่ต่อเข้าพอร์ตนี้ และส่งบรรทัด ID ก่อน: ID minecraft <เวอร์ชัน>",
    "รอบรรทัด E จาก Minecraft ที่ต่ออยู่",
    "รอบรรทัด B จาก Minecraft ที่ต่ออยู่",
    "เปิดในแท็บ ทดสอบ เมื่อตำแหน่งผู้เล่น Minecraft เริ่มเข้ามาแล้ว ปิดเป็นค่าเริ่มต้น ต้องเปิดเอง",
    "ยังไม่มีโค้ด ต้องมีข้อมูลบล็อกจริงจากฝั่ง Minecraft ก่อน",
    "ยังไม่มีโค้ด",
    "ยังไม่มีโค้ด",
};

static_assert(sizeof(kMsNameTh) / sizeof(kMsNameTh[0]) == Mc::kMsCount, "one Thai name per milestone");
static_assert(sizeof(kHintEn) / sizeof(kHintEn[0]) == Mc::kMsCount, "one English hint per milestone");
static_assert(sizeof(kHintTh) / sizeof(kHintTh[0]) == Mc::kMsCount, "one Thai hint per milestone");

static float g_sc = 1.f;                          // UI scale (screen short side / 1080)
static float g_slop = 48.f;                       // finger jitter allowed before a tap becomes a drag (pixels)
static float g_scrW = 0.f, g_scrH = 0.f, g_fabD = 100.f;
static volatile float g_fabX = 0.f, g_fabY = 0.f; // top-left of the floating button (written by the touch side while dragging)
static bool g_posInit = false;
static float g_cfgFx = -1.f, g_cfgFy = -1.f;      // saved button position as a fraction of the screen
static bool g_open = false;
static float g_openAnim = 0.f, g_idle = 0.f, g_fade = 1.f;
static int g_lang = -1;                           // 0 = English, 1 = Thai, -1 = not chosen yet
static bool g_thaiOk = false;                     // a Thai font was loaded
static int g_tab = kTabStatus, g_lastTab = -1;
static char g_toast[96] = "";
static float g_toastT = 0.f;
static bool g_expanded[4] = {true, false, false, false};   // Log page sections
static bool g_objArmed = false;                   // object test waits for its second tap
static float g_objArmedT = 0.f;

static const Mc::Evidence* g_evp = nullptr;       // the evidence of this frame
static Mc::Row g_rows[Mc::kMsCount];
static int g_done = 0;
static uint32_t g_ever = 0;
static unsigned g_runs = 0;

static volatile int g_dbgTouches = 0, g_dbgTaps = 0;
static volatile int g_dbgRaw[4] = {0, 0, 0, 0};   // raw event counts by type

static const char* T(const char* en, const char* th) { return (g_lang == 1 && g_thaiOk) ? th : en; }
static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double Now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static bool InRect(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
static void Toast(const char* text) { snprintf(g_toast, sizeof(g_toast), "%s", text); g_toastT = text[0] ? 2.2f : 0.f; }

// ------------------------------------------------------------ touch input
// Touch events arrive on the game's input callback, drawing happens on the render hook. Shared data is either a plain
// volatile value or protected by g_lock.
struct TapRect { float x0, y0, x1, y1; int id; };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<TapRect> g_rects;              // published by the render side, read by the touch side
static std::vector<TapRect> g_rectsBuild;         // being built this frame

static volatile bool g_visible = false, g_panelValid = false;
static volatile float g_pX0 = 0, g_pY0 = 0, g_pX1 = 0, g_pY1 = 0;   // panel rectangle
static volatile float g_cX0 = 0, g_cY0 = 0, g_cX1 = 0, g_cY1 = 0;   // scrollable content rectangle

static int g_finger = -1, g_target = 0;           // target: 1 = floating button, 2 = panel
static float g_downX = 0, g_downY = 0, g_lastX = 0, g_lastY = 0, g_startFabX = 0, g_startFabY = 0;
static bool g_moved = false, g_scrollArea = false;
static int g_downId = 0;
static float g_scrollDy = 0.f, g_velY = 0.f;
static volatile float g_fling = 0.f;
static double g_lastMoveT = 0.0, g_lastEventT = 0.0;
static volatile bool g_fabTap = false, g_saveReq = false;
static int g_tapQ[8];
static volatile int g_tqH = 0, g_tqT = 0;

static int FindId(float x, float y) {
    int id = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_rects.size(); i++) {
        const TapRect& r = g_rects[i];
        if (InRect(x, y, r.x0, r.y0, r.x1, r.y1)) { id = r.id; break; }
    }
    pthread_mutex_unlock(&g_lock);
    return id;
}

static void PushTap(int id) {
    pthread_mutex_lock(&g_lock);
    const int next = (g_tqT + 1) % 8;
    if (next != g_tqH) { g_tapQ[g_tqT] = id; g_tqT = next; }
    pthread_mutex_unlock(&g_lock);
}

static bool HitFab(float x, float y) {
    const float r = g_fabD * 0.5f + 18.f * g_sc;
    const float dx = x - (g_fabX + g_fabD * 0.5f), dy = y - (g_fabY + g_fabD * 0.5f);
    return dx * dx + dy * dy <= r * r;
}

// Touch event types of the game's AND_TouchEvent(type, finger, x, y): 1 = finger UP, 2 = finger DOWN, 3 = MOVE
// (read out of the original CheatMenu's touch hook and used the same way by ProMenu).
enum { kTouchUp = 1, kTouchDown = 2, kTouchMove = 3 };

static void ResetFinger() { g_finger = -1; g_target = 0; g_downId = 0; g_moved = false; }

// returns true when the touch belongs to this menu and must NOT reach the game
static bool OnTouch(int type, int idx, int x, int y) {
    if (type >= 1 && type <= 3) g_dbgRaw[type] = g_dbgRaw[type] + 1;
    if (!g_visible) return false;
    const float fx = (float)x, fy = (float)y;

    if (type == kTouchDown) {
        if (g_finger == idx) ResetFinger();            // we missed its UP: start over
        if (g_finger >= 0) return false;               // one menu finger at a time
        int target = 0, id = 0;
        if (HitFab(fx, fy)) target = 1;
        else if (g_panelValid && InRect(fx, fy, g_pX0, g_pY0, g_pX1, g_pY1)) { target = 2; id = FindId(fx, fy); }
        if (!target) return false;                     // not ours: the game (or another mod) gets it
        g_finger = idx; g_target = target;
        g_downX = g_lastX = fx; g_downY = g_lastY = fy;
        g_startFabX = g_fabX; g_startFabY = g_fabY;
        g_moved = false; g_downId = id;
        g_scrollArea = (target == 2) && InRect(fx, fy, g_cX0, g_cY0, g_cX1, g_cY1);
        g_velY = 0.f; g_fling = 0.f; g_lastMoveT = g_lastEventT = Now();
        g_dbgTouches = g_dbgTouches + 1;
        return true;
    }
    if (idx != g_finger) return false;                 // some other finger: not ours
    g_lastEventT = Now();

    if (type == kTouchMove) {
        const float mx = fx - g_downX, my = fy - g_downY;
        if (!g_moved && mx * mx + my * my > g_slop * g_slop) g_moved = true;
        if (g_moved) {
            if (g_target == 1) {
                g_fabX = Clamp(g_startFabX + mx, 0.f, g_scrW - g_fabD);
                g_fabY = Clamp(g_startFabY + my, 0.f, g_scrH - g_fabD);
            } else if (g_scrollArea) {
                const double t = Now();
                float dtm = (float)(t - g_lastMoveT);
                g_lastMoveT = t;
                if (dtm < 0.004f) dtm = 0.004f;
                const float dy = fy - g_lastY;
                g_velY = g_velY * 0.6f + Clamp(dy / dtm, -6000.f, 6000.f) * 0.4f;
                pthread_mutex_lock(&g_lock);
                g_scrollDy += dy;
                pthread_mutex_unlock(&g_lock);
            }
        }
        g_lastX = fx; g_lastY = fy;
        return true;
    }

    if (type == kTouchUp) {
        if (!g_moved) {
            if (g_target == 1) g_fabTap = true;
            else {
                const int id = FindId(fx, fy);
                if (id && id == g_downId) PushTap(id);
            }
        } else if (g_target == 1) {
            g_saveReq = true;                          // remember where the button was dropped
        } else if (g_scrollArea && (Now() - g_lastMoveT) < 0.08) {
            g_fling = g_velY;                          // let the list glide on
        }
        ResetFinger();
        return true;
    }
    return false;
}

// called by the render hook every frame before Draw()
static void SetVisible(bool visible) {
    if (g_visible && !visible && g_finger >= 0) ResetFinger();
    g_visible = visible;
    if (!visible) g_panelValid = false;
}

// ------------------------------------------------------------ settings file (menu.cfg: language + button position)
static void LoadConfig() {
    char path[256]; Diag::Path(path, sizeof(path), "menu.cfg");
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        int iv = 0; float fv = 0.f;
        if (sscanf(line, "lang=%d", &iv) == 1) g_lang = (iv == 1) ? 1 : 0;
        else if (sscanf(line, "fab_x=%f", &fv) == 1) g_cfgFx = Clamp(fv, 0.f, 1.f);
        else if (sscanf(line, "fab_y=%f", &fv) == 1) g_cfgFy = Clamp(fv, 0.f, 1.f);
    }
    fclose(f);
}

static void SaveConfig(float W, float H) {
    char path[256]; Diag::Path(path, sizeof(path), "menu.cfg");
    FILE* f = fopen(path, "w");
    if (!f) return;
    g_cfgFx = (W > 0.f) ? g_fabX / W : -1.f;
    g_cfgFy = (H > 0.f) ? g_fabY / H : -1.f;
    fprintf(f, "lang=%d\nfab_x=%.4f\nfab_y=%.4f\n", g_lang == 1 ? 1 : 0, g_cfgFx, g_cfgFy);
    fclose(f);
}

// ------------------------------------------------------------ log files + report
static char g_logBuf[3][8192];                    // 0 = last_action.txt, 1 = session.log (tail), 2 = previous_session.txt
static bool g_logLoaded = false;
static char g_savedPath[256] = "";

static void ReadTail(const char* file, char* out, size_t cap) {
    char path[256]; Diag::Path(path, sizeof(path), file);
    FILE* f = fopen(path, "r");
    if (!f) { snprintf(out, cap, "(not found: %s)", path); return; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    const long want = (long)cap - 1;
    const long start = (sz > want) ? (sz - want) : 0;
    fseek(f, start, SEEK_SET);
    const size_t n = fread(out, 1, (size_t)(sz - start), f);
    out[n] = 0;
    fclose(f);
}

static void RefreshLogs() {
    ReadTail("last_action.txt", g_logBuf[0], sizeof(g_logBuf[0]));
    ReadTail("session.log", g_logBuf[1], sizeof(g_logBuf[1]));
    ReadTail("previous_session.txt", g_logBuf[2], sizeof(g_logBuf[2]));
    g_logLoaded = true;
}

// One plain-text file with the progress table + the three logs: easy to open with any file manager and send on.
static void SaveReport() {
    RefreshLogs();
    char path[256]; Diag::Path(path, sizeof(path), "report.txt");
    FILE* f = fopen(path, "w");
    if (!f) { g_savedPath[0] = 0; return; }
    fprintf(f, "%s report\n\n", kVersion);
    fprintf(f, "=== Progress: %d of %d milestones (this run + earlier runs) ===\n", g_done, Mc::kMsCount);
    for (int i = 0; i < Mc::kMsCount; i++)
        fprintf(f, "[%s%s] %s - %s\n", Mc::StateName(g_rows[i].st), g_rows[i].earlier ? ", proven in an earlier run" : "",
                Mc::kMsName[i], g_rows[i].detail);
    if (g_evp) {
        const Mc::Evidence& e = *g_evp;
        fprintf(f, "\n=== Live evidence ===\n");
        fprintf(f, "game state %d%s, symbols %d/%d, server listening %d (port %d, bind errno %d)\n", e.gameState, e.paused ? " (paused)" : "",
                e.symFound, e.symTotal, e.listening, e.port, e.bindErrno);
        fprintf(f, "client connected %d, name \"%s\", connections %lu, lines rx %lu tx %lu dropped %lu\n", e.connected, e.clientName,
                e.accepted, e.rx, e.tx, e.dropped);
        fprintf(f, "E lines %lu, B lines %lu (from Minecraft); %lu from other clients\n", e.entMsgs, e.blkMsgs, e.otherDataMsgs);
        fprintf(f, "player position reads %lu, last %.2f %.2f %.2f\n", e.posReads, e.pos[0], e.pos[1], e.pos[2]);
        for (int i = 0; i < e.nProbe; i++) fprintf(f, "probe model '%s' -> id %d\n", e.probe[i].name, e.probe[i].id);
    }
    fprintf(f, "\n=== Last steps (last_action.txt) ===\n%s\n\n", g_logBuf[0]);
    fprintf(f, "=== Full log, tail (session.log) ===\n%s\n\n", g_logBuf[1]);
    fprintf(f, "=== Previous run (previous_session.txt) ===\n%s\n", g_logBuf[2]);
    fclose(f);
    snprintf(g_savedPath, sizeof(g_savedPath), "%s", path);
}

// ------------------------------------------------------------ drawing helpers
static int g_fired[8];
static int g_nFired = 0;
static bool Fired(int id) { for (int i = 0; i < g_nFired; i++) if (g_fired[i] == id) return true; return false; }

static void Reg(ImVec2 p, ImVec2 q, int id) {
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    TapRect r = { fmaxf(p.x, wp.x), fmaxf(p.y, wp.y), fminf(q.x, wp.x + ws.x), fminf(q.y, wp.y + ws.y), id };
    if (r.x1 > r.x0 && r.y1 > r.y0) g_rectsBuild.push_back(r);
}
static bool Pressed(int id) { return g_finger >= 0 && !g_moved && g_downId == id; }
static float RowWidth() { return ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 4.f * g_sc; }

static const ImU32 kBtnCol    = IM_COL32(40, 48, 68, 255);
static const ImU32 kAccentCol = IM_COL32(52, 120, 246, 255);
static const ImU32 kDangerCol = IM_COL32(86, 44, 52, 255);
static const ImU32 kCardCol   = IM_COL32(26, 34, 54, 255);
static const ImU32 kWhite     = IM_COL32(255, 255, 255, 255);
static const ImU32 kDim       = IM_COL32(150, 160, 185, 255);
static const ImU32 kGreen     = IM_COL32(36, 170, 90, 255);
static const ImU32 kAmber     = IM_COL32(214, 160, 40, 255);
static const ImU32 kRed       = IM_COL32(200, 60, 60, 255);
static const ImU32 kGrey      = IM_COL32(70, 75, 92, 255);
static const ImU32 kBlueGrey  = IM_COL32(80, 100, 150, 255);

static ImU32 StateColor(Mc::State s) {
    switch (s) {
        case Mc::kDone: return kGreen;
        case Mc::kWaiting: return kAmber;
        case Mc::kFailed: return kRed;
        case Mc::kNotStarted: return kGrey;
        default: return kBlueGrey;
    }
}
static const char* StateLabel(Mc::State s) {
    switch (s) {
        case Mc::kDone: return T("DONE", "สำเร็จ");
        case Mc::kWaiting: return T("WAITING", "รอ");
        case Mc::kFailed: return T("FAILED", "ล้มเหลว");
        case Mc::kNotStarted: return T("NOT STARTED", "ยังไม่เริ่ม");
        default: return T("UNTESTED", "ยังไม่ทดสอบ");
    }
}

// A button drawn by hand. It is "tapped" when the raw touch logic reports its id.
static bool Btn(const char* label, ImVec2 size, int id, ImU32 fill, bool enabled = true) {
    const float sc = g_sc;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 q(p.x + size.x, p.y + size.y);
    if (enabled && id) Reg(p, q, id);

    ImU32 col = (enabled && Pressed(id)) ? kAccentCol : fill;
    if (!enabled) col = IM_COL32(30, 34, 46, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, col, 18.f * sc);

    const ImU32 tc = enabled ? kWhite : IM_COL32(120, 125, 140, 255);
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float maxW = size.x - 36.f * sc;
    float k = 1.f;
    if (ts.x > maxW && ts.x > 0.f) k = maxW / ts.x;
    const float tw = ts.x * k, th = ts.y * k;
    dl->AddText(ImGui::GetFont(), fs * k, ImVec2(p.x + (size.x - tw) * 0.5f, p.y + (size.y - th) * 0.5f), tc, label);
    return enabled && Fired(id);
}

// collapsible section bar; returns true when tapped
static bool Header(const char* label, int id, bool expanded) {
    const float sc = g_sc;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = RowWidth(), h = 68.f * sc;
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 q(p.x + w, p.y + h);
    Reg(p, q, id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, Pressed(id) ? IM_COL32(35, 60, 110, 255) : kCardCol, 16.f * sc);
    const float cx = p.x + 36.f * sc, cy = p.y + h * 0.5f, r = 11.f * sc;
    const ImU32 ac = IM_COL32(111, 160, 255, 255);
    if (expanded) dl->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f), ImVec2(cx, cy + r * 0.8f), ac);
    else          dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r), ImVec2(cx - r * 0.6f, cy + r), ImVec2(cx + r * 0.8f, cy), ac);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 70.f * sc, p.y + (h - ts.y) * 0.5f), kWhite, label);
    return Fired(id);
}

static bool Section(const char* label, int idx, int id) {
    if (Header(label, id, g_expanded[idx])) g_expanded[idx] = !g_expanded[idx];
    return g_expanded[idx];
}

// A read-only card: title + status chip on the first row, then up to two wrapped lines of smaller text.
static void Card(const char* title, const char* chip, ImU32 chipCol, const char* line1, const char* line2) {
    const float sc = g_sc, w = RowWidth(), pad = 22.f * sc, k = 0.78f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 cs = ImGui::CalcTextSize(chip);
    const float chipW = cs.x + 28.f * sc, chipH = cs.y + 12.f * sc;
    const float titleW = w - pad * 2.f - chipW - 14.f * sc;
    const float textW = w - pad * 2.f;
    const ImVec2 ts = ImGui::CalcTextSize(title, nullptr, false, titleW);
    const bool has1 = line1 && line1[0], has2 = line2 && line2[0];
    const float h1 = has1 ? ImGui::CalcTextSize(line1, nullptr, false, textW / k).y * k : 0.f;
    const float h2 = has2 ? ImGui::CalcTextSize(line2, nullptr, false, textW / k).y * k : 0.f;
    const float top = fmaxf(ts.y, chipH);
    const float h = pad + top + (has1 ? 8.f * sc + h1 : 0.f) + (has2 ? 8.f * sc + h2 : 0.f) + pad;

    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kCardCol, 18.f * sc);
    float y = p.y + pad;
    dl->AddText(font, fs, ImVec2(p.x + pad, y), kWhite, title, nullptr, titleW);
    const ImVec2 ca(p.x + w - pad - chipW, y);
    dl->AddRectFilled(ca, ImVec2(ca.x + chipW, ca.y + chipH), chipCol, chipH * 0.5f);
    dl->AddText(ImVec2(ca.x + 14.f * sc, ca.y + 6.f * sc), kWhite, chip);
    y += top;
    if (has1) { y += 8.f * sc; dl->AddText(font, fs * k, ImVec2(p.x + pad, y), kDim, line1, nullptr, textW); y += h1; }
    if (has2) { y += 8.f * sc; dl->AddText(font, fs * k, ImVec2(p.x + pad, y), IM_COL32(230, 190, 110, 255), line2, nullptr, textW); }
}

// one "label ... value" row with a coloured dot
static void Line(const char* label, const char* value, ImU32 dot) {
    const float sc = g_sc, w = RowWidth(), k = 0.86f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float labelX = 46.f * sc, valueX = w * 0.40f, valueW = w - valueX - 12.f * sc;
    const ImVec2 ls = ImGui::CalcTextSize(label);
    const float vh = ImGui::CalcTextSize(value, nullptr, false, valueW / k).y * k;
    const float h = fmaxf(ls.y, vh) + 20.f * sc;
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + 20.f * sc, p.y + h * 0.5f), 9.f * sc, dot, 20);
    dl->AddText(ImVec2(p.x + labelX, p.y + 10.f * sc), kDim, label);
    dl->AddText(font, fs * k, ImVec2(p.x + valueX, p.y + 10.f * sc), kWhite, value, nullptr, valueW);
}

static void Note(const char* text, ImU32 col) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + RowWidth());
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------ pages
static void PageStatus() {
    const Mc::Evidence& e = *g_evp;
    const float sc = g_sc, w = RowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    char buf[160];

    {   // overall progress card
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 176.f * sc;
        ImGui::Dummy(ImVec2(w, h));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kCardCol, 20.f * sc);
        snprintf(buf, sizeof(buf), T("Progress: %d of %d steps", "ความคืบหน้า: %d จาก %d ขั้น"), g_done, Mc::kMsCount);
        dl->AddText(ImVec2(p.x + 24.f * sc, p.y + 18.f * sc), kWhite, buf);
        const float bx0 = p.x + 24.f * sc, bx1 = p.x + w - 24.f * sc, by0 = p.y + 78.f * sc, by1 = by0 + 26.f * sc;
        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(12, 16, 28, 255), 13.f * sc);
        if (g_done > 0)
            dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx0 + (bx1 - bx0) * ((float)g_done / (float)Mc::kMsCount), by1), kGreen, 13.f * sc);
        const char* sub = T("Counts only what was proven on this phone.", "นับเฉพาะสิ่งที่พิสูจน์ได้จริงบนเครื่องนี้");
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.74f, ImVec2(p.x + 24.f * sc, p.y + 118.f * sc), kDim, sub, nullptr, w - 48.f * sc);
    }

    ImGui::TextColored(ImVec4(0.45f, 0.62f, 1.f, 1.f), "%s", T("Right now", "ตอนนี้"));
    {
        const char* gs = (e.gameState == 9) ? T("Playing", "กำลังเล่น") : (e.gameState == 7) ? T("Front-end menu", "หน้าเมนูหลัก") : T("Other state", "สถานะอื่น");
        snprintf(buf, sizeof(buf), "%s (%d)%s", gs, e.gameState, e.paused ? T(", paused", ", หยุดชั่วคราว") : "");
        Line(T("Game", "เกม"), buf, (e.gameState == 9 && !e.paused) ? kGreen : kAmber);
    }
    Line(T("Plugin", "ปลั๊กอิน"), g_rows[Mc::kMsPlugin].detail, StateColor(g_rows[Mc::kMsPlugin].st));
    Line(T("Server", "เซิร์ฟเวอร์"), g_rows[Mc::kMsServer].detail, StateColor(g_rows[Mc::kMsServer].st));
    Line(T("Client", "ไคลเอนต์"), g_rows[Mc::kMsMcConn].detail, StateColor(g_rows[Mc::kMsMcConn].st));
    snprintf(buf, sizeof(buf), "rx %lu   tx %lu   dropped %lu", e.rx, e.tx, e.dropped);
    Line(T("Traffic", "ข้อมูลที่ส่งรับ"), buf, e.rx > 0 ? kGreen : kGrey);
    if (e.otherDataMsgs) snprintf(buf, sizeof(buf), "E %lu   B %lu   (+%lu %s)", e.entMsgs, e.blkMsgs, e.otherDataMsgs, T("from test clients", "จากตัวทดสอบ"));
    else snprintf(buf, sizeof(buf), "E %lu   B %lu", e.entMsgs, e.blkMsgs);
    Line(T("Minecraft data", "ข้อมูล Minecraft"), buf, (e.entMsgs + e.blkMsgs) > 0 ? kGreen : kGrey);
    Line(T("Player", "ผู้เล่น"), g_rows[Mc::kMsPos].detail, StateColor(g_rows[Mc::kMsPos].st));
    if (e.mcPosCount > 0) {
        snprintf(buf, sizeof(buf), "%.1f  %.1f  %.1f   (%lu)", e.mcPos[0], e.mcPos[1], e.mcPos[2], e.mcPosCount);
        Line(T("Minecraft player", "ผู้เล่น Minecraft"), buf, kGreen);
    }
    if (e.mcStatus[0]) Line(T("Minecraft side", "ฝั่ง Minecraft"), e.mcStatus, kGreen);
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.45f, 0.62f, 1.f, 1.f), "%s", T("Next step", "ขั้นถัดไป"));
    int next = -1;
    for (int i = 0; i < Mc::kMsCount; i++) if (g_rows[i].st != Mc::kDone && !g_rows[i].earlier) { next = i; break; }
    if (next < 0) Note(T("Every step that has code is done.", "ทุกขั้นที่มีโค้ดผ่านแล้ว"), kDim);
    else Card(T(Mc::kMsName[next], kMsNameTh[next]), StateLabel(g_rows[next].st), StateColor(g_rows[next].st),
              g_rows[next].detail, T(kHintEn[next], kHintTh[next]));
    ImGui::Dummy(ImVec2(1.f, 90.f * sc));
}

static void PageProgress() {
    const float sc = g_sc;
    Note(T("Each step turns DONE only from live evidence. Steps with no code stay NOT STARTED.",
           "แต่ละขั้นจะเป็น 'สำเร็จ' ได้จากหลักฐานจริงเท่านั้น ขั้นที่ยังไม่มีโค้ดจะเป็น 'ยังไม่เริ่ม'"), kDim);
    ImGui::Spacing();
    char line1[160];
    for (int i = 0; i < Mc::kMsCount; i++) {
        const Mc::Row& r = g_rows[i];
        const bool earlierOnly = r.earlier;
        const char* chip = earlierOnly ? T("EARLIER RUN", "เคยผ่านแล้ว") : StateLabel(r.st);
        const ImU32 col = earlierOnly ? IM_COL32(30, 120, 80, 255) : StateColor(r.st);
        if (earlierOnly) snprintf(line1, sizeof(line1), "%s %s", T("Proven in an earlier run. Now:", "เคยพิสูจน์แล้วในรอบก่อน ตอนนี้:"), r.detail);
        else snprintf(line1, sizeof(line1), "%s", r.detail);
        const bool needHint = (r.st != Mc::kDone);
        Card(T(Mc::kMsName[i], kMsNameTh[i]), chip, col, line1, needHint ? T(kHintEn[i], kHintTh[i]) : nullptr);
        ImGui::Spacing();
    }
    ImGui::Dummy(ImVec2(1.f, 90.f * sc));
}

static void PageTests() {
    const Mc::Evidence& e = *g_evp;
    const float sc = g_sc, w = RowWidth(), btnH = 96.f * sc;
    const bool canRun = (e.gameState == 9) && !e.paused;
    char buf[160];

    Note(T("Run these while playing, not in a menu. Results go to the Progress tab and to the log.",
           "กดตอนกำลังเล่นอยู่ ไม่ใช่ตอนอยู่ในเมนู ผลจะขึ้นในแท็บ ความคืบหน้า และถูกบันทึกลงล็อก"), kDim);
    if (!canRun) Note(T("Not playing right now: the buttons are off.", "ตอนนี้ยังไม่ได้เล่น ปุ่มจึงถูกปิดอยู่"), kAmber);
    ImGui::Spacing();

    if (Btn(T("Check model names (safe)", "ตรวจชื่อโมเดล (ปลอดภัย)"), ImVec2(w, btnH), 50, kBtnCol, canRun)) { Mc::g_req.probe = 1; Toast(T("Model check queued", "สั่งตรวจชื่อโมเดลแล้ว")); }
    if (Btn(T("Spawn a pedestrian", "สร้างคนเดินถนน"), ImVec2(w, btnH), 51, kBtnCol, canRun)) { Mc::g_req.ped = 1; Toast(T("Pedestrian test queued", "สั่งทดสอบคนเดินถนนแล้ว")); }
    snprintf(buf, sizeof(buf), "%s (%s)", T("Spawn object (risky)", "สร้างวัตถุ (เสี่ยง)"), e.objName);
    if (Btn(g_objArmed ? T("TAP AGAIN to confirm", "แตะอีกครั้งเพื่อยืนยัน") : buf, ImVec2(w, btnH), 52, g_objArmed ? kRed : kDangerCol, canRun)) {
        if (g_objArmed) { g_objArmed = false; Mc::g_req.obj = 1; Toast(T("Object test queued", "สั่งทดสอบวัตถุแล้ว")); }
        else { g_objArmed = true; g_objArmedT = 3.f; }
    }
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.45f, 0.62f, 1.f, 1.f), "%s", T("Minecraft link", "ลิงก์กับ Minecraft"));
    {
        const bool haveMc = e.mcPosCount > 0;
        snprintf(buf, sizeof(buf), "%s: %s", T("Mirror the Minecraft player", "ให้ตัวละคร GTA เดินตาม Minecraft"), e.mirrorOn ? T("ON", "เปิด") : T("OFF", "ปิด"));
        if (Btn(buf, ImVec2(w, btnH), 53, e.mirrorOn ? kAccentCol : kBtnCol, canRun && (haveMc || e.mirrorOn))) {
            Mc::g_req.mirror = 1;
            Toast(e.mirrorOn ? T("Mirror turned off", "ปิดการเดินตามแล้ว") : T("Mirror turned on", "เปิดการเดินตามแล้ว"));
        }
        if (!haveMc && !e.mirrorOn) Note(T("Needs the Minecraft player position first (see the Status tab).", "ต้องมีตำแหน่งผู้เล่น Minecraft เข้ามาก่อน (ดูแท็บ สถานะ)"), kDim);
        else Note(T("Opt-in. A second character copies your Minecraft movement. If the game ever removes that character it could crash: turn it off when done.",
                    "ต้องเปิดเอง ตัวละครตัวที่สองจะเดินตามการเคลื่อนที่ใน Minecraft ถ้าเกมลบตัวละครนี้ทิ้งอาจทำให้เกมเด้งได้ เลิกใช้แล้วให้ปิด"), kDim);
        const Mc::Row& r = g_rows[Mc::kMsMirror];
        Card(T("Mirror", "เดินตาม"), StateLabel(r.st), StateColor(r.st), r.detail, nullptr);
    }
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.45f, 0.62f, 1.f, 1.f), "%s", T("Results", "ผลลัพธ์"));
    {
        const Mc::Row& r = g_rows[Mc::kMsPed];
        Card(T("Pedestrian", "คนเดินถนน"), StateLabel(r.st), StateColor(r.st), r.detail, nullptr);
    }
    ImGui::Spacing();
    {
        const Mc::Row& r = g_rows[Mc::kMsObj];
        Card(T("Object", "วัตถุ"), StateLabel(r.st), StateColor(r.st), r.detail, nullptr);
    }
    ImGui::Spacing();
    if (e.nProbe > 0) {
        char list[640]; list[0] = 0;
        for (int i = 0; i < e.nProbe; i++) {
            char one[72];
            if (e.probe[i].id >= 0) snprintf(one, sizeof(one), "%s: id %d\n", e.probe[i].name, e.probe[i].id);
            else snprintf(one, sizeof(one), "%s: %s\n", e.probe[i].name, T("not found", "ไม่พบ"));
            strncat(list, one, sizeof(list) - strlen(list) - 1);
        }
        Card(T("Model names", "ชื่อโมเดล"), "", kGrey, list, nullptr);
    }
    ImGui::Dummy(ImVec2(1.f, 90.f * sc));
}

static void LogBlock(const char* title, const char* text, int idx, int id) {
    if (Section(title, idx, id)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + RowWidth());
        ImGui::TextUnformatted(text[0] ? text : "(empty)");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
}

static void PageLog() {
    const float sc = g_sc, w = RowWidth(), btnH = 96.f * sc, sp = ImGui::GetStyle().ItemSpacing.x;
    if (!g_logLoaded) RefreshLogs();
    const float hw = (w - sp) * 0.5f;
    if (Btn(T("Refresh", "รีเฟรช"), ImVec2(hw, btnH), 30, kAccentCol)) RefreshLogs();
    ImGui::SameLine();
    if (Btn(T("Save report", "บันทึกรายงาน"), ImVec2(hw, btnH), 31, kBtnCol)) { SaveReport(); Toast(g_savedPath[0] ? T("Report saved", "บันทึกรายงานแล้ว") : T("Could not save the report", "บันทึกรายงานไม่สำเร็จ")); }
    ImGui::Spacing();
    Note(T("MCBridge's own log files. The report bundles the progress table and these logs into report.txt, so it is easy to send on.",
           "ไฟล์ล็อกของ MCBridge เอง ปุ่มบันทึกรายงานจะรวมตารางความคืบหน้าและล็อกทั้งหมดไว้ใน report.txt เพื่อส่งต่อได้ง่าย"), kDim);
    if (g_savedPath[0]) {
        Note(g_savedPath, IM_COL32(115, 190, 128, 255));
        Note(T("Open that file with any file manager to share or copy it.", "เปิดไฟล์นั้นด้วยตัวจัดการไฟล์ใดก็ได้เพื่อแชร์หรือคัดลอก"), kDim);
    }
    ImGui::Spacing();
    LogBlock(T("Last steps (last_action.txt)", "ขั้นล่าสุด (last_action.txt)"), g_logBuf[0], 0, 100);
    LogBlock(T("Full log, tail (session.log)", "ล็อกเต็ม ส่วนท้าย (session.log)"), g_logBuf[1], 1, 101);
    LogBlock(T("Previous run (previous_session.txt)", "รอบก่อนหน้า (previous_session.txt)"), g_logBuf[2], 2, 102);
    ImGui::Dummy(ImVec2(1.f, 90.f * sc));
}

static void PageSettings() {
    const Mc::Evidence& e = *g_evp;
    const float sc = g_sc, w = RowWidth(), btnH = 96.f * sc, sp = ImGui::GetStyle().ItemSpacing.x;
    const ImVec4 hdr(0.45f, 0.62f, 1.f, 1.f);
    const ImVec4 dim(0.55f, 0.60f, 0.70f, 1);

    ImGui::TextColored(hdr, "%s", g_thaiOk ? "Language / ภาษา" : "Language");
    const float lw = (w - sp) * 0.5f;
    if (Btn("English", ImVec2(lw, btnH), 40, (g_lang != 1) ? kAccentCol : kBtnCol)) { g_lang = 0; g_saveReq = true; }
    ImGui::SameLine();
    if (Btn(g_thaiOk ? "ไทย" : "Thai", ImVec2(lw, btnH), 41, (g_lang == 1) ? kAccentCol : kBtnCol, g_thaiOk)) { g_lang = 1; g_saveReq = true; }
    if (!g_thaiOk) Note("Thai font not found. Put a Thai .ttf at files/MCBridge/font_th.ttf", kDim);
    ImGui::Spacing();

    ImGui::TextColored(hdr, "%s", T("Floating button", "ปุ่มลอย"));
    if (Btn(T("Reset position", "รีเซ็ตตำแหน่ง"), ImVec2(w, btnH), 42, kBtnCol)) { g_posInit = false; g_cfgFx = g_cfgFy = -1.f; g_saveReq = true; }
    ImGui::Spacing();

    ImGui::TextColored(hdr, "%s", T("About", "เกี่ยวกับ"));
    ImGui::TextColored(dim, "%s", kVersion);
    ImGui::TextColored(dim, "%s %d    %s %d/%d", T("Game state:", "สถานะเกม:"), e.gameState, T("symbols", "symbol"), e.symFound, e.symTotal);
    ImGui::TextColored(dim, "%s %u    %s 0x%x", T("Runs on this phone:", "จำนวนรอบที่เปิดเกม:"), g_runs, T("proven bits", "บิตที่พิสูจน์แล้ว"), (unsigned)g_ever);
    ImGui::TextColored(dim, "%s %d    %s %d", T("Touches:", "แตะที่เมนู:"), (int)g_dbgTouches, T("Taps:", "แตะปุ่ม:"), (int)g_dbgTaps);
    ImGui::TextColored(dim, "raw up %d  down %d  move %d", (int)g_dbgRaw[1], (int)g_dbgRaw[2], (int)g_dbgRaw[3]);
    const char* dir = Diag::Dir();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + RowWidth());
    ImGui::TextColored(dim, "%s %s", T("Data folder:", "โฟลเดอร์ข้อมูล:"), dir ? dir : "(none)");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(1.f, 60.f * sc));
}

// ------------------------------------------------------------ setup + per-frame draw
static void Init(float scale, bool thaiOk) {
    g_sc = scale;
    g_thaiOk = thaiOk;
    g_slop = fmaxf(40.f, 48.f * scale);
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 28.f * scale;
    st.ChildRounding = 20.f * scale;
    st.ScrollbarSize = 8.f * scale;
    st.ScrollbarRounding = 4.f * scale;
    st.ItemSpacing = ImVec2(14.f * scale, 14.f * scale);
    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 0.f;
    st.Colors[ImGuiCol_ChildBg]       = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarBg]   = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.22f);

    LoadConfig();
    if (g_lang < 0) g_lang = thaiOk ? 1 : 0;       // first start: Thai when a Thai font is available (changeable in Settings)
    char p[256]; Diag::Path(p, sizeof(p), "progress.txt");
    g_ever = Mc::LoadEver(p, &g_runs);
    g_runs++;
    Mc::SaveEver(p, g_ever, g_runs);
}

static void Draw(float W, float H, float dt, const Mc::Evidence& ev) {
    const float sc = g_sc;
    g_scrW = W; g_scrH = H;
    const float D = 110.f * sc;
    g_fabD = D;

    if (!g_posInit) {
        g_fabX = (g_cfgFx >= 0.f) ? g_cfgFx * W : 24.f * sc;          // default: left edge (ProMenu's button defaults to the right)
        g_fabY = (g_cfgFy >= 0.f) ? g_cfgFy * H : H * 0.55f;
        g_posInit = true;
    }
    g_fabX = Clamp(g_fabX, 0.f, W - D);
    g_fabY = Clamp(g_fabY, 0.f, H - D);

    // evidence -> rows; remember (on disk) what has been proven live on this phone
    g_evp = &ev;
    {
        const uint32_t live = Mc::LiveBits(ev);
        if (live & ~g_ever) {
            g_ever |= live;
            char p[256]; Diag::Path(p, sizeof(p), "progress.txt");
            Mc::SaveEver(p, g_ever, g_runs);
        }
        g_done = Mc::Evaluate(ev, g_ever, g_rows);
    }

    // what the touch side reported since the last frame
    float scrollDy = 0.f;
    pthread_mutex_lock(&g_lock);
    g_nFired = 0;
    while (g_tqH != g_tqT && g_nFired < 8) { g_fired[g_nFired++] = g_tapQ[g_tqH]; g_tqH = (g_tqH + 1) % 8; }
    scrollDy = g_scrollDy; g_scrollDy = 0.f;
    pthread_mutex_unlock(&g_lock);
    g_rectsBuild.clear();
    g_dbgTaps = g_dbgTaps + g_nFired;
    if (g_objArmedT > 0.f) { g_objArmedT -= dt; if (g_objArmedT <= 0.f) g_objArmed = false; }

    if (g_finger >= 0 && Now() - g_lastEventT > 5.0) ResetFinger();      // lost UP event: never stay stuck
    if (g_fabTap) { g_fabTap = false; g_open = !g_open; Toast(""); }
    if (g_saveReq) { g_saveReq = false; SaveConfig(W, H); }

    // ---- floating button (always on top, stays where it was dropped) -----
    if (g_finger >= 0 || g_open) g_idle = 0.f; else g_idle += dt;
    const float fadeTarget = (g_idle > 3.f) ? 0.45f : 1.f;
    g_fade += (fadeTarget - g_fade) * fminf(1.f, dt * 5.f);
    const float alpha = 0.92f * g_fade;
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const bool down = (g_finger >= 0 && g_target == 1);
        const ImVec2 c(g_fabX + D * 0.5f, g_fabY + D * 0.5f);
        const float r = down ? D * 0.47f : D * 0.5f;
        #define A(base) ((int)((base) * alpha))
        fg->AddCircleFilled(ImVec2(c.x, c.y + 5.f * sc), r + sc, IM_COL32(0, 0, 0, A(90)), 48);
        // green (ProMenu's button is blue) so the two are never confused
        ImU32 body = g_open ? IM_COL32(235, 87, 87, A(255)) : IM_COL32(40, 160, 90, A(255));
        if (down) body = g_open ? IM_COL32(200, 60, 60, A(255)) : IM_COL32(28, 130, 70, A(255));
        fg->AddCircleFilled(c, r, body, 48);
        fg->AddCircle(c, r - 2.f * sc, IM_COL32(255, 255, 255, A(70)), 48, 2.f * sc);
        const ImU32 ic = IM_COL32(255, 255, 255, A(255));
        if (g_open) {
            const float s = r * 0.38f, th = 5.f * sc;
            fg->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ic, th);
            fg->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), ic, th);
        } else {
            const float fs = ImGui::GetFontSize() * 1.05f;
            const ImVec2 ts = ImGui::CalcTextSize("MC");
            fg->AddText(ImGui::GetFont(), fs, ImVec2(c.x - ts.x * 1.05f * 0.5f, c.y - ts.y * 1.05f * 0.5f), ic, "MC");
        }
        #undef A
    }

    // ---- panel --------------------------------------------------------------
    g_openAnim += ((g_open ? 1.f : 0.f) - g_openAnim) * fminf(1.f, dt * 14.f);
    if (g_toastT > 0.f) g_toastT -= dt;
    if (!g_open && g_openAnim < 0.02f) {
        g_panelValid = false;
        pthread_mutex_lock(&g_lock); g_rects.clear(); pthread_mutex_unlock(&g_lock);
        return;
    }

    const float gap = 18.f * sc;
    const bool fabRight = (g_fabX + D * 0.5f) > W * 0.5f;
    const float room = fabRight ? (g_fabX - gap) : (W - (g_fabX + D) - gap);
    float pw, px;
    if (room - gap >= 560.f * sc) {                                   // sit beside the button
        pw = fminf(1280.f * sc, room - gap);
        px = fabRight ? (g_fabX - gap - pw) : (g_fabX + D + gap);
    } else {                                                           // button is in the way: centre it
        pw = fminf(W * 0.9f, 1280.f * sc);
        px = (W - pw) * 0.5f;
    }
    const float ph = fminf(H * 0.92f, 960.f * sc);
    const float py = (H - ph) * 0.5f;
    px += (fabRight ? 1.f : -1.f) * (1.f - g_openAnim) * 60.f * sc;    // slide in
    g_pX0 = px; g_pY0 = py; g_pX1 = px + pw; g_pY1 = py + ph;
    g_panelValid = true;

    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos(ImVec2(px, py));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_openAnim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * sc, 12.f * sc));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.11f, 0.95f));
    ImGui::Begin("##mcpanel", nullptr, panelFlags);

    const float hh = 84.f * sc;                      // header height
    const float btnH = 96.f * sc;                    // sidebar button height

    ImGui::SetCursorPos(ImVec2(18.f * sc, (hh - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", T("MCBRIDGE STATUS", "สถานะ MCBRIDGE"));
    {
        const float cs = 90.f * sc;
        ImGui::SetCursorPos(ImVec2(pw - cs - 26.f * sc, (hh - cs) * 0.5f));
        if (Btn("X", ImVec2(cs, cs), 20, kRed)) g_open = false;
    }

    ImGui::SetCursorPos(ImVec2(16.f * sc, hh));
    const float bodyH = ImGui::GetContentRegionAvail().y;
    const float sideW = fminf(280.f * sc, pw * 0.27f);

    // sidebar
    ImGui::BeginChild("##mcside", ImVec2(sideW, bodyH), 0, ImGuiWindowFlags_NoScrollbar);
    for (int t = 0; t < kTabCount; t++)
        if (Btn(T(kTabEn[t], kTabTh[t]), ImVec2(ImGui::GetContentRegionAvail().x, btnH), 1 + t, g_tab == t ? kAccentCol : kBtnCol)) g_tab = t;
    ImGui::EndChild();

    ImGui::SameLine();

    // content (scrolls by dragging, with a little glide)
    ImGui::BeginChild("##mccontent", ImVec2(0, bodyH), 0, 0);
    {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        g_cX0 = wp.x; g_cY0 = wp.y; g_cX1 = wp.x + ws.x; g_cY1 = wp.y + ws.y;
        if (g_tab != g_lastTab) {
            ImGui::SetScrollY(0.f); g_lastTab = g_tab; g_fling = 0.f;
            if (g_tab == kTabLog) g_logLoaded = false;                // reload the files when the Log page opens
            Diag::Crumb("menu tab: %s", kTabEn[g_tab]);
        }
        else if (scrollDy != 0.f) { ImGui::SetScrollY(ImGui::GetScrollY() - scrollDy); g_fling = 0.f; }
        else if (g_finger >= 0 && g_scrollArea) g_fling = 0.f;
        else if (fabsf(g_fling) > 40.f) { ImGui::SetScrollY(ImGui::GetScrollY() - g_fling * dt); g_fling = g_fling * expf(-dt * 3.5f); }
        else g_fling = 0.f;
    }
    switch (g_tab) {
        case kTabStatus:   PageStatus();   break;
        case kTabProgress: PageProgress(); break;
        case kTabTests:    PageTests();    break;
        case kTabLog:      PageLog();      break;
        default:           PageSettings(); break;
    }
    ImGui::EndChild();

    // toast
    if (g_toastT > 0.f && g_toast[0]) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ts = ImGui::CalcTextSize(g_toast);
        const float a = Clamp(g_toastT / 0.4f, 0.f, 1.f);
        const float bx = wp.x + (pw - ts.x) * 0.5f, by = wp.y + ph - 76.f * sc;
        dl->AddRectFilled(ImVec2(bx - 26.f * sc, by - 12.f * sc), ImVec2(bx + ts.x + 26.f * sc, by + ts.y + 12.f * sc),
                          IM_COL32(20, 130, 80, (int)(235 * a)), 20.f * sc);
        dl->AddText(ImVec2(bx, by), IM_COL32(255, 255, 255, (int)(255 * a)), g_toast);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // publish this frame's tappable rectangles to the touch side
    pthread_mutex_lock(&g_lock);
    g_rects.swap(g_rectsBuild);
    pthread_mutex_unlock(&g_lock);
}

} // namespace McUI
