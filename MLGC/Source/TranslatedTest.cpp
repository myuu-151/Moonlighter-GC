// TTEST=1 builds (test builds, Dolphin only; Makefile_GCN): the BabySlime's oracle replay on the
// GameCube, against the game's own code translated (Moonlighter-Recomp's
// tests/translated/enemy_translated_test.cpp, its ctest slime_translated_gc: the collector at every
// frame end) and against the hand port (tests/slime_test.cpp). The same programs as on the PC, their
// main()s renamed: the recording's inputs drive the enemies and every row is compared bit for bit.
//
// - Files: the replays read with std::ifstream; here a "disc:" device serves them from the disc
//   through Octave's file access (SYS_AcquireFileData), from MLGC/Scripts/Data/ttest/
//   (tools/make_test_data.py puts the recording and the data the replays need there).
// - Output: printf to Dolphin's log (OSReport: SYS_STDIO_Report), read back by
//   tools/dolphin_test.ps1. Never on a console: OSReport overwrites the RTC counter.
// - Time: each phase of the game's work per frame (tests/replay_profile.h), on the Gekko's
//   timebase (40.5 MHz: 12 core cycles a tick). In Dolphin the timebase follows its own count of
//   the emulated cycles (no cache misses, no pipeline stalls): see docs/gamecube_translated.md in
//   Moonlighter-Recomp for how far that is from the console.
// - Memory: newlib's malloc (mallinfo) and the arena left, per frame end; the collector's live
//   objects and what each collection frees.
// - TTEST_PROFILE=1: only the translated replay, sampled every 100 us by a tick task (an interrupt)
//   that records where the main thread was and its callers (the stack's back chain), inside the
//   timed phases; the samples go to the log for tools/ttest_profile.py (symbols from MLGC.elf).
#include "TranslatedTest.h"

#if MLGC_TTEST

#ifndef MLGC_TTEST_PROFILE
#define MLGC_TTEST_PROFILE 0
#endif

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "System/System.h"
#include "cs/Object.h"
#include "cs/String.h"
#include "replay_frames.h"
#include "replay_profile.h"
#include "util/Json.h"

#if MLGC_TTEST_PROFILE
#include <tuxedo/thread.h>
#include <tuxedo/tick.h>
#endif

// The replays (Moonlighter-Recomp's tests, built with ML_TEST_MAIN: Makefile_GCN).
int EnemyTranslatedTestMain(int argc, char** argv);
int SlimeTestMain(int argc, char** argv);
// Source/TranslatedLoad.cpp
void MeasureConstantsLoad(uint64_t (*clock)(), uint32_t (*inUse)(), double ticksPerMicrosecond);

