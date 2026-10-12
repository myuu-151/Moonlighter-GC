#include "DungeonView.h"

#include "Engine.h"
#include "Graphics/GX/GxTypes.h"
#include "Graphics/GX/GxUtils.h"
#include "MoonGame.h"

DEFINE_NODE(DungeonView, Widget);

extern GxContext gGxContext; // Graphics_GX.cpp

DrawData DungeonView::GetDrawData()
{
    DrawData data = {};
    data.mNode = this;
    return data;
}

void DungeonView::Render()
{
    Widget::Render();

    if (mGame == nullptr)
    {
        return;
    }

    // The room: letterboxed, or squeezed over the framebuffer (anamorphic 16:9).
    GXRModeObj* rmode = &GetEngineState()->mSystem.mGxRmode;
    mGame->Render(float(rmode->fbWidth), float(rmode->efbHeight));

    // Back to Octave's UI state for anything drawn after the room.
    GX_SetViewport(0.0f, 0.0f, float(rmode->fbWidth), float(rmode->efbHeight), 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    PrepareUiRendering();

    // Octave's SetupLightingChannels skips setting channels it thinks are already set: put
    // back what it remembers (the UI's).
    const LightingState& ui = gGxContext.mLighting;
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR1A1, false, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR0A0, ui.mEnabled, GX_SRC_REG, ui.mMaterialSrc, ui.mLightMask, ui.mDiffuseFunc,
        ui.mAttenuationFunc);
}
