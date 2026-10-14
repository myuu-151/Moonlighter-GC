#include "GameSound.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <stdexcept>

#include "AudioGc.h"
#include "Log.h"
#include "System/System.h"

// MLGC_AUDIOLOG (test builds, AUDIOLOG=1): each sound played, in Dolphin's log.
#ifdef MLGC_AUDIOLOG
#define AUDIO_LOG(...) SYS_Report(__VA_ARGS__)
#else
#define AUDIO_LOG(...) ((void)0)
#endif

namespace {

const char* const kCultures[5] = {"", "Golem", "Forest", "Desert", "Tech"};
// DungeonGenerator.GetSoundPathByCulture's creators.
const char* const kCultureCreators[5] = {"", "Sounds - GolemDungeon", "Sounds - ForestDungeon", "Sounds - DesertDungeon",
                                         "Sounds - TeslaDungeon"};
// PlaylistManager's controllers for the dungeons (StartDungeonPlaylist), by culture.
const char* const kControllers[5] = {"", "golemDungeonPlaylistController", "forestDungeonPlaylistController",
                                     "desertDungeonPlaylistController", "techDungeonPlaylistController"};
// AmbientSoundManager's.
const char* const kMainAmbients[5] = {"", "golem_dungeon_main_ambient", "forest_dungeon_main_ambient", "desert_dungeon_main_ambient",
                                      "tesla_dungeon_main_ambient"};

struct Song {
    std::string clip;
    float volume = 1.0f, pitch = 1.0f;
    bool loop = true;
};
std::map<std::string, Song> g_playlists;                               // a playlist's first song
std::map<std::string, float> g_controllerGain;                        // a PlaylistManager field's controller's mixer gain
std::map<std::string, std::pair<std::string, std::string>> g_creators; // creator -> (bank, groups file)

// Runs `f` on the presentation sounds' own Random state, the game's put back after.
template <typename F>
void OnOwnRandom(unity::Random::State& own, F&& f)
{
    const unity::Random::State game = unity::Random::GetState();
    unity::Random::SetState(own);
    f();
    own = unity::Random::GetState();
    unity::Random::SetState(game);
}

} // namespace

GameSound::GameSound() = default;
GameSound::~GameSound() = default;

bool GameSound::Init(DiscData& data)
{
    try {
        const json::Value index = json::Parse(data.Read("audio/index.json"));
        if (!gcaudio::Init(index)) return false;
        for (const auto& [name, c] : index["creators"].obj) g_creators[name] = {c["bank"].s, c["groups"].s};
        // The controllers' mixer gains (their mixerChannel's), by PlaylistManager's fields.
        std::map<std::string, float> gains;
        for (const auto& [name, c] : index["controllers"].obj)
            if (const json::Value* g = c.Find("mixerGain")) gains[name] = static_cast<float>(g->AsDouble());
        if (const json::Value* manager = index.Find("playlistManager"))
            for (const auto& [field, controller] : manager->obj)
                if (gains.count(controller.s)) g_controllerGain[field] = gains[controller.s];
        for (const auto& [name, p] : index["playlists"].obj) {
            if (p["songs"].arr.empty()) continue;
            const json::Value& s = p["songs"][0];
            g_playlists[name] = Song{s["clip"].s, static_cast<float>(s["volume"].AsDouble()), static_cast<float>(s["pitch"].AsDouble()),
                                     s["loop"].b};
        }
    } catch (const std::exception& e) {
        LogError("mlgc: no sound (%s)", e.what());
        return false;
    }
    // The presentation sounds' Random: a stream of their own.
    const unity::Random::State game = unity::Random::GetState();
    unity::Random::InitState(0x4D4C4743);
    fxRandom_ = unity::Random::GetState();
    unity::Random::SetState(game);
    std::vector<std::string> banks;
    for (const char* c : {"Sounds - Will", "Sounds - interface"})
        if (g_creators.count(c)) banks.push_back(g_creators[c].first);
    gcaudio::WantBanks(banks);
    ready_ = true;
    return true;
}

