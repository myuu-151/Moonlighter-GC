#include "AudioGc.h"

#include <asndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/semaphore.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>

#include "Audio/Audio.h"
#include "AudioDsp.h"
#include "Log.h"
#include "System/System.h"

// Octave's ARAM (Audio_Dolphin.cpp: its allocator over the whole of ARAM, and transfers a piece
// at a time with interrupts off), which Audio.h doesn't declare.
uint32_t AUD_AllocAram(uint32_t len);
void AUD_AramDma(bool toAram, void* mem, uint32_t aram, uint32_t len);
// Octave's SD driver: the thread's card reads then run above the main thread (SdGeckoDma.c).
extern "C" void OctSd_NoteThreadPriority(u32 prio);

// MLGC_AUDIOLOG (test builds, AUDIOLOG=1): what the sound does, in Dolphin's log. (On a console
// SYS_Report overwrites the RTC counter: Octave's System_Dolphin.cpp.)
#ifdef MLGC_AUDIOLOG
#define AUDIO_LOG(...) SYS_Report(__VA_ARGS__)
#else
#define AUDIO_LOG(...) ((void)0)
#endif

namespace gcaudio {

namespace {

const char* const kRoot = "MLGC/Scripts/Data/";
constexpr int kMixFrames = 1024;   // 32 ms a buffer: over one of ASND's mixing blocks
constexpr int kBuffers = 3;
constexpr int kAsndVoice = 15;     // above Octave's (0-7) and its streams' (8-14, unused here)
constexpr int kVoices = 40;        // sound effects and streams alike
constexpr int kRings = 4;          // streams at once (music, the ambience, a crossfade)
constexpr uint32_t kRingBytes = 256 * 1024;      // 7 s of 32 kHz stereo
constexpr uint32_t kWindowFrames = 64;           // a voice's frames copied from ARAM at a time
constexpr uint32_t kWindowBytes = kWindowFrames * dsp::kFrameBytes;
constexpr uint32_t kChunkBytes = 8 * dsp::kChannelBlockBytes; // the reader's: 8 mono or 4 stereo blocks
constexpr uint32_t kHandleShift = 6;

struct Clip {
    std::string name;
    std::vector<std::pair<int, uint32_t>> banks; // (bank, offset) for each bank it's in; none: streamed
    std::string stream;                          // its file (streams)
    uint32_t bytes = 0, rate = kOutputRate, samples = 0;
    uint8_t channels = 1;
};

struct Bank {
    std::string name, file;
    uint32_t bytes = 0;
    uint32_t aram = 0;           // 0: not in ARAM
    uint32_t loaded = 0;         // bytes read in so far
    uint32_t generation = 0;     // a new one each time it's wanted or freed
    int order = 0;               // when it's read in
    bool wanted = false;
};

struct Ring {
    bool used = false;
    uint32_t at = 0;             // ARAM
    int clip = -1;
    uint32_t blockBytes = 0;     // the clip's blocks (all its channels')
    uint32_t blocks = 0;         // of them the ring holds
    uint32_t fileBlocks = 0;
    bool loop = false;
    uint32_t readBlock = 0;      // the file's next block to read
    uint32_t written = 0;        // blocks put in the ring, counting up
    uint32_t floor = 0;          // the oldest still wanted (the one being decoded)
    bool readEnd = false;
    uint32_t generation = 0;
};

struct Voice {
    bool active = false;
    uint32_t handle = 0;
    uint32_t started = 0;        // (the oldest is stolen)
    const Clip* clip = nullptr;
    const Bank* bank = nullptr;
    int ring = -1;
    uint32_t base = 0;           // bank clips: the clip's ARAM address
    uint32_t blocks = 0;         // the clip's
    // Where it is: the clip's block and frame (and, streamed, the ring's block), the next to take.
    uint32_t block = 0, nextBlock = 0, serial = 0, nextSerial = 0;
    uint32_t frame = 0, frames = 0;
    int16_t coefs[2][16];
    int32_t yn1[2], yn2[2];
    int16_t pcm[2][dsp::kFrameSamples];
    uint32_t pcmPos = dsp::kFrameSamples;
    uint32_t played = 0;         // of the clip's samples, this time through
    uint32_t skip = 0;           // samples to drop first (a start part way in)
    int loopsLeft = 1;           // 0: for ever
    uint32_t delay = 0;          // output frames of silence first
    // Resampling: source samples an output frame (16.16), between cur and next.
    uint32_t step = 1 << 16, frac = 0;
    int32_t cur[2] = {}, next[2] = {};
    bool ending = false;
    // Gain, 16.16, ramping to `target` (a fade); `stopAtTarget`: a fade out.
    int32_t gain = 0, target = 0, ramp = 0;
    bool stopAtTarget = false;
    // Its frames copied from ARAM: which block (or ring block) and frames.
    uint32_t windowKey = UINT32_MAX, windowFrame = 0, windowFrames = 0;
    alignas(32) uint8_t window[2][kWindowBytes];
};

bool g_ready = false;
std::vector<Clip> g_clips;
std::map<std::string, int> g_clipNames;
std::vector<Bank> g_banks;
Ring g_rings[kRings];
Voice* g_voices = nullptr;
mutex_t g_lock = LWP_MUTEX_NULL;
sem_t g_readerSem = LWP_SEM_NULL;
int16_t* g_out[kBuffers];
int32_t g_mix[kMixFrames * 2];
alignas(32) uint8_t g_header[dsp::kHeaderBytes];
uint8_t* g_chunk = nullptr;
uint32_t g_serial = 1;
Stats g_stats;
bool g_loggedEarly = false, g_loggedSteal = false, g_loggedRings = false;

struct Lock {
    Lock() { LWP_MutexLock(g_lock); }
    ~Lock() { LWP_MutexUnlock(g_lock); }
};

int32_t ToGain(float volume)
{
    // (At most 2: 40 voices at full scale can't overflow the mix.)
    return static_cast<int32_t>(std::clamp(volume, 0.0f, 2.0f) * 65536.0f);
}

Voice* FromHandle(uint32_t handle)
{
    const uint32_t index = handle & ((1u << kHandleShift) - 1);
    if (handle == 0 || index >= static_cast<uint32_t>(kVoices) || !g_voices) return nullptr;
    Voice& v = g_voices[index];
    return v.active && v.handle == handle ? &v : nullptr;
}

void EndVoice(Voice& v)
{
    if (v.ring >= 0) {
        Ring& r = g_rings[v.ring];
        r.used = false;
        r.generation++;
    }
    v.active = false;
    v.ring = -1;
}

// ---- decoding a voice

uint32_t BlockAddress(const Voice& v, int channel)
{
    if (v.ring >= 0) {
        const Ring& r = g_rings[v.ring];
        return r.at + (v.serial % r.blocks) * r.blockBytes + static_cast<uint32_t>(channel) * dsp::kChannelBlockBytes;
    }
    return v.base + (v.block * v.clip->channels + static_cast<uint32_t>(channel)) * dsp::kChannelBlockBytes;
}

// The voice's next frame decoded into pcm; false at the end of its data, or (`starved`) when
// a stream's block hasn't come from the disc yet.
bool DecodeNext(Voice& v, bool& starved)
{
    const int channels = v.clip->channels;
    if (v.frame >= v.frames) {
        if (v.nextBlock >= v.blocks) return false;
        if (v.ring >= 0) {
            Ring& r = g_rings[v.ring];
            if (v.nextSerial >= r.written) {
                starved = true;
                return false;
            }
            v.serial = v.nextSerial++;
            r.floor = v.serial;
            LWP_SemPost(g_readerSem); // (room for another block)
        }
        v.block = v.nextBlock++;
        for (int c = 0; c < channels; c++) {
            AUD_AramDma(false, g_header, BlockAddress(v, c), dsp::kHeaderBytes);
            const dsp::BlockHeader& h = *reinterpret_cast<const dsp::BlockHeader*>(g_header);
            std::memcpy(v.coefs[c], h.coefs, sizeof(h.coefs));
            v.yn1[c] = h.yn1;
            v.yn2[c] = h.yn2;
            v.frames = h.frames;
        }
        v.frame = 0;
        v.windowKey = UINT32_MAX;
        if (v.frames == 0) return false;
    }
    const uint32_t key = v.ring >= 0 ? v.serial : v.block;
    if (key != v.windowKey || v.frame < v.windowFrame || v.frame >= v.windowFrame + v.windowFrames) {
        const uint32_t first = v.frame & ~(kWindowFrames - 1);
        const uint32_t count = std::min(kWindowFrames, v.frames - first);
        for (int c = 0; c < channels; c++)
            AUD_AramDma(false, v.window[c], BlockAddress(v, c) + dsp::kHeaderBytes + first * dsp::kFrameBytes,
                        (count * dsp::kFrameBytes + 31) & ~31u);
        v.windowKey = key;
        v.windowFrame = first;
        v.windowFrames = count;
    }
    for (int c = 0; c < channels; c++)
        dsp::DecodeFrame(v.window[c] + (v.frame - v.windowFrame) * dsp::kFrameBytes, v.coefs[c], v.yn1[c], v.yn2[c], v.pcm[c]);
    v.frame++;
    v.pcmPos = 0;
    return true;
}

// The voice's next sample (both channels; a mono one twice); false at its end, or starved.
bool NextSample(Voice& v, int32_t* out, bool& starved)
{
    for (;;) {
        if (v.played >= v.clip->samples) {
            if (v.loopsLeft == 1) return false;
            if (v.loopsLeft > 1) v.loopsLeft--;
            v.played = 0;
            v.nextBlock = 0;
            v.frame = v.frames = 0;
            v.pcmPos = dsp::kFrameSamples;
        }
        if (v.pcmPos >= dsp::kFrameSamples && !DecodeNext(v, starved)) return false;
        const int last = v.clip->channels - 1;
        out[0] = v.pcm[0][v.pcmPos];
        out[1] = v.pcm[last][v.pcmPos];
        v.pcmPos++;
        v.played++;
        if (v.skip == 0) return true;
        v.skip--;
    }
}

void MixVoice(Voice& v)
{
    int n = 0;
    if (v.delay) {
        const uint32_t wait = std::min<uint32_t>(v.delay, kMixFrames);
        v.delay -= wait;
        n = static_cast<int>(wait);
    }
    for (; n < kMixFrames; n++) {
        while (v.frac >= (1u << 16)) {
            if (v.ending) {
                EndVoice(v);
                return;
            }
            int32_t s[2];
            bool starved = false;
            if (!NextSample(v, s, starved)) {
                if (starved) {
                    g_stats.starved++;
                    return; // (the disc: it goes on from here when the ring has more)
                }
                v.ending = true; // one more step, to the last sample
                s[0] = v.next[0];
                s[1] = v.next[1];
            }
            v.cur[0] = v.next[0];
            v.cur[1] = v.next[1];
            v.next[0] = s[0];
            v.next[1] = s[1];
            v.frac -= 1u << 16;
        }
        if (v.ramp != 0) {
            v.gain += v.ramp;
            if ((v.ramp > 0 && v.gain >= v.target) || (v.ramp < 0 && v.gain <= v.target)) {
                v.gain = v.target;
                v.ramp = 0;
                if (v.stopAtTarget) {
                    EndVoice(v);
                    return;
                }
            }
        }
        const int32_t t = static_cast<int32_t>(v.frac);
        const int32_t l = v.cur[0] + (((v.next[0] - v.cur[0]) * t) >> 16);
        const int32_t r = v.cur[1] + (((v.next[1] - v.cur[1]) * t) >> 16);
        const int32_t g = v.gain >> 8;
        g_mix[n * 2] += (l * g) >> 8;
        g_mix[n * 2 + 1] += (r * g) >> 8;
        v.frac += v.step;
    }
}

void Mix(int16_t* out)
{
    const u64 started = gettime();
    std::memset(g_mix, 0, sizeof(g_mix));
    {
        Lock lock;
        for (int i = 0; i < kVoices; i++)
            if (g_voices[i].active) MixVoice(g_voices[i]);
    }
    for (int k = 0; k < kMixFrames * 2; k++) out[k] = static_cast<int16_t>(std::clamp(g_mix[k], -32768, 32767));
    // Its time, averaged (for the stats: the CPU the sound takes).
    const uint32_t us = static_cast<uint32_t>(ticks_to_microsecs(gettime() - started));
    g_stats.mixMicros = (g_stats.mixMicros * 15 + us) / 16;
    g_stats.mixMicrosMost = std::max(g_stats.mixMicrosMost, us);
}

void VoiceCallback(s32) {} // (non-null: an underrun waits, rather than ending the voice)

void* MixerMain(void*)
{
    bool started = false;
    int16_t* pending = nullptr;
    for (;;) {
        // ASND stopped and started again (Octave does it to mount a memory card put in after
        // boot): the voice is gone, so it is set again.
        if (started && ASND_StatusVoice(kAsndVoice) == SND_UNUSED) {
            started = false;
            pending = nullptr;
        }
        if (!started) {
            Mix(g_out[0]);
            DCFlushRange(g_out[0], kMixFrames * 4);
            if (ASND_SetVoice(kAsndVoice, VOICE_STEREO_16BIT, kOutputRate, 0, g_out[0], kMixFrames * 4, MAX_VOLUME, MAX_VOLUME,
                              VoiceCallback) == SND_OK)
                started = true;
        } else if (ASND_TestVoiceBufferReady(kAsndVoice) == 1) {
            // A buffer mixed and not yet taken is offered again as it is: ASND refuses one until
            // its mixer has picked up the one before.
            if (!pending) {
                for (int16_t* buf : g_out) {
                    if (ASND_TestPointer(kAsndVoice, buf) == SND_BUSY) continue;
                    Mix(buf);
                    DCFlushRange(buf, kMixFrames * 4);
                    pending = buf;
                    break;
                }
            }
            if (pending && ASND_AddVoice(kAsndVoice, pending, kMixFrames * 4) == SND_OK) {
                pending = nullptr;
                g_stats.buffersMixed++;
            }
        }
        usleep(4000);
    }
    return nullptr;
}

// ---- the reader: the streams first, then the banks, else it waits

// One read for a stream whose ring has room; false if there was nothing to do.
bool FeedStream()
{
    std::string path;
    int which = -1;
    uint32_t generation = 0, first = 0, count = 0, blockBytes = 0;
    {
        Lock lock;
        // The ring with the fewest blocks ahead.
        uint32_t fewest = UINT32_MAX;
        for (int i = 0; i < kRings; i++) {
            const Ring& r = g_rings[i];
            if (!r.used || r.readEnd) continue;
            const uint32_t ahead = r.written - r.floor;
            const uint32_t room = r.blocks - ahead;
            const uint32_t want = std::min<uint32_t>(kChunkBytes / r.blockBytes, r.blocks / 2);
            if (room >= want && ahead < fewest) {
                fewest = ahead;
                which = i;
            }
        }
        if (which < 0) return false;
        Ring& r = g_rings[which];
        if (r.readBlock >= r.fileBlocks) r.readBlock = 0; // (looping)
        path = std::string(kRoot) + g_clips[static_cast<size_t>(r.clip)].stream;
        generation = r.generation;
        first = r.readBlock;
        blockBytes = r.blockBytes;
        count = std::min({kChunkBytes / r.blockBytes, r.blocks - (r.written - r.floor), r.fileBlocks - first});
    }
    const bool ok = SYS_ReadFileRange(path.c_str(), true, first * blockBytes, count * blockBytes, reinterpret_cast<char*>(g_chunk));
    // A failed read is tried again (a console's disc reads fail now and then); only after many in
    // a row (5 s of them; the ring holds more) is the stream given up.
    static uint32_t failures = 0;
    if (!ok) {
        failures++;
        g_stats.readFailures++;
        AUDIO_LOG("audio: reading %s at block %u failed (%u in a row)\n", path.c_str(), first, failures);
        if (failures < 100) {
            usleep(50 * 1000);
            return true;
        }
    }
    Lock lock;
    Ring& r = g_rings[which];
    if (!r.used || r.generation != generation) {
        failures = 0;
        return true; // (another stream there now)
    }
    if (!ok) {
        failures = 0;
        r.readEnd = true;
        return true;
    }
    failures = 0;
    for (uint32_t i = 0; i < count; i++)
        AUD_AramDma(true, g_chunk + i * blockBytes, r.at + ((r.written + i) % r.blocks) * blockBytes, blockBytes);
    r.written += count;
    r.readBlock = first + count;
    if (r.readBlock >= r.fileBlocks) {
        if (r.loop) r.readBlock = 0;
        else r.readEnd = true;
    }
    return true;
}

// The next piece of a wanted bank into ARAM; false when they're all there.
bool FeedBank()
{
    std::string path;
    int which = -1;
    uint32_t generation = 0, at = 0, size = 0;
    {
        Lock lock;
        for (size_t i = 0; i < g_banks.size(); i++) {
            const Bank& b = g_banks[i];
            if (!b.wanted || !b.aram || b.loaded >= b.bytes) continue;
            if (which < 0 || b.order < g_banks[static_cast<size_t>(which)].order) which = static_cast<int>(i);
        }
        if (which < 0) return false;
        const Bank& b = g_banks[static_cast<size_t>(which)];
        path = std::string(kRoot) + b.file;
        generation = b.generation;
        at = b.loaded;
        size = std::min(kChunkBytes & ~31u, b.bytes - at);
    }
    const bool ok = SYS_ReadFileRange(path.c_str(), true, at, size, reinterpret_cast<char*>(g_chunk));
    Lock lock;
    Bank& b = g_banks[static_cast<size_t>(which)];
    if (b.generation != generation) return true;
    if (!ok) {
        g_stats.readFailures++;
        AUDIO_LOG("audio: reading %s at %u failed: its sounds after it are silent\n", path.c_str(), at);
        LogError("audio: reading %s failed", path.c_str());
        b.bytes = at; // (what's in plays)
        return true;
    }
    AUD_AramDma(true, g_chunk, b.aram + at, (size + 31) & ~31u);
    b.loaded = at + size;
    if (b.loaded >= b.bytes) AUDIO_LOG("audio: bank %s in ARAM (%u KB at 0x%x)\n", b.name.c_str(), b.bytes / 1024, b.aram);
    return true;
}

void* ReaderMain(void*)
{
    OctSd_NoteThreadPriority(50); // (as created in Init)
    for (;;) {
        if (FeedStream()) continue;
        if (FeedBank()) continue;
        LWP_SemWait(g_readerSem);
    }
    return nullptr;
}

} // namespace

bool Init(const json::Value& index)
{
    if (g_ready) return true;
    if (index["rate"].AsInt() != kOutputRate || index["blockFrames"].AsInt() != static_cast<int64_t>(dsp::kBlockFrames)) {
        LogError("audio: index.json is for another format");
        return false;
    }
    std::map<std::string, int> bankIndex;
    for (const auto& [name, b] : index["banks"].obj) {
        Bank bank;
        bank.name = name;
        bank.file = b["file"].s;
        bank.bytes = static_cast<uint32_t>(b["bytes"].AsInt());
        bankIndex[name] = static_cast<int>(g_banks.size());
        g_banks.push_back(bank);
    }
    for (const auto& [key, c] : index["clips"].obj) {
        Clip clip;
        clip.name = key;
        if (const json::Value* banks = c.Find("banks")) {
            for (const json::Value& b : banks->arr) {
                auto it = bankIndex.find(b[0].s);
                if (it != bankIndex.end()) clip.banks.emplace_back(it->second, static_cast<uint32_t>(b[1].AsInt()));
            }
            if (clip.banks.empty()) continue;
        } else {
            clip.stream = c["stream"].s;
        }
        clip.bytes = static_cast<uint32_t>(c["bytes"].AsInt());
        clip.rate = static_cast<uint32_t>(c["rate"].AsInt());
        clip.samples = static_cast<uint32_t>(c["samples"].AsInt());
        clip.channels = static_cast<uint8_t>(c["channels"].AsInt());
        if (clip.channels < 1 || clip.channels > 2 || clip.rate == 0 || clip.rate > kOutputRate) continue;
        g_clipNames[key] = static_cast<int>(g_clips.size());
        g_clips.push_back(clip);
    }
    // The streams' rings.
    for (Ring& r : g_rings) {
        r.at = AUD_AllocAram(kRingBytes);
        if (r.at == 0) {
            LogError("audio: no room in ARAM for the streams");
            return false;
        }
        g_stats.aramRings += kRingBytes;
    }
    g_voices = static_cast<Voice*>(memalign(32, sizeof(Voice) * kVoices));
    g_chunk = static_cast<uint8_t*>(memalign(32, kChunkBytes));
    for (int16_t*& b : g_out) b = static_cast<int16_t*>(memalign(32, kMixFrames * 4));
    if (!g_voices || !g_chunk || !g_out[kBuffers - 1]) return false;
    for (int i = 0; i < kVoices; i++) new (&g_voices[i]) Voice();
    g_stats.mram = sizeof(Voice) * kVoices + kChunkBytes + kBuffers * kMixFrames * 4 + sizeof(g_mix) +
                   static_cast<uint32_t>(g_clips.size() * (sizeof(Clip) + 32) + g_banks.size() * sizeof(Bank)) + 16 * 1024 +
                   64 * 1024;
    LWP_MutexInit(&g_lock, false);
    LWP_SemInit(&g_readerSem, 0, 64);
    // The mixer above the main thread (64), so a long frame can't starve the sound; the reader
    // below it (as Octave's audio reader, 50): the rings are long enough to wait for the main
    // thread's spare time.
    static uint8_t mixerStack[16 * 1024] __attribute__((aligned(32)));
    static uint8_t readerStack[64 * 1024] __attribute__((aligned(32)));
    lwp_t thread;
    if (LWP_CreateThread(&thread, ReaderMain, nullptr, readerStack, sizeof(readerStack), 50) != 0) return false;
    if (LWP_CreateThread(&thread, MixerMain, nullptr, mixerStack, sizeof(mixerStack), 80) != 0) return false;
    g_ready = true;
    AUDIO_LOG("audio: %u clips, %u banks; rings %u KB in ARAM\n", static_cast<unsigned>(g_clips.size()),
              static_cast<unsigned>(g_banks.size()), static_cast<unsigned>(g_stats.aramRings / 1024));
    return true;
}

bool Ready() { return g_ready; }

int FindClip(const std::string& key)
{
    auto it = g_clipNames.find(key);
    return it == g_clipNames.end() ? -1 : it->second;
}

const std::string& ClipName(int clip)
{
    static const std::string none;
    return clip >= 0 && clip < static_cast<int>(g_clips.size()) ? g_clips[static_cast<size_t>(clip)].name : none;
}

float ClipLength(int clip)
{
    if (clip < 0 || clip >= static_cast<int>(g_clips.size())) return 0.0f;
    const Clip& c = g_clips[static_cast<size_t>(clip)];
    return static_cast<float>(c.samples) / static_cast<float>(c.rate);
}

void WantBanks(const std::vector<std::string>& banks)
{
    if (!g_ready) return;
    {
        Lock lock;
        for (Bank& b : g_banks) {
            const auto at = std::find(banks.begin(), banks.end(), b.name);
            const bool wanted = at != banks.end();
            b.order = static_cast<int>(at - banks.begin());
            if (wanted == b.wanted) continue;
            b.wanted = wanted;
            b.generation++;
            if (!wanted) {
                for (int i = 0; i < kVoices; i++)
                    if (g_voices[i].active && g_voices[i].bank == &b) EndVoice(g_voices[i]);
                if (b.aram) AUD_FreeAram(b.aram);
                if (b.aram) g_stats.aramBanks -= (b.bytes + 31) & ~31u;
                b.aram = 0;
                b.loaded = 0;
                AUDIO_LOG("audio: bank %s out of ARAM\n", b.name.c_str());
            }
        }
        for (const std::string& name : banks)
            for (Bank& b : g_banks) {
                if (b.name != name || b.aram || b.bytes == 0) continue;
                b.aram = AUD_AllocAram((b.bytes + 31) & ~31u);
                b.loaded = 0;
                if (b.aram == 0) {
                    uint32_t total = 0, free = 0, sounds = 0;
                    AUD_GetAramStats(total, free, sounds);
                    LogError("audio: no room in ARAM for bank %s (%u KB, %u KB free)", b.name.c_str(), b.bytes / 1024, free / 1024);
                    AUDIO_LOG("audio: no room in ARAM for bank %s (%u KB, %u KB free)\n", b.name.c_str(), b.bytes / 1024, free / 1024);
                } else {
                    g_stats.aramBanks += (b.bytes + 31) & ~31u;
                }
            }
    }
    LWP_SemPost(g_readerSem);
}

bool BankLoaded(const std::string& bank)
{
    Lock lock;
    for (const Bank& b : g_banks)
        if (b.name == bank) return b.aram && b.loaded >= b.bytes;
    return false;
}

uint32_t Play(int clipIndex, const PlayParams& p)
{
    if (!g_ready || clipIndex < 0 || clipIndex >= static_cast<int>(g_clips.size())) return 0;
    const Clip& clip = g_clips[static_cast<size_t>(clipIndex)];
    uint32_t handle = 0;
    {
        Lock lock;
        // A bank it's in, read into ARAM that far.
        const Bank* bank = nullptr;
        uint32_t offset = 0;
        for (const auto& [b, at] : clip.banks) {
            const Bank& candidate = g_banks[static_cast<size_t>(b)];
            if (candidate.aram && at + clip.bytes <= candidate.loaded) {
                bank = &candidate;
                offset = at;
                break;
            }
        }
        if (!clip.banks.empty() && !bank) {
            g_stats.unplayed++;
            if (!g_loggedEarly) {
                g_loggedEarly = true;
                AUDIO_LOG("audio: mismatch: %s played before its bank (%s) was in ARAM (silent)\n", clip.name.c_str(),
                          g_banks[static_cast<size_t>(clip.banks[0].first)].name.c_str());
            }
            return 0;
        }
        int ring = -1;
        if (!bank) {
            for (int i = 0; i < kRings && ring < 0; i++)
                if (!g_rings[i].used) ring = i;
            if (ring < 0) {
                g_stats.unplayed++;
                if (!g_loggedRings) {
                    g_loggedRings = true;
                    AUDIO_LOG("audio: mismatch: all %d streams busy (%s not played)\n", kRings, clip.name.c_str());
                }
                return 0;
            }
        }
        // A free voice, else the oldest sound effect that ends by itself.
        Voice* v = nullptr;
        for (int i = 0; i < kVoices && !v; i++)
            if (!g_voices[i].active) v = &g_voices[i];
        if (!v) {
            for (int i = 0; i < kVoices; i++) {
                Voice& o = g_voices[i];
                if (o.ring < 0 && o.loopsLeft != 0 && (!v || o.started < v->started)) v = &o;
            }
            if (!v) {
                g_stats.unplayed++;
                return 0;
            }
            g_stats.stolen++;
            if (!g_loggedSteal) {
                g_loggedSteal = true;
                AUDIO_LOG("audio: mismatch: all %d voices busy (%s took the oldest's)\n", kVoices, clip.name.c_str());
            }
            EndVoice(*v);
        }
        const int index = static_cast<int>(v - g_voices);
        *v = Voice();
        v->clip = &clip;
        v->bank = bank;
        v->blocks = (clip.samples + dsp::kBlockSamples - 1) / dsp::kBlockSamples;
        v->base = bank ? bank->aram + offset : 0;
        const uint32_t startSample = std::min(static_cast<uint32_t>(std::clamp(p.start, 0.0f, 1.0f) * static_cast<float>(clip.samples)),
                                              clip.samples > 0 ? clip.samples - 1 : 0);
        v->nextBlock = startSample / dsp::kBlockSamples;
        v->played = v->nextBlock * dsp::kBlockSamples;
        v->skip = startSample - v->played;
        v->loopsLeft = std::max(0, p.loops);
        v->delay = static_cast<uint32_t>(std::max(0.0f, p.delay) * kOutputRate);
        v->step = static_cast<uint32_t>(static_cast<float>(clip.rate) * std::clamp(p.pitch, 0.1f, 4.0f) / kOutputRate * 65536.0f);
        v->frac = 2u << 16; // the first two samples are read before the first is played
        v->target = ToGain(p.volume);
        if (p.fadeIn > 0.0f) {
            v->gain = 0;
            v->ramp = std::max(1, static_cast<int32_t>(static_cast<float>(v->target) / (p.fadeIn * kOutputRate)));
        } else {
            v->gain = v->target;
        }
        if (ring >= 0) {
            Ring& r = g_rings[ring];
            r.used = true;
            r.clip = clipIndex;
            r.blockBytes = clip.channels * dsp::kChannelBlockBytes;
            r.blocks = kRingBytes / r.blockBytes;
            r.fileBlocks = v->blocks;
            r.loop = p.loops == 0 || p.loops > 1;
            r.readBlock = v->nextBlock;
            r.written = r.floor = 0;
            r.readEnd = false;
            r.generation++;
            v->ring = ring;
        }
        v->handle = (g_serial++ << kHandleShift) | static_cast<uint32_t>(index);
        if ((v->handle >> kHandleShift) == 0) v->handle = (g_serial++ << kHandleShift) | static_cast<uint32_t>(index);
        v->started = v->handle >> kHandleShift;
        v->active = true;
        handle = v->handle;
    }
    if (clip.banks.empty()) LWP_SemPost(g_readerSem);
    return handle;
}

void Stop(uint32_t handle, float fadeOut)
{
    if (!g_ready) return;
    Lock lock;
    Voice* v = FromHandle(handle);
    if (!v) return;
    if (fadeOut <= 0.0f || v->gain == 0) {
        EndVoice(*v);
        return;
    }
    v->target = 0;
    v->ramp = -std::max(1, static_cast<int32_t>(static_cast<float>(v->gain) / (fadeOut * kOutputRate)));
    v->stopAtTarget = true;
}

void SetVolume(uint32_t handle, float volume, float seconds)
{
    if (!g_ready) return;
    Lock lock;
    Voice* v = FromHandle(handle);
    if (!v || v->stopAtTarget) return;
    v->target = ToGain(volume);
    // (Not at once: a step in the gain clicks. At least 8 ms.)
    const float frames = std::max(seconds * kOutputRate, 256.0f);
    const int32_t delta = v->target - v->gain;
    v->ramp = delta == 0 ? 0 : (delta > 0 ? 1 : -1) * std::max(1, static_cast<int32_t>(static_cast<float>(std::abs(delta)) / frames));
}

bool Playing(uint32_t handle)
{
    if (!g_ready) return false;
    Lock lock;
    return FromHandle(handle) != nullptr;
}

Stats GetStats()
{
    Stats s;
    if (!g_ready) return s;
    Lock lock;
    s = g_stats;
    for (int i = 0; i < kVoices; i++) {
        if (!g_voices[i].active) continue;
        s.voices++;
        if (g_voices[i].ring >= 0) {
            s.streams++;
            const Ring& r = g_rings[g_voices[i].ring];
            s.streamAhead = std::max(s.streamAhead, r.written - r.floor);
        }
    }
    uint32_t sounds = 0;
    AUD_GetAramStats(s.aramTotal, s.aramFree, sounds);
    return s;
}

} // namespace gcaudio