namespace {

// ---- "disc:": the disc's files for std::ifstream ---------------------------------------------------

const char* const kDiscRoot = "MLGC/Scripts/Data/";

struct DiscFile {
    char* data;
    uint32_t size;
    uint32_t pos;
};

int DiscOpen(struct _reent* r, void* fileStruct, const char* path, int flags, int /*mode*/)
{
    if ((flags & O_ACCMODE) != O_RDONLY) {
        r->_errno = EROFS;
        return -1;
    }
    const char* p = strchr(path, ':');
    p = p ? p + 1 : path;
    while (*p == '/') p++;
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData((std::string(kDiscRoot) + p).c_str(), true, 0, data, size);
    if (data == nullptr) {
        r->_errno = ENOENT;
        return -1;
    }
    DiscFile* f = static_cast<DiscFile*>(fileStruct);
    f->data = data;
    f->size = size;
    f->pos = 0;
    return 0;
}

int DiscClose(struct _reent*, void* fd)
{
    DiscFile* f = static_cast<DiscFile*>(fd);
    SYS_ReleaseFileData(f->data);
    f->data = nullptr;
    return 0;
}

ssize_t DiscRead(struct _reent*, void* fd, char* ptr, size_t len)
{
    DiscFile* f = static_cast<DiscFile*>(fd);
    const uint32_t left = f->size - f->pos;
    const uint32_t n = len < left ? static_cast<uint32_t>(len) : left;
    memcpy(ptr, f->data + f->pos, n);
    f->pos += n;
    return static_cast<ssize_t>(n);
}

off_t DiscSeek(struct _reent* r, void* fd, off_t pos, int dir)
{
    DiscFile* f = static_cast<DiscFile*>(fd);
    off_t to = dir == SEEK_SET ? pos : dir == SEEK_CUR ? static_cast<off_t>(f->pos) + pos : static_cast<off_t>(f->size) + pos;
    if (to < 0 || to > static_cast<off_t>(f->size)) {
        r->_errno = EINVAL;
        return -1;
    }
    f->pos = static_cast<uint32_t>(to);
    return to;
}

int DiscFstat(struct _reent*, void* fd, struct stat* st)
{
    memset(st, 0, sizeof *st);
    st->st_size = static_cast<DiscFile*>(fd)->size;
    st->st_mode = S_IFREG | S_IRUSR;
    return 0;
}

devoptab_t gDisc;

void MountDisc()
{
    gDisc.name = "disc";
    gDisc.structSize = sizeof(DiscFile);
    gDisc.open_r = DiscOpen;
    gDisc.close_r = DiscClose;
    gDisc.read_r = DiscRead;
    gDisc.seek_r = DiscSeek;
    gDisc.fstat_r = DiscFstat;
    AddDevice(&gDisc);
}

// ---- time and memory ---------------------------------------------------------------------------------

uint64_t Timebase() { return gettime(); }

struct Mem {
    uint32_t inUse; // malloc'd, not freed
    uint32_t arena; // taken from the arena by malloc (its high-water mark: newlib never gives back)
    uint32_t free;  // malloc's free blocks and the arena it hasn't taken
};

Mem Now()
{
    const struct mallinfo mi = mallinfo();
    return {static_cast<uint32_t>(mi.uordblks), static_cast<uint32_t>(mi.arena),
            static_cast<uint32_t>(mi.fordblks) + static_cast<uint32_t>((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())};
}

uint32_t InUse() { return Now().inUse; }

void Report(const char* what)
{
    const Mem m = Now();
    printf("MEM %s: in use %u KB, malloc's arena %u KB, free %u KB\n", what, m.inUse / 1024, m.arena / 1024, m.free / 1024);
}

// Per frame end and per collection, through replay_profile's hooks.
struct Stats {
    uint64_t sum = 0;
    uint32_t min = 0xFFFFFFFFu, max = 0, n = 0;
    void Add(uint32_t v)
    {
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
        n++;
    }
    double Mean() const { return n ? static_cast<double>(sum) / n : 0.0; }
};
struct Memory {
    uint32_t start = 0, recording = 0; // in use before the replay; the replay's own copy of the recording
    Stats inUse, liveObjects, freedObjects, freedBytes, newObjects, newBytes;
    uint32_t beforeCollect = 0, objectsBeforeCollect = 0;
    uint32_t lastInUse = 0, lastObjects = 0;
    bool started = false;
};
Memory gMem;
bool gCensus = false; // (the translated replay: its heap counted by type at its middle frame)

// The heap's objects by class: how many, their own blocks (malloc_usable_size), and the strings'
// UTF-16 buffers (the other containers' buffers aren't counted).
void Census(const char* when)
{
    struct Row {
        const char* name;
        uint32_t count = 0, bytes = 0, buffers = 0;
        bool traced = false;
    };
    std::vector<Row> rows;
    uint32_t count = 0, bytes = 0, buffers = 0;
    cs::CurrentHeap().ForEach([&](cs::Object* o) {
        const char* name = o->cs_typename();
        const uint32_t own = static_cast<uint32_t>(malloc_usable_size(dynamic_cast<void*>(o)));
        uint32_t buffer = 0;
        if (cs::String* str = dynamic_cast<cs::String*>(o))
            if (str->str().capacity() > 7) buffer = static_cast<uint32_t>(malloc_usable_size(const_cast<char16_t*>(str->str().data())));
        Row* row = nullptr;
        for (Row& r : rows)
            if (strcmp(r.name, name) == 0) row = &r;
        if (!row) {
            rows.push_back(Row{name});
            row = &rows.back();
        }
        row->count++;
        row->bytes += own;
        row->buffers += buffer;
        row->traced = o->cs_traced();
        count++;
        bytes += own;
        buffers += buffer;
    });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes + a.buffers > b.bytes + b.buffers; });
    printf("\nCENSUS the heap %s: %u objects, %u KB their own blocks, %u KB the strings' buffers; "
           "%u classes, the largest:\n",
           when, static_cast<unsigned>(count), static_cast<unsigned>(bytes / 1024), static_cast<unsigned>(buffers / 1024),
           static_cast<unsigned>(rows.size()));
    for (size_t i = 0; i < rows.size() && i < 30; i++)
        printf("CENSUS %6u %8u %8u %s %s\n", static_cast<unsigned>(rows[i].count), static_cast<unsigned>(rows[i].bytes),
               static_cast<unsigned>(rows[i].buffers), rows[i].traced ? "traced  " : "untraced", rows[i].name);
    // The strings of 1 KB and more, by length: how many of each, and how they start.
    struct Big {
        size_t length;
        uint32_t count;
        std::string start;
    };
    std::vector<Big> big;
    cs::CurrentHeap().ForEach([&](cs::Object* o) {
        cs::String* str = dynamic_cast<cs::String*>(o);
        if (!str || str->str().size() < 512) return;
        for (Big& b : big)
            if (b.length == str->str().size()) {
                b.count++;
                return;
            }
        std::string start;
        for (size_t i = 0; i < 48 && i < str->str().size(); i++) {
            const char16_t c = str->str()[i];
            start += c >= 32 && c < 127 ? static_cast<char>(c) : '.';
        }
        big.push_back(Big{str->str().size(), 1, start});
    });
    std::sort(big.begin(), big.end(), [](const Big& a, const Big& b) { return a.length * a.count > b.length * b.count; });
    for (size_t i = 0; i < big.size() && i < 12; i++)
        printf("CENSUS string of %u chars x %u: %s\n", static_cast<unsigned>(big[i].length), static_cast<unsigned>(big[i].count),
               big[i].start.c_str());
}

