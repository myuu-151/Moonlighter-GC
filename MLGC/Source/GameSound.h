// MasterAudio on the GameCube: what plays a sound is the port's game::MasterAudio (the group,
// its pool, its variation, the random volume and pitch, the limits, the retrigger: the same
// code and data as the PC's, sound_groups.json's), and what it chose plays through gcaudio.
//
// The draws: MasterAudio's draws come from UnityEngine.Random, the stream the whole game draws
// from, so playing sounds must not change them.
//   - The sounds the port's own code plays (the enemies' MasterAudio.PlaySound, their
//     EventSounds: game::EnemyScene::audio) go through Game(), on the game's Random: the draws
//     the PC makes and its replays check. Sound only listens (MasterAudio::onPlay, called with
//     what was drawn); nothing it does feeds back: whether a variation is "still playing" for
//     the retrigger and the voice limits is MasterAudio's own clock model, not the voices'.
//   - The sounds the port doesn't play yet (the hero's steps and swings, the hit and death
//     sounds, the breakables, the ambience) go through Play(): a second MasterAudio, the same
//     groups, on a Random state of its own, swapped in around each call, so the game's stream
//     never moves for them. When the port plays one itself (its draws checked against the
//     game), it moves to Game() and draws there, on the PC and the GameCube alike.
//   - The music draws nothing: the playlists aren't shuffled.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "DiscData.h"
#include "game/MasterAudio.h"
#include "unity/Random.h"

class GameSound {
public:
    GameSound();
    ~GameSound();

    // The sound's index and the first scene's banks (Will's and the interface's, read into ARAM
    // in the background); false (and silence) if the disc has no sound.
    bool Init(DiscData& data);
    // A dungeon's sound groups, as DungeonGenerator.GenerateSounds makes them: the first scene's
    // creators, then the culture's (1 Golem .. 4 Tech), then the general dungeon ones; their
    // banks into ARAM (the last dungeon's out).
    void EnterDungeon(DiscData& data, int culture);

    // The port's MasterAudio (the game's Random), for its code to play through.
    game::MasterAudio* Game() { return game_.get(); }
    // MasterAudio.PlaySound for a sound the port doesn't play itself (its own Random).
    void Play(const std::string& group);
    // MasterAudio.StopAllOfSound (FadeOutAllOfSound with a fade).
    void StopGroup(const std::string& group, float fadeOut = 0.0f);

    // The frame's clocks (Time.realtimeSinceStartup's stand-in, Time.frameCount), before the
    // frame's scripts; MasterAudio's LateUpdate after them (the voices start then, as the
    // game's AudioSources do).
    void BeginFrame(float realDeltaTime);
    void LateUpdate();

    // PlaylistManager: the dungeon's playlist (by floor) faded in over 0.5 s, the one before
    // faded out; FadeOutAllMusic. AmbientSoundManager: the dungeon's main ambience.
    void StartDungeonPlaylist(int level, int culture);
    void FadeOutAllMusic(float seconds = 0.5f);
    void PlayDungeonMainAmbient(int culture);
    void StopCurrentAmbient();

    // "audio: N voices, ARAM ..." for the log.
    std::string StatsLine() const;

private:
    struct Variation {
        int clip = -1;
        float volume = 1.0f, pitch = 1.0f; // its AudioSource's
        bool loop = false;
        int randomVolumeMode = 0, randomPitchMode = 0; // 0: added to the AudioSource's, 1: instead of it
        bool customLooping = false;
        float mixer = 1.0f; // the AudioMixer's gain on its way out (its bus's group's, the options at 4)
    };
    struct Group {
        float volume = 1.0f; // groupMasterVolume
        std::vector<Variation> variations;
    };

    void OnPlay(const game::MasterAudio::Played& played);
    void Forget(); // finished voices out of playing_

    bool ready_ = false;
    std::unique_ptr<json::Value> index_;
    std::unique_ptr<game::MasterAudio> game_, fx_;
    unity::Random::State fxRandom_{};
    std::map<std::string, Group> groups_;
    std::map<std::string, std::vector<uint32_t>> playing_; // a group's voices
    int culture_ = 0;
    double now_ = 0.0;
    int frame_ = 0;
    std::string playlist_;
    uint32_t music_ = 0;
    std::vector<std::string> ambient_;
    uint32_t plays_ = 0;
};