void GameSound::EnterDungeon(DiscData& data, int culture)
{
    if (!ready_ || culture == culture_ || culture < 1 || culture > 4) return;
    const std::vector<std::string> creators = {"Sounds - Will", "Sounds - interface", kCultureCreators[culture], "Sounds - GeneralDungeons"};
    // The creators' groups, as sound_groups.json has them (each file holds one creator's).
    json::Value all;
    all.kind = json::Value::Kind::Object;
    std::vector<std::string> banks;
    for (const std::string& creator : creators) {
        auto it = g_creators.find(creator);
        if (it == g_creators.end()) continue;
        banks.push_back(it->second.first);
        try {
            json::Value file = json::Parse(data.Read(it->second.second));
            for (auto& entry : file.obj) all.obj.push_back(std::move(entry));
        } catch (const std::exception& e) {
            LogError("mlgc: sound groups %s: %s", creator.c_str(), e.what());
        }
    }
    for (auto& [group, voices] : playing_)
        for (uint32_t v : voices) gcaudio::Stop(v);
    playing_.clear();
    ambient_.clear();
    game_ = std::make_unique<game::MasterAudio>();
    fx_ = std::make_unique<game::MasterAudio>();
    game_->Load(all, creators);
    fx_->Load(all, creators);
    // The presentation sounds' groups made as CreateGroup makes them (their pools shuffled), on
    // their own Random. (The game's MasterAudio is left as the PC's replays have it.)
    OnOwnRandom(fxRandom_, [&] { fx_->CreateGroups(); });
    game_->onPlay = fx_->onPlay = [this](const game::MasterAudio::Played& p) { OnPlay(p); };
    game_->now = fx_->now = now_;
    game_->frameCount = fx_->frameCount = frame_;
    // What plays each variation: the first group of a name wins, as MasterAudio makes them.
    groups_.clear();
    for (const std::string& creator : creators) {
        const json::Value* c = all.Find(creator);
        if (!c) continue;
        for (const auto& [name, g] : c->obj) {
            if (groups_.count(name)) continue;
            Group group;
            if (const json::Value* v = g.Find("groupMasterVolume")) group.volume = static_cast<float>(v->AsDouble());
            for (const json::Value& v : g["variations"].arr) {
                Variation var;
                if (const json::Value* clip = v.Find("clip"); clip && clip->IsString()) var.clip = gcaudio::FindClip(clip->s);
                if (const json::Value* f = v.Find("volume")) var.volume = static_cast<float>(f->AsDouble());
                if (const json::Value* f = v.Find("pitch")) var.pitch = static_cast<float>(f->AsDouble());
                if (const json::Value* f = v.Find("loop")) var.loop = f->kind == json::Value::Kind::Bool ? f->b : f->AsInt() != 0;
                if (const json::Value* f = v.Find("randomVolumeMode")) var.randomVolumeMode = static_cast<int>(f->AsInt());
                if (const json::Value* f = v.Find("randomPitchMode")) var.randomPitchMode = static_cast<int>(f->AsInt());
                if (const json::Value* f = v.Find("useCustomLooping")) var.customLooping = f->AsInt() != 0;
                if (const json::Value* f = v.Find("mixerGain")) var.mixer = static_cast<float>(f->AsDouble());
                group.variations.push_back(var);
            }
            groups_[name] = std::move(group);
        }
    }
    gcaudio::WantBanks(banks);
    culture_ = culture;
    AUDIO_LOG("audio: the %s dungeon's sound: %u groups\n", kCultures[culture], static_cast<unsigned>(groups_.size()));
}

void GameSound::OnPlay(const game::MasterAudio::Played& p)
{
    auto it = groups_.find(p.group);
    if (it == groups_.end() || p.variation < 0 || p.variation >= static_cast<int>(it->second.variations.size())) return;
    const Group& g = it->second;
    const Variation& v = g.variations[static_cast<size_t>(p.variation)];
    if (v.clip < 0) return;
    // MasterAudio.PlaySoundIfAvailable's volume (its buses at 1, the master volume 1), then the
    // AudioMixer's on the way out; SoundGroupVariation.Play's pitch.
    float volume = v.volume;
    if (p.randomVolume) volume = v.randomVolumeMode == 0 ? volume + p.volume : p.volume;
    volume *= g.volume * v.mixer;
    float pitch = v.pitch;
    if (p.randomPitch) pitch = v.randomPitchMode == 0 ? p.pitch + v.pitch : p.pitch;
    gcaudio::PlayParams params;
    params.volume = volume;
    params.pitch = pitch;
    // useCustomLooping: _maxLoops times; else the AudioSource's loop.
    params.loops = v.customLooping ? std::max(1, p.loops) : (v.loop ? 0 : 1);
    params.delay = p.introSilence;
    params.start = p.startPercent / 100.0f;
    const uint32_t voice = gcaudio::Play(v.clip, params);
    plays_++;
    if (voice) playing_[p.group].push_back(voice);
    AUDIO_LOG("audio: play %s #%d %s vol %.3f pitch %.3f loops %d%s\n", p.group.c_str(), p.variation, gcaudio::ClipName(v.clip).c_str(),
              volume, pitch, params.loops, voice ? "" : " (no voice)");
}