void FrameEnd()
{
    const uint32_t inUse = Now().inUse;
    const uint32_t objects = static_cast<uint32_t>(cs::CurrentHeap().Count());
    gMem.inUse.Add(inUse);
    gMem.liveObjects.Add(objects);
    gMem.lastInUse = inUse;
    gMem.lastObjects = objects;
    gMem.started = true;
    if (gCensus && gMem.inUse.n == 364) Census("at the replay's middle frame (after its collection)");
}

volatile int gPhase = -1; // the phase under way (the sampler's)

void PhaseHook(int phase, bool begin)
{
    gPhase = begin ? phase : -1;
    if (phase != replay_profile::Collector) return;
    const uint32_t inUse = Now().inUse;
    const uint32_t objects = static_cast<uint32_t>(cs::CurrentHeap().Count());
    if (begin) {
        if (gCensus && !gMem.started) Census("before the first collection (the loading's garbage still in it)");
        gMem.beforeCollect = inUse;
        gMem.objectsBeforeCollect = objects;
        if (gMem.started) {
            gMem.newObjects.Add(objects - gMem.lastObjects);
            gMem.newBytes.Add(inUse > gMem.lastInUse ? inUse - gMem.lastInUse : 0);
        }
    } else {
        gMem.freedObjects.Add(gMem.objectsBeforeCollect - objects);
        gMem.freedBytes.Add(gMem.beforeCollect > inUse ? gMem.beforeCollect - inUse : 0);
    }
}

void PrintStats(const char* what, const Stats& s, double scale, const char* unit)
{
    if (!s.n) return;
    printf("MEM   %-58s min %8.1f  mean %8.1f  max %8.1f %s (%u)\n", what, s.min * scale, s.Mean() * scale, s.max * scale, unit, s.n);
}

