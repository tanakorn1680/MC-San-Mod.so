#pragma once
// ImGui renderer on top of the game's own RenderWare (GTA SA Android, arm64, v2.10).
// MCBridge's own copy (same drawing code as ProMenu's, which is known to work in the game; kept separate on purpose so
// the two plugins never depend on each other). Needs, before it is included: Diag (diag.h) and
// template <typename T> bool ResolveSym(T& out, const char* name).
//
// Everything below was read out of the original ARM CheatMenu binary
// (libCheatMenu64.so), not guessed:
//   * 2D vertex = 28 bytes { x, y, z, rhw, color, u, v }
//       z   = *CSprite2d::NearScreenZ     rhw = *CSprite2d::RecipNearClip
//       color is ImGui's ABGR value copied verbatim
//   * draw call  = RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST = 3, ...)
//   * font atlas = RwImage -> RwRaster, the RwRaster* is used as ImTextureID
//   * clipping   = CWidget::SetScissor(CRect{ left, bottom, right, top })
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "imgui.h"

namespace RWB {

struct Raster;
struct Image { int32_t flags, width, height, depth, stride; uint8_t* pixels; void* palette; };
struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };
struct CRect { float left, bottom, right, top; };

static_assert(sizeof(Vertex) == 28, "RW 2D vertex must be 28 bytes");
static_assert(offsetof(Image, pixels) == 0x18, "RwImage.cpPixels must sit at 0x18");

// RwRenderState ids used by the original
enum {
    rsTextureRaster = 1, rsTextureAddress = 2, rsZTest = 6, rsZWrite = 8,
    rsTextureFilter = 9, rsSrcBlend = 10, rsDestBlend = 11, rsVertexAlpha = 12,
    rsBorderColor = 13, rsFog = 14, rsCullMode = 20, rsAlphaTestFunc = 29, rsAlphaTestRef = 30
};

static int      (*RenderStateSet)(int, void*)                                   = nullptr;
static Image*   (*ImageCreate)(int, int, int)                                   = nullptr;
static Image*   (*ImageAllocatePixels)(Image*)                                  = nullptr;
static int      (*ImageDestroy)(Image*)                                         = nullptr;
static void*    (*ImageFindRasterFormat)(Image*, int, int*, int*, int*, int*)   = nullptr;
static Raster*  (*RasterCreate)(int, int, int, int)                             = nullptr;
static Raster*  (*RasterSetFromImage)(Raster*, Image*)                          = nullptr;
static int      (*RasterDestroy)(Raster*)                                       = nullptr;
static int      (*Im2DRenderIndexedPrimitive)(int, Vertex*, int, uint16_t*, int) = nullptr;
static void     (*SetScissor)(CRect&)                                           = nullptr;
static float*   NearScreenZ                                                     = nullptr;
static float*   RecipNearClip                                                   = nullptr;

static Raster*             g_fontRaster = nullptr;
static std::vector<Vertex> g_vb;

static inline void RS(int state, intptr_t value) { RenderStateSet(state, (void*)value); }

#define RWB_NEED(var, name)                                                   \
    do { if (!ResolveSym(var, name)) { Diag::Log("rwb: game symbol MISSING: %s", name); ok = false; } } while (0)

static bool Resolve() {
    bool ok = true;
    RWB_NEED(RenderStateSet,         "_Z16RwRenderStateSet13RwRenderStatePv");
    RWB_NEED(ImageCreate,            "_Z13RwImageCreateiii");
    RWB_NEED(ImageAllocatePixels,    "_Z21RwImageAllocatePixelsP7RwImage");
    RWB_NEED(ImageDestroy,           "_Z14RwImageDestroyP7RwImage");
    RWB_NEED(ImageFindRasterFormat,  "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_");
    RWB_NEED(RasterCreate,           "_Z14RwRasterCreateiiii");
    RWB_NEED(RasterSetFromImage,     "_Z20RwRasterSetFromImageP8RwRasterP7RwImage");
    RWB_NEED(RasterDestroy,          "_Z15RwRasterDestroyP8RwRaster");
    RWB_NEED(SetScissor,             "_ZN7CWidget10SetScissorER5CRect");
    RWB_NEED(NearScreenZ,            "_ZN9CSprite2d11NearScreenZE");
    RWB_NEED(RecipNearClip,          "_ZN9CSprite2d13RecipNearClipE");
    // The game's real export uses the OpenGL vertex type in its name (confirmed in the
    // original menu's wrapper, which resolves exactly this string via GetSym).
    RWB_NEED(Im2DRenderIndexedPrimitive,
             "_Z28RwIm2DRenderIndexedPrimitive15RwPrimitiveTypeP14RwOpenGLVertexiPti");
    return ok;
}
#undef RWB_NEED

