#include <stdint.h>

#undef min
#undef max

#include "Engine.h"
#include "Log.h"
#include "Renderer.h"
#include "World.h"
#include "Nodes/Widgets/Text.h"

#include "DungeonView.h"
#include "MoonGame.h"

// Octave's packager generates these; embedded builds ("GameCube Embedded") fill them.
#if __has_include("../Generated/EmbeddedAssets.h")
#include "../Generated/EmbeddedAssets.h"
#include "../Generated/EmbeddedScripts.h"
#define MLGC_HAS_GENERATED 1
#else
#define MLGC_HAS_GENERATED 0
#endif

// No scene: MoonGame runs the dungeon from MLGC/Scripts/Data (packaged with the project and
// served from the disc), drawn by one full-screen DungeonView.

static MoonGame* sGame = nullptr;
static Text* sStatus = nullptr; // where we are, over the room

#if OCT_GECKO_LOG
bool OctGeckoLogEnable(); // Octave (System_Dolphin.cpp): the log over the USB Gecko
#endif

void OctPreInitialize(EngineConfig& config)
{
#if OCT_GECKO_LOG
    OctGeckoLogEnable();
#endif
    GetEngineState()->mStandalone = true;

    if (config.mWindowWidth == 0)
        config.mWindowWidth = 1280;

    if (config.mWindowHeight == 0)
        config.mWindowHeight = 720;

#if MLGC_HAS_GENERATED
    config.mEmbeddedAssetCount = gNumEmbeddedAssets;
    config.mEmbeddedAssets = gEmbeddedAssets;
    config.mEmbeddedScriptCount = gNumEmbeddedScripts;
    config.mEmbeddedScripts = gEmbeddedScripts;
    config.mEmbeddedConfig = gEmbeddedConfig_Data;
    config.mEmbeddedConfigSize = gEmbeddedConfig_Size;
#endif
}

void OctPostInitialize()
{
    if (Renderer::Get() != nullptr)
    {
        Renderer::Get()->EnableConsole(false);
    }

    // The picture's whole width: the VI scales the 640-pixel frame out to 704 of its 720, so the
    // room (squeezed into the frame) fills a 16:9 TV edge to edge, and the letterbox (Z) keeps
    // square pixels on a 4:3 one.
    GXRModeObj* rmode = &GetEngineState()->mSystem.mGxRmode;
    rmode->viWidth = 704;
    rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 704) / 2;
    VIDEO_Configure(rmode);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    sGame = new MoonGame();
    if (!sGame->Initialize())
    {
        LogError("mlgc: initialization failed");
    }

    glm::vec2 res = Renderer::Get()->GetScreenResolution();
    DungeonView* view = GetWorld(0)->SpawnNode<DungeonView>();
    view->SetName("Dungeon");
    view->SetRect(0.0f, 0.0f, res.x, res.y);
    view->SetGame(sGame);

    sStatus = view->CreateChild<Text>("Status");
    sStatus->SetRect(8.0f, 4.0f, res.x - 16.0f, 20.0f);
    sStatus->SetTextSize(12.0f);
    sStatus->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
}

void OctPreUpdate()
{
}

void OctPostUpdate()
{
    if (sGame != nullptr)
    {
        sGame->Update(GetEngineState()->mGameDeltaTime);
        if (sStatus != nullptr)
        {
            // Up for a few seconds after it changes (errors stay). Dolphin judges widescreen by
            // what's drawn squeezed against what's drawn 4:3, and the 4:3 text holds it at 4:3.
            static std::string shown;
            static int frames = 0;
            const std::string& status = sGame->Status();
            if (status != shown)
            {
                shown = status;
                frames = 0;
                sStatus->SetText(status);
            }
            frames++;
            sStatus->SetVisible(frames < 180 || status.compare(0, 6, "error:") == 0);
        }
    }
}

void OctPreShutdown()
{
    delete sGame;
    sGame = nullptr;
}

void OctPostShutdown()
{
}