void PrintMemory(const char* replay)
{
    const Mem end = Now();
    printf("\nMEM the %s replay, per frame end:\n", replay);
    printf("MEM   in use before the replay %u KB; the replay's own copy of the recording (its lines, the parsed "
           "start, enemies, maps, colliders, a few frames) %u KB; malloc's arena at its end (newlib's high-water mark) %u KB\n",
           gMem.start / 1024, gMem.recording / 1024, end.arena / 1024);
    PrintStats("heap in use at frame ends (after the collection)", gMem.inUse, 1.0 / 1024, "KB");
    if (gMem.inUse.n) {
        const double game = (static_cast<double>(gMem.inUse.max) - gMem.start - gMem.recording) / 1024;
        printf("MEM   the scene's share at its largest (in use, less what was in use before and the recording) %.1f KB\n", game);
    }
    if (gMem.freedObjects.n) PrintStats("collector: live objects after each collection", gMem.liveObjects, 1.0, "objects");
    PrintStats("collector: objects made in the frame", gMem.newObjects, 1.0, "objects");
    PrintStats("collector: bytes made in the frame (net)", gMem.newBytes, 1.0 / 1024, "KB");
    PrintStats("collector: objects freed per collection", gMem.freedObjects, 1.0, "objects");
    PrintStats("collector: bytes freed per collection", gMem.freedBytes, 1.0 / 1024, "KB");
}

// What the replay's own reading of the recording holds while it runs (its loop, done here alone):
// the frames' lines, the other lines parsed, a few frames parsed.
uint32_t RecordingBytes(const char* file)
{
    const uint32_t before = Now().inUse;
    uint32_t held = 0;
    {
        std::vector<json::Value> other;
        ReplayFrames frames;
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            if (line.empty()) continue;
            json::Value v = json::Parse(line);
            if (v["ev"].s == "frame") frames.push_back(std::move(line));
            else other.push_back(std::move(v));
        }
        in.close();
        for (size_t i = 0; i < 4 && i < frames.size(); i++) (void)frames[i];
        held = Now().inUse - before;
    }
    return held;
}

void StartTiming(const char* recording)
{
    replay_profile::gState.clock = &Timebase;
    replay_profile::gState.ticksPerMicrosecond = 40.5;
    replay_profile::gState.clockName = "the Gekko's timebase in Dolphin";
    replay_profile::gState.frameEnd = &FrameEnd;
    replay_profile::gState.phaseHook = &PhaseHook;
    gMem = Memory();
    gMem.recording = RecordingBytes(recording);
    gMem.start = Now().inUse;
}

// ---- how Dolphin counts the Gekko's cycles ------------------------------------------------------------
// Chains of one instruction, each depending on the last (inline assembly: the counts are exact),
// timed on the timebase (12 core cycles a tick), less an empty loop's time: the cycles Dolphin
// charges for each kind against the console's latency (IBM's 750CL manual). The loads chase
// pointers through a ring of 1 KB (in the L1 cache) and of 4 MB (past the 256 KB L2, in a shuffled
// order: on the console each one goes to main memory).
#define ML_X8(op) op "\n\t" op "\n\t" op "\n\t" op "\n\t" op "\n\t" op "\n\t" op "\n\t" op "\n\t"
__attribute__((noinline)) uint32_t CalEmpty(uint32_t n, uint32_t x)
{
    for (uint32_t i = 0; i < n; i++) asm volatile("" : "+r"(x));
    return x;
}
__attribute__((noinline)) uint32_t CalAdd(uint32_t n, uint32_t x)
{
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("addi %0,%0,1") : "+r"(x));
    return x;
}
__attribute__((noinline)) uint32_t CalMul(uint32_t n, uint32_t x)
{
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("mullw %0,%0,%0") : "+r"(x));
    return x;
}
__attribute__((noinline)) uint32_t CalDiv(uint32_t n, uint32_t x)
{
    const uint32_t d = 3;
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("divw %0,%0,%1") : "+r"(x) : "r"(d));
    return x;
}
__attribute__((noinline)) double CalFAdd(uint32_t n, double x)
{
    const double d = 1.0000001;
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("fadd %0,%0,%1") : "+f"(x) : "f"(d));
    return x;
}
__attribute__((noinline)) double CalFMul(uint32_t n, double x)
{
    const double d = 1.0000001;
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("fmul %0,%0,%1") : "+f"(x) : "f"(d));
    return x;
}
__attribute__((noinline)) double CalFDiv(uint32_t n, double x)
{
    const double d = 1.0000001;
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("fdiv %0,%0,%1") : "+f"(x) : "f"(d));
    return x;
}
__attribute__((noinline)) uint32_t* CalLoad(uint32_t n, uint32_t* p)
{
    for (uint32_t i = 0; i < n; i++) asm volatile(ML_X8("lwz %0,0(%0)") : "+b"(p));
    return p;
}

