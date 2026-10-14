// The GameCube's sound: the game's clips (tools/make_audio.py: DSP-ADPCM, audio/index.json)
// played as voices, mixed by the CPU into one stereo stream that one ASND voice plays.
//
// A previous GameCube port worked this out on the console, and this follows it:
//   - one mixing thread, above the main thread, decodes and resamples every voice into a
//     32 kHz stereo buffer a few times a frame and hands it to ASND (voice 15: above Octave's
//     own, 0-7, and its PCM streams, 8-14, which MLGC doesn't use). Any number of voices, any
//     pitch (MasterAudio's random pitch is the resampling step), and a long frame on the main
//     thread can't starve the sound;
//   - the sound effects are banks in ARAM (one a sound group creator), read in by a reader
//     thread below the main thread, a piece at a time, while the game goes on: loading a file
//     a sound took tens of seconds there. A voice copies its frames back from ARAM a window
//     at a time;
//   - music and long ambient loops stream from the disc into a ring in ARAM (7 s each): a
//     2 s ring in main memory ran dry under long frames there. A failed read is tried again
//     (for 5 s) instead of ending the track;
//   - ASND restarted under it (Octave does, to mount a memory card) is noticed and the voice
//     set again; ARAM goes through Octave's allocator and transfers (32-byte, interrupts off).
// The GameCube's own ADPCM decoder (the DSP's, through ARAM) isn't reachable from libogc's
// ASND, so the CPU decodes; it is a few percent of the CPU with a dozen voices.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/Json.h"

namespace gcaudio {

constexpr int kOutputRate = 32000;

// Starts the threads and reads audio/index.json's banks and clips; false (and silence) if
// there's no sound on the disc or no room for it.
bool Init(const json::Value& index);
bool Ready();

// A clip by its key (audio.json's: the AudioClip's name), -1 if there's none.
int FindClip(const std::string& key);
const std::string& ClipName(int clip);
float ClipLength(int clip); // seconds

// The banks wanted in ARAM (by name: "will", "golemdungeon", ...): the others' voices stop and
// their ARAM is freed; the new ones are read in by the reader, in this order. A clip whose
// bank isn't in yet doesn't play (the log says so, once).
void WantBanks(const std::vector<std::string>& banks);
bool BankLoaded(const std::string& bank);

struct PlayParams {
    float volume = 1.0f;
    float pitch = 1.0f;
    int loops = 1;          // times to play it; 0: for ever
    float delay = 0.0f;     // seconds of silence first (MasterAudio's intro silence)
    float start = 0.0f;     // where to start, a fraction of the clip (its random start)
    float fadeIn = 0.0f;    // seconds from silence to `volume`
};
// A voice playing the clip: a handle (0 if it can't: no clip, its bank not in, no voice).
uint32_t Play(int clip, const PlayParams& params);
void Stop(uint32_t voice, float fadeOut = 0.0f);
// Its volume, reached over `seconds` (MasterAudio's fades, the playlists' FadeToVolume).
void SetVolume(uint32_t voice, float volume, float seconds = 0.0f);
bool Playing(uint32_t voice);

struct Stats {
    uint32_t voices = 0, streams = 0;       // playing
    uint32_t buffersMixed = 0;
    uint32_t aramBanks = 0, aramRings = 0;  // bytes in use
    uint32_t aramTotal = 0, aramFree = 0;   // Octave's allocator
    uint32_t mram = 0;                      // main memory the sound keeps
    uint32_t starved = 0, stolen = 0, unplayed = 0, readFailures = 0;
    uint32_t streamAhead = 0;               // the music's blocks read ahead (of its ring)
    uint32_t mixMicros = 0, mixMicrosMost = 0; // mixing a 32 ms buffer: on average, at most
};
Stats GetStats();

} // namespace gcaudio