static bool CreateFontRaster() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!px || w <= 0 || h <= 0) return false;

    Image* img = ImageCreate(w, h, 32);
    if (!img) return false;
    ImageAllocatePixels(img);
    if (!img->pixels) { ImageDestroy(img); return false; }

    const size_t rowBytes = (size_t)w * 4;
    const size_t copy = rowBytes < (size_t)img->stride ? rowBytes : (size_t)img->stride;
    for (int y = 0; y < h; y++)
        memcpy(img->pixels + (size_t)y * (size_t)img->stride, px + (size_t)y * rowBytes, copy);

    int rw = 0, rh = 0, rd = 0, rf = 0;
    ImageFindRasterFormat(img, 4 /* rwRASTERTYPETEXTURE */, &rw, &rh, &rd, &rf);
    Raster* r = RasterCreate(rw, rh, rd, rf);
    if (!r) { ImageDestroy(img); return false; }
    Raster* r2 = RasterSetFromImage(r, img);
    ImageDestroy(img);

    g_fontRaster = r2 ? r2 : r;
    io.Fonts->SetTexID((ImTextureID)(uintptr_t)g_fontRaster);
    return true;
}

// call BEFORE the game tears RenderWare down (GL context is about to disappear)
static void DestroyFontRaster() {
    if (g_fontRaster && RasterDestroy) RasterDestroy(g_fontRaster);
    g_fontRaster = nullptr;
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().Fonts->SetTexID((ImTextureID)(uintptr_t)0);
}

static bool NewFrame(float w, float h, float dt) {
    if (!g_fontRaster && !CreateFontRaster()) return false;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = dt;
    return true;
}

static void RenderDrawData(ImDrawData* dd) {
    if (!dd || dd->CmdListsCount == 0 || dd->TotalVtxCount == 0) return;
    if (dd->DisplaySize.x <= 0.f || dd->DisplaySize.y <= 0.f) return;

    const float z = *NearScreenZ;
    const float rhw = *RecipNearClip;

    if ((int)g_vb.size() < dd->TotalVtxCount) g_vb.resize((size_t)dd->TotalVtxCount + 1024);

    int base = 0;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* dl = dd->CmdLists[n];
        const ImDrawVert* src = dl->VtxBuffer.Data;
        Vertex* dst = &g_vb[(size_t)base];
        for (int i = 0; i < dl->VtxBuffer.Size; i++) {
            dst[i].x = src[i].pos.x;  dst[i].y = src[i].pos.y;
            dst[i].z = z;             dst[i].rhw = rhw;
            dst[i].color = src[i].col;
            dst[i].u = src[i].uv.x;   dst[i].v = src[i].uv.y;
        }
        base += dl->VtxBuffer.Size;
    }

    // same state set (and order) as the original
    RS(rsZTest, 0);
    RS(rsZWrite, 0);
    RS(rsVertexAlpha, 1);
    RS(rsSrcBlend, 5);        // rwBLENDSRCALPHA
    RS(rsDestBlend, 6);       // rwBLENDINVSRCALPHA
    RS(rsFog, 0);
    RS(rsCullMode, 1);        // rwCULLMODECULLNONE
    RS(rsBorderColor, 0);
    RS(rsAlphaTestFunc, 5);   // rwALPHATESTFUNCTIONGREATER
    RS(rsAlphaTestRef, 2);
    RS(rsTextureFilter, 2);   // rwFILTERLINEAR
    RS(rsTextureAddress, 3);  // rwTEXTUREADDRESSCLAMP

    base = 0;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (int c = 0; c < dl->CmdBuffer.Size; c++) {
            const ImDrawCmd& cmd = dl->CmdBuffer[c];
            if (cmd.UserCallback || cmd.ElemCount == 0) continue;

            CRect clip = { cmd.ClipRect.x, cmd.ClipRect.w, cmd.ClipRect.z, cmd.ClipRect.y };
            SetScissor(clip);
            RS(rsTextureRaster, (intptr_t)(uintptr_t)cmd.GetTexID());
            Im2DRenderIndexedPrimitive(3 /* rwPRIMTYPETRILIST */, &g_vb[(size_t)base],
                                       dl->VtxBuffer.Size,
                                       (uint16_t*)(dl->IdxBuffer.Data + cmd.IdxOffset),
                                       (int)cmd.ElemCount);
        }
        base += dl->VtxBuffer.Size;
    }

    CRect off = { 0.f, 0.f, 0.f, 0.f };   // original resets the scissor like this
    SetScissor(off);
}

} // namespace RWB