// A ring of pointers, 32 bytes apart (a cache line each), in a shuffled order.
uint32_t* MakeRing(uint32_t bytes)
{
    const uint32_t n = bytes / 32;
    uint32_t* base = static_cast<uint32_t*>(memalign(32, bytes));
    if (!base) return nullptr;
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    uint32_t seed = 12345;
    for (uint32_t i = n - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t j = (seed >> 8) % (i + 1);
        const uint32_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (uint32_t i = 0; i < n; i++) base[order[i] * 8] = reinterpret_cast<uint32_t>(&base[order[(i + 1) % n] * 8]);
    return base;
}

void Calibrate()
{
    const uint32_t n = 100000;
    volatile uint32_t sink = 0;
    volatile double fsink = 0;
    uint64_t t = gettime();
    sink = CalEmpty(n, 1);
    const uint64_t empty = gettime() - t;
    auto perOp = [&](uint64_t ticks) { return (static_cast<double>(ticks) - static_cast<double>(empty)) * 12.0 / (8.0 * n); };
    printf("\nCAL how Dolphin counts the Gekko's cycles (a chain of 8 x %u dependent instructions, less an empty loop of %.1f cycles a turn):\n",
           static_cast<unsigned>(n), static_cast<double>(empty) * 12.0 / n);
    t = gettime();
    sink = CalAdd(n, 1);
    printf("CAL   addi    %6.2f cycles (the console: 1)\n", perOp(gettime() - t));
    t = gettime();
    sink = CalMul(n, 7);
    printf("CAL   mullw   %6.2f cycles (the console: 2 to 5, by the operands' size)\n", perOp(gettime() - t));
    t = gettime();
    sink = CalDiv(n, 0x7FFFFFFF);
    printf("CAL   divw    %6.2f cycles (the console: 19)\n", perOp(gettime() - t));
    t = gettime();
    fsink = CalFAdd(n, 1.0);
    printf("CAL   fadd    %6.2f cycles (the console: 3 latency)\n", perOp(gettime() - t));
    t = gettime();
    fsink = CalFMul(n, 1.0);
    printf("CAL   fmul    %6.2f cycles (the console: 4 latency, double)\n", perOp(gettime() - t));
    t = gettime();
    fsink = CalFDiv(n, 1.0);
    printf("CAL   fdiv    %6.2f cycles (the console: 31 to 33, double)\n", perOp(gettime() - t));
    for (uint32_t bytes : {1024u, 4u * 1024u * 1024u}) {
        uint32_t* ring = MakeRing(bytes);
        if (!ring) continue;
        CalLoad(bytes / 32, ring); // (once round: the small ring into the cache)
        t = gettime();
        uint32_t* p = CalLoad(n, ring);
        sink = reinterpret_cast<uint32_t>(p);
        printf("CAL   lwz     %6.2f cycles, chasing pointers in %u KB (the console: %s)\n", perOp(gettime() - t), static_cast<unsigned>(bytes / 1024),
               bytes <= 32768 ? "2, an L1 hit" : "a main-memory access each: tens of cycles");
        free(ring);
    }
    (void)sink;
    (void)fsink;
}

// ---- TTEST_PROFILE: the sampler -----------------------------------------------------------------------

#if MLGC_TTEST_PROFILE
constexpr int kDepth = 16;          // return addresses kept per sample
constexpr uint32_t kMaxSamples = 30000;
struct Sample {
    int16_t phase;
    uint16_t depth;
    uint32_t addr[2 + kDepth]; // the PC, LR, then the saved return addresses up the back chain
};
Sample* gSamples = nullptr;
volatile uint32_t gSampleCount = 0;
KTickTask gSampler;
KThread* gMain = nullptr;

inline bool InRam(uint32_t p) { return p >= 0x80000000u && p < 0x81800000u && (p & 3) == 0; }

void TakeSample(KTickTask*)
{
    const int phase = gPhase;
    if (phase < 0 || gSampleCount >= kMaxSamples) return;
    KThread* t = KThreadGetSelf(); // (the interrupted thread: its registers are saved in ctx)
    if (t != gMain) return;
    Sample& s = gSamples[gSampleCount];
    s.phase = static_cast<int16_t>(phase);
    s.addr[0] = t->ctx.pc;
    s.addr[1] = t->ctx.lr;
    uint32_t n = 2, sp = t->ctx.gpr[1];
    while (n < 2 + kDepth && InRam(sp)) {
        const uint32_t back = *reinterpret_cast<const uint32_t*>(sp);
        if (!InRam(back) || back <= sp) break;
        const uint32_t ra = *reinterpret_cast<const uint32_t*>(back + 4);
        if (!InRam(ra)) break;
        s.addr[n++] = ra;
        sp = back;
    }
    s.depth = static_cast<uint16_t>(n);
    gSampleCount = gSampleCount + 1;
}

void StartSampler()
{
    gSamples = static_cast<Sample*>(malloc(sizeof(Sample) * kMaxSamples));
    gMain = KThreadGetSelf();
    gSampleCount = 0;
    KTickTaskStart(&gSampler, TakeSample, PPCUsToTicks(100), PPCUsToTicks(100));
}

void StopSampler()
{
    KTickTaskStop(&gSampler);
    printf("\nSAMPLES %u (every 100 us of the timebase, in the timed phases; addresses less 0x80000000, hex)\n",
           static_cast<unsigned>(gSampleCount));
    char line[300];
    for (uint32_t i = 0; i < gSampleCount; i++) {
        const Sample& s = gSamples[i];
        int k = snprintf(line, sizeof line, "S %d", s.phase);
        for (uint32_t d = 0; d < s.depth && k < static_cast<int>(sizeof line) - 12; d++)
            k += snprintf(line + k, sizeof line - k, " %x", static_cast<unsigned>(s.addr[d] - 0x80000000u));
        printf("%s\n", line);
    }
    printf("SAMPLES END\n");
    free(gSamples);
    gSamples = nullptr;
}
#endif

} // namespace

