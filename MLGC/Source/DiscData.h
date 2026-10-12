// The game's data on the disc: MLGC/Scripts/Data/ (tools/make_data.py puts it there from
// your copy of the game; Octave's packager puts the project's Scripts/ on the disc), read
// through Octave.
#pragma once

#include <string>

#include "game/Session.h"

class DiscData : public game::DataSource {
public:
    std::string Read(const std::string& path) override;
};