void GameSound::Play(const std::string& group)
{
    if (!fx_) return;
    OnOwnRandom(fxRandom_, [&] { fx_->PlaySound(group); });
}

void GameSound::StopGroup(const std::string& group, float fadeOut)
{
    auto it = playing_.find(group);
    if (it == playing_.end()) return;
    for (uint32_t v : it->second) gcaudio::Stop(v, fadeOut);
    playing_.erase(it);
}

void GameSound::BeginFrame(float realDeltaTime)
{
    now_ += static_cast<double>(realDeltaTime);
    frame_++;
    for (game::MasterAudio* a : {game_.get(), fx_.get()})
        if (a) {
            a->now = now_;
            a->frameCount = frame_;
        }
}

void GameSound::LateUpdate()
{
    if (game_) game_->LateUpdate();
    if (fx_) OnOwnRandom(fxRandom_, [&] { fx_->LateUpdate(); });
    if (frame_ % 60 == 0) Forget();
}

void GameSound::Forget()
{
    for (auto it = playing_.begin(); it != playing_.end();) {
        std::vector<uint32_t>& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [](uint32_t h) { return !gcaudio::Playing(h); }), v.end());
        it = v.empty() ? playing_.erase(it) : std::next(it);
    }
}

void GameSound::StartDungeonPlaylist(int level, int culture)
{
    if (!ready_ || culture < 1 || culture > 4) return;
    const std::string name = std::string(kCultures[culture]) + "Dungeon" + (level > 0 ? "Level" + std::to_string(level) : "") + "Playlist";
    auto it = g_playlists.find(name);
    if (it == g_playlists.end()) return;
    const Song& song = it->second;
    // The song's volume, its controller faded to 1 (FadeToVolume(1, 0.5)), the playlists' master
    // volume 1, then the AudioMixer's (the controller's mixerChannel).
    auto gain = g_controllerGain.find(kControllers[culture]);
    const float volume = song.volume * (gain != g_controllerGain.end() ? gain->second : 1.0f);
    if (name == playlist_ && gcaudio::Playing(music_)) {
        gcaudio::SetVolume(music_, volume, 0.5f);
        return;
    }
    if (music_) gcaudio::Stop(music_, 0.5f);
    gcaudio::PlayParams params;
    params.volume = volume;
    params.pitch = song.pitch;
    params.loops = song.loop ? 0 : 1;
    params.fadeIn = 0.5f;
    music_ = gcaudio::Play(gcaudio::FindClip(song.clip), params);
    playlist_ = name;
    AUDIO_LOG("audio: playlist %s: %s%s\n", name.c_str(), song.clip.c_str(), music_ ? "" : " (not played)");
}

void GameSound::FadeOutAllMusic(float seconds)
{
    if (music_) gcaudio::Stop(music_, seconds);
    music_ = 0;
    playlist_.clear();
}

void GameSound::PlayDungeonMainAmbient(int culture)
{
    if (culture < 1 || culture > 4) return;
    StopCurrentAmbient();
    Play(kMainAmbients[culture]);
    ambient_.push_back(kMainAmbients[culture]);
}

void GameSound::StopCurrentAmbient()
{
    // (Its variations fade out over 0.2 s, then StopAllOfSound.)
    for (const std::string& g : ambient_) StopGroup(g, 0.2f);
    ambient_.clear();
}

std::string GameSound::StatsLine() const
{
    const gcaudio::Stats s = gcaudio::GetStats();
    char line[384];
    std::snprintf(line, sizeof line,
                  "audio: %u plays, %u voices (%u streamed, %u blocks ahead), %u buffers mixed; ARAM banks %u KB, rings %u KB, "
                  "%u KB free of %u KB; main memory %u KB; mixing %u us a 32 ms buffer (at most %u); starved %u, stolen %u, "
                  "unplayed %u, read failures %u",
                  plays_, s.voices, s.streams, s.streamAhead, s.buffersMixed, s.aramBanks / 1024, s.aramRings / 1024, s.aramFree / 1024,
                  s.aramTotal / 1024, s.mram / 1024, s.mixMicros, s.mixMicrosMost, s.starved, s.stolen, s.unplayed, s.readFailures);
    return line;
}