void RunTranslatedTest()
{
    SYS_STDIO_Report(true); // printf to Dolphin's log
    setvbuf(stdout, nullptr, _IOLBF, 0);
    MountDisc();
    setenv("ML_DATA", "disc:/ttest", 1);
    printf("\nMLGC TTEST: the BabySlime's oracle replay (slime.jsonl) on the GameCube\n");
    Report("at the start");
    Calibrate();
    char recording[] = "disc:/ttest/slime.jsonl";
    char prefab[] = "BabySlime";

    // The translated code, the collector at every frame end (the ctest slime_translated_gc; no poison).
    printf("\nTTEST translated (enemy_translated_test, ML_GC_EVERY_FRAME=1)\n");
    setenv("ML_GC_EVERY_FRAME", "1", 1);
    StartTiming(recording);
#if MLGC_TTEST_PROFILE
    StartSampler();
#endif
    // (On the program's own heap, as the ctest runs it: what the collector sees is the same.)
    char name0[] = "enemy_translated_test";
    char* argv0[] = {name0, recording, prefab, nullptr};
    gCensus = !MLGC_TTEST_PROFILE; // (not where the sampler runs: it would count the census)
    const int translated = EnemyTranslatedTestMain(3, argv0);
    gCensus = false;
#if MLGC_TTEST_PROFILE
    StopSampler();
#endif
    PrintMemory("translated");
    Report("after the translated replay");
    printf("TTEST translated: exit %d\n", translated);

#if !MLGC_TTEST_PROFILE
    // The hand port (slime_test: its self-checks, then the same replay).
    printf("\nTTEST hand port (slime_test)\n");
    unsetenv("ML_GC_EVERY_FRAME");
    StartTiming(recording);
    char name[] = "slime_test";
    char* argv[] = {name, recording, nullptr};
    const int hand = SlimeTestMain(2, argv);
    PrintMemory("hand-port");
    Report("after the hand port's replay");
    printf("TTEST hand port: exit %d\n", hand);
    MeasureConstantsLoad(&Timebase, &InUse, 40.5);
#endif
    replay_profile::gState = replay_profile::State();
    printf("\nMLGC TTEST DONE\n");
}

#endif
