// TTEST=1 builds: what loading the constants costs (constants.json, 34 KB), the game's own code
// translated against the hand port's reader, on the GameCube. Called by Source/TranslatedTest.cpp
// after the replays. A translation unit of its own: Octave's headers (its Maths.h's PI) stay out of
// the translated code's.
#if MLGC_TTEST

#include <stdint.h>
#include <stdio.h>

#include <fstream>
#include <sstream>
#include <string>

#include "bind/UnityEngine.h"
#include "game.h"
#include "game/Constants.h"

void MeasureConstantsLoad(uint64_t (*clock)(), uint32_t (*inUse)(), double ticksPerMicrosecond)
{
    printf("\nLOAD the constants (constants.json), the game's own reader translated against the hand port's:\n");
    // The translated: Constants.ReadFile's steps (Resources.Load's text, FsJSONManager.RawDeserialize:
    // RemoveComments, then FullSerializer's fsJsonParser).
    uint32_t m0 = inUse();
    uint64_t t0 = clock();
    cs::String* text = UnityEngine::Resources::Load<UnityEngine::TextAsset*>(cs::String::FromUtf8("constants"))->get_text();
    uint64_t t1 = clock();
    uint32_t m1 = inUse();
    cs::String* stripped = gen::FsJSONManager::RemoveComments(text);
    uint64_t t2 = clock();
    uint32_t m2 = inUse();
    gen::FullSerializer::fsData* data = gen::FullSerializer::fsJsonParser::Parse(stripped);
    uint64_t t3 = clock();
    uint32_t m3 = inUse();
    printf("LOAD   translated: the text (UTF-8 to a UTF-16 string)        %8.0f us %7u KB\n", (t1 - t0) / ticksPerMicrosecond,
           static_cast<unsigned>((m1 - m0) / 1024));
    printf("LOAD   translated: FsJSONManager.RemoveComments              %8.0f us %7u KB (a String.Remove per comment: "
           "the whole text copied each time)\n",
           (t2 - t1) / ticksPerMicrosecond, static_cast<unsigned>((m2 - m1) / 1024));
    printf("LOAD   translated: fsJsonParser.Parse                        %8.0f us %7u KB (%s)\n", (t3 - t2) / ticksPerMicrosecond,
           static_cast<unsigned>((m3 - m2) / 1024), data ? "parsed" : "failed");
    // The hand port: game::Constants's own reader of the same file.
    std::ifstream f("disc:/ttest/constants.json", std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string utf8 = ss.str();
    const uint32_t m4 = inUse();
    const uint64_t t4 = clock();
    game::Constants::LoadFromText(utf8);
    const uint64_t t5 = clock();
    printf("LOAD   hand port: game::Constants::LoadFromText               %8.0f us %7d KB (what it keeps)\n", (t5 - t4) / ticksPerMicrosecond,
           static_cast<int>(static_cast<int32_t>(inUse() - m4) / 1024));
}

#endif
