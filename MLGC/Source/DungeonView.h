// A full-screen widget that draws the dungeon with GX when Octave renders its UI.
#pragma once

#include "Nodes/Widgets/Widget.h"

class MoonGame;

class DungeonView : public Widget
{
public:
    DECLARE_NODE(DungeonView, Widget);

    void SetGame(MoonGame* game) { mGame = game; }

    // A plain Widget has no draw data, so Octave would never call Render.
    virtual DrawData GetDrawData() override;
    virtual void Render() override;

private:
    MoonGame* mGame = nullptr;
};
