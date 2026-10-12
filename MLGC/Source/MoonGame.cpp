#include "MoonGame.h"

#include <malloc.h>
#include <ogc/pad.h>
#include <ogc/system.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "Engine.h"
#include "Log.h"
#include "game/Constants.h"
#include "game/HoleColliders.h"
#include "game/Items.h"
#include "game/Prefabs.h"
#include "game/RoomPhysics.h"
#include "game/RoomScene.h"
#include "util/Json.h"

using namespace game;

// A test build's line in Dolphin's log (OSReport). Only in AUTOTOUR builds, which run in
// Dolphin: on a console SYS_Report overwrites the RTC counter (Octave's System_Dolphin.cpp).
#if defined(MLGC_AUTOTOUR) || defined(MLGC_AUTOWALK) || defined(MLGC_PADLOG)
#define TOUR_REPORT(...) SYS_Report(__VA_ARGS__)
#else
#define TOUR_REPORT(...) ((void)0)
#endif

namespace {

// Our vertex format slot (Octave's UI uses the low ones).
constexpr u8 kFormat = GX_VTXFMT7;

const char* const kCultureNames[5] = {"", "Golem", "Forest", "Desert", "Tech"};

// A painted texture (RGBA, rows from the bottom) as GX RGB5A3 in 4 x 4 tiles, rows from the top.
void* ToRgb5a3(const RoomTexture& t, uint32_t& bytes)
{
    const int w = (t.width + 3) / 4 * 4, h = (t.height + 3) / 4 * 4;
    bytes = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 2;
    auto* out = static_cast<uint16_t*>(memalign(32, bytes));
    if (!out) return nullptr;
    size_t n = 0;
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
            for (int y = ty; y < ty + 4; y++)
                for (int x = tx; x < tx + 4; x++) {
                    uint16_t v = 0;
                    if (x < t.width && y < t.height) {
                        const uint8_t* p = &t.rgba[(static_cast<size_t>(t.height - 1 - y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)) * 4];
                        v = p[3] >= 0xE0 ? static_cast<uint16_t>(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3))
                                         : static_cast<uint16_t>(((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
                    }
                    out[n++] = v; // the GameCube is big-endian: as GX reads it
                }
    DCFlushRange(out, bytes);
    return out;
}

} // namespace

MoonGame::MoonGame() = default;

MoonGame::~MoonGame()
{
    FreeRoomTextures();
    FreeSpriteTextures();
}

bool MoonGame::Initialize()
{
    try {
        session_.Load();
        heroAnimator_ = unity::AnimatorController::FromJson(json::Parse(data_.Read("animators/hero_merchant.json")));
        // A new game's roll: the starting ability's cooldown; its weapon (the items, then they go).
        {
            const json::Value items = json::Parse(data_.Read("items.json"));
            abilityCooldown_ = Items::StartingAbilityCooldown(items);
            if (const json::Value* save = items.Find("initialSave"))
                if (const json::Value* equipped = save->Find("initialEquippedItems"))
                    for (const json::Value& name : equipped->arr) {
                        const json::Value* item = Items::Find(items, name.s);
                        const json::Value* type = item ? item->Find("weaponType") : nullptr;
                        if (!type || !weaponItem_.empty()) continue;
                        weaponItem_ = name.s;
                        weaponType_ = type->s;
                        weaponArt_ = (*item)["inGameArtPrefabName"].s;
                    }
        }
        if (!weaponArt_.empty()) {
            // animators/<its prefab's name, snake case>.json (tools/extract/animators.py).
            std::string file;
            for (char ch : weaponArt_) file += ch == ' ' ? '_' : static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            weaponAnimator_ = unity::AnimatorController::FromJson(json::Parse(data_.Read("animators/" + file + ".json")));
        }
        commonTiles_.Load(data_.Read("tiles/common.bin"));
        session_.Start(seed_);
        NewFloor();
        return true;
    } catch (const std::exception& e) {
        LogError("mlgc: %s", e.what());
        status_ = std::string("error: ") + e.what();
        return false;
    }
}

void MoonGame::FreeRoomTextures()
{
    for (auto& [_, t] : roomTextures_) free(t.texels);
    roomTextures_.clear();
}

void MoonGame::FreeSpriteTextures()
{
    for (auto& [_, t] : spriteTextures_) free(t.texels);
    spriteTextures_.clear();
}

void MoonGame::NewFloor()
{
    FreeRoomTextures();
    room_ = render::RoomDraw();
    floor_.reset(); // its heap goes before the next floor's is made
    const Culture culture = static_cast<Culture>(culture_);
    // A floor's sprites and tiles: only this floor's are kept.
    FreeSpriteTextures();
    tiles_.clear();
    floor_ = session_.Generate(culture, level_);
    if (bank_.Folder() != std::string("sprites/") + Session::PackName(culture) + "/") {
        bank_.Load(data_, std::string("sprites/") + Session::PackName(culture) + "/",
                   {"sprites/hero/", "sprites/weapons/broom_spear/", "sprites/golem/", "sprites/forest/", "sprites/desert/", "sprites/tech/"});
    }
    SetUpHero();
    // Start in the floor's first room (the initial room).
    cell_ = 0;
    for (size_t i = 0; i < floor_->dungeon->cells.size(); i++)
        if (floor_->dungeon->cells[i].cave == floor_->dungeon->layout.initCaveRoom) cell_ = static_cast<int>(i);
    EnterRoom(cell_);
}

// Will's settings from his prefab (hero_merchant's HeroMerchant, in every pack) and the
// constants table; his Animator from animators/hero_merchant.json.
void MoonGame::SetUpHero()
{
    if (hero_) return;
    const Prefab* p = PrefabLibrary::Find("HeroMerchant", "hero_merchant");
    if (!p) throw std::runtime_error("no HeroMerchant prefab in the pack");
    HeroMerchantController::Settings s = HeroMerchantController::SettingsFrom(*p);
    s.abilityCooldown = abilityCooldown_ > 0.0f ? abilityCooldown_ : 0.0f;
    heroDepthOffset_ = HeroMerchantController::DepthOffset(*p);
    const std::vector<std::string>& layers = PrefabLibrary::SortingLayers();
    for (size_t i = 0; i < layers.size(); i++)
        if (layers[i] == "Default") defaultLayer_ = static_cast<int>(i);
    hero_ = std::make_unique<HeroMerchantController>(heroAnimator_, s, physics_);
    hero_->currentPlayerMoveSpeed = Constants::GetFloat("kBaseSpeed");
    hero_->SetDirection(HeroMerchantController::Front);
    // His weapon: its prefab (the items bundle's, in every pack) for the hitbox's defaults.
    if (const Prefab* w = weaponArt_.empty() ? nullptr : PrefabLibrary::Find(weaponArt_, "items")) {
        weapon_ = std::make_unique<Weapon>(weaponAnimator_, *w, WeaponTypeFromName(weaponType_));
        hero_->weapon = weapon_.get();
    } else if (!weaponArt_.empty()) {
        LogError("mlgc: no %s prefab in the pack: %s can't attack", weaponArt_.c_str(), weaponItem_.c_str());
    }
}

void MoonGame::EnterRoom(int cell, int from, const unity::Vector2* arrival)
{
    FreeRoomTextures();
    cell_ = cell;
    const PlacedCell& c = floor_->dungeon->cells[static_cast<size_t>(cell)];
    const unity::Vector2 roomPos = RoomPosition(c.x, c.y);
    // The room's colliders, and Will inside the door he came through (or in the middle).
    physics_.Clear();
    AddRoomColliders(RoomSceneTree(*c.room, roomPos), roomPos, physics_);
    AddHoleColliders(*c.room, roomPos, physics_);
    if (hero_) {
        static const float kEntry[4][2] = {{0.0f, 100.0f}, {0.0f, -112.0f}, {-216.0f, 0.0f}, {216.0f, 0.0f}};
        hero_->position = arrival ? *arrival
                          : from >= 0 ? unity::Vector2{roomPos.x + kEntry[from][0], roomPos.y + kEntry[from][1]}
                                      : roomPos;
        hero_->velX = hero_->velY = 0.0f;
        hero_->timePushingDoors = 0.0f;
        // Where a fall puts him back: the door he came in by (TeleportHeroToLastDoorUsed), or in
        // the floor's first room his last spot clear of holes.
        hero_->roomIsInit = c.cave == floor_->dungeon->layout.initCaveRoom;
        hero_->respawn = hero_->position;
        hero_->lastHolesFreePosition = hero_->position;
    }
    const Culture rc = c.room->roomInfo->culture;
    if (!tiles_.count(rc)) Session::LoadTiles(data_, rc, level_, tiles_[rc]);
    std::vector<std::string> missing;
    room_ = render::RoomDraw(); // the last room's first
    room_ = render::BuildRoomDraw(*c.room, RoomPosition(c.x, c.y), level_, bank_, tiles_[rc], commonTiles_, &missing, [&](RoomTexture& t) {
        // Its GX texture now, then the RGBA goes.
        GxTexture g;
        uint32_t bytes = 0;
        g.texels = ToRgb5a3(t, bytes);
        if (g.texels) {
            GX_InitTexObj(&g.obj, g.texels, static_cast<u16>(t.width), static_cast<u16>(t.height), GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GX_InitTexObjFilterMode(&g.obj, GX_NEAR, GX_NEAR);
            roomTextures_[&t] = g;
        }
        std::vector<uint8_t>().swap(t.rgba);
    });
    GX_InvalidateTexAll();
    char line[256];
    std::snprintf(line, sizeof line, "%s floor %d (seed %d)  room %d/%zu: %s  %zu objects%s", kCultureNames[culture_], level_ + 1, seed_, cell + 1,
                  floor_->dungeon->cells.size(), c.info->name.c_str(), room_.quads.size(), missing.empty() ? "" : "  (sprites missing)");
    status_ = line;
    LogDebug("mlgc: %s", line);
    TOUR_REPORT("mlgc: %s\n", line);
}

const MoonGame::GxTexture* MoonGame::SpriteTexture(const render::Sprite& s)
{
    auto it = spriteTextures_.find(&s);
    if (it != spriteTextures_.end()) return &it->second;
    std::string texels;
    try {
        texels = data_.Read(bank_.TexelsPath(s));
    } catch (const std::exception&) {
        return nullptr;
    }
    if (texels.size() < render::SpriteBank::TexelBytes(s)) return nullptr;
    GxTexture g;
    g.texels = memalign(32, texels.size());
    if (!g.texels) return nullptr;
    std::memcpy(g.texels, texels.data(), texels.size());
    DCFlushRange(g.texels, texels.size());
    if (s.format == render::TexFormat::CI8) {
        size_t palette = static_cast<size_t>((s.width + 7) / 8 * 8) * static_cast<size_t>((s.height + 3) / 4 * 4);
        GX_InitTlutObj(&g.tlut, static_cast<u8*>(g.texels) + palette, GX_TL_RGB5A3, 256);
        GX_InitTexObjCI(&g.obj, g.texels, static_cast<u16>(s.width), static_cast<u16>(s.height), GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
        g.paletted = true;
    } else {
        GX_InitTexObj(&g.obj, g.texels, static_cast<u16>(s.width), static_cast<u16>(s.height), GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    GX_InitTexObjFilterMode(&g.obj, GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
    return &(spriteTextures_[&s] = g);
}


// Free memory: the heap's free blocks and what the arena hasn't handed out yet.
[[maybe_unused]] static unsigned FreeKb()
{
    const struct mallinfo info = mallinfo();
    return (unsigned(info.fordblks) + unsigned((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())) / 1024;
}

void MoonGame::Update(float deltaTime)
{
    if (!floor_) return;
    try {
#ifdef MLGC_AUTOTOUR
        // Test builds (AUTOTOUR=N): every N frames the next room; after the floor's last, the
        // next floor; after the third, the next dungeon. The log has the free memory per floor.
        static int frames = 0;
        if (++frames % MLGC_AUTOTOUR == 0) {
            if (cell_ + 1 < static_cast<int>(floor_->dungeon->cells.size())) {
                EnterRoom(cell_ + 1);
            } else {
                level_ = (level_ + 1) % 3;
                if (level_ == 0) {
                    culture_ = culture_ % 4 + 1;
                    session_.Start(++seed_);
                }
                NewFloor();
                TOUR_REPORT("mlgc: autotour %s floor %d, %u KB free\n", kCultureNames[culture_], level_ + 1, FreeKb());
            }
        }
        return;
#endif
        ReadPad();
        // Debug shortcuts while START is held (START alone is the game's pause).
        if (pad_.debug) {
            if (padPressed_ & PAD_BUTTON_A) {
                level_ = (level_ + 1) % 3;
                if (level_ == 0) session_.Start(++seed_);
                NewFloor();
            } else if (padPressed_ & (PAD_TRIGGER_R | PAD_TRIGGER_L)) {
                culture_ = (padPressed_ & PAD_TRIGGER_R) ? culture_ % 4 + 1 : (culture_ + 2) % 4 + 1;
                level_ = 0;
                session_.Start(seed_);
                NewFloor();
            } else if (padPressed_ & PAD_BUTTON_Y) {
                level_ = 0;
                session_.Start(++seed_);
                NewFloor();
            } else if (padPressed_ & PAD_TRIGGER_Z) {
                widescreen_ = !widescreen_;
            }
            return;
        }
        UpdateHero(deltaTime);
    } catch (const std::exception& e) {
        LogError("mlgc: %s", e.what());
        TOUR_REPORT("mlgc: error %s\n", e.what());
        status_ = std::string("error: ") + e.what();
    }
}

// The pads, as PAD_ScanPads would read them (CCGC's reader; Octave's input never resets a
// port): a port with no controller is reset, or one plugged in late is never seen; a read
// that failed on the way keeps the reading before.
static void ReadPadStatus(PADStatus (&pads)[PAD_CHANMAX])
{
    static PADStatus last[PAD_CHANMAX] = {};
    PAD_Read(pads);
    uint32_t reset = 0;
    for (int i = 0; i < PAD_CHANMAX; i++) {
        if (pads[i].err == PAD_ERR_NO_CONTROLLER) reset |= PAD_CHAN0_BIT >> i;
        else if (pads[i].err == PAD_ERR_TRANSFER || pads[i].err == PAD_ERR_NOT_READY) pads[i] = last[i];
        last[i] = pads[i];
    }
    if (reset) PAD_Reset(reset);
}

// The pad as the game's actions (InputActionsManager's bindings, on a GameCube pad):
//   stick or D-pad: LeftStick (InControl's circular dead zone from 0.2, full at 0.9)
//   A Action1 (attack)    B Action2 (hold: pendant)    X Action3 (secondary)    Y Action4 (inventory)
//   R LeftTrigger (roll)  L RightTrigger (potion)      Z the bumpers (swap weapons)
//   START Start (pause)   C-stick Select (map)
// Holding START makes the other buttons debug shortcuts instead (pad_.debug).
void MoonGame::ReadPad()
{
    PADStatus pads[PAD_CHANMAX];
    ReadPadStatus(pads);
    const PADStatus& p = pads[0];
    const uint16_t held = p.err == PAD_ERR_NONE ? p.button : 0;
    padPressed_ = held & static_cast<uint16_t>(~padHeld_);
    padHeld_ = held;
    // A GameCube stick reaches about 100 of its 127.
    float x = p.err == PAD_ERR_NONE ? p.stickX / 100.0f : 0.0f, y = p.err == PAD_ERR_NONE ? p.stickY / 100.0f : 0.0f;
    float mag = std::sqrt(x * x + y * y);
    if (mag < 0.2f) {
        x = y = 0.0f;
    } else {
        float k = std::min((mag - 0.2f) / 0.7f, 1.0f) / mag;
        x *= k;
        y *= k;
    }
    // The game binds the D-pad to the stick's directions too.
    if (held & PAD_BUTTON_LEFT) x = -1.0f;
    if (held & PAD_BUTTON_RIGHT) x = 1.0f;
    if (held & PAD_BUTTON_UP) y = 1.0f;
    if (held & PAD_BUTTON_DOWN) y = -1.0f;
    pad_.stick = {x, y};
    const bool cstick = p.err == PAD_ERR_NONE && (std::abs(p.substickX) > 50 || std::abs(p.substickY) > 50);
    pad_.select = cstick && !cstickHeld_;
    cstickHeld_ = cstick;
    pad_.action1 = padPressed_ & PAD_BUTTON_A;
    pad_.action2Held = held & PAD_BUTTON_B;
    pad_.action3 = padPressed_ & PAD_BUTTON_X;
    pad_.action3Held = held & PAD_BUTTON_X;
    pad_.action4 = padPressed_ & PAD_BUTTON_Y;
    pad_.leftTrigger = padPressed_ & PAD_TRIGGER_R;
    pad_.rightTrigger = padPressed_ & PAD_TRIGGER_L;
    pad_.bumper = padPressed_ & PAD_TRIGGER_Z;
#ifdef MLGC_PADLOG
    static int padFrames = 0;
    if (++padFrames % 60 == 0)
        TOUR_REPORT("mlgc: pad err %d stick %d %d -> %.3f %.3f buttons %04x hero %.2f %.2f\n", p.err, p.stickX, p.stickY, x, y, held,
                    hero_ ? hero_->position.x : 0.0f, hero_ ? hero_->position.y : 0.0f);
#endif
    // START: a pause on release if nothing else was pressed while it was held.
    pad_.start = false;
    if (held & PAD_BUTTON_START) {
        if (padPressed_ & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R))
            startCombo_ = true;
        pad_.debug = true;
    } else {
        if (pad_.debug && !startCombo_) pad_.start = true;
        pad_.debug = false;
        startCombo_ = false;
    }
}

// The controller's frame from the pad, and the door he pushed through.
void MoonGame::UpdateHero(float deltaTime)
{
    if (!hero_) return;
    float x = pad_.stick.x, y = pad_.stick.y;
#ifdef MLGC_AUTOWALK
    // Test builds: right (from the middle: through the east door if there's one), up, left,
    // down, a diagonal, idle; again and again.
    static int frame = 0;
    static const int kScript[][3] = {{240, 1, 0}, {70, 0, 1}, {160, -1, 0}, {140, 0, -1}, {60, 1, 1}, {30, 0, 0}};
    int f = frame++ % 700;
    for (const auto& seg : kScript) {
        if (f < seg[0]) {
            x = static_cast<float>(seg[1]) * (seg[2] ? 0.7071068f : 1.0f);
            y = static_cast<float>(seg[2]) * (seg[1] ? 0.7071068f : 1.0f);
            break;
        }
        f -= seg[0];
    }
    if (frame % 30 == 0) {
        unity::AnimatorStateInfo info = hero_->animator.GetCurrentAnimatorStateInfo();
        const std::string* sprite = hero_->animator.Sprite();
        TOUR_REPORT("mlgc: walk %d room %d pos %.2f %.2f state %s t %.2f sprite %s\n", frame, cell_, hero_->position.x, hero_->position.y,
                    heroAnimator_.states[static_cast<size_t>(hero_->animator.CurrentState())].name.c_str(), info.normalizedTime,
                    sprite ? sprite->c_str() : "-");
    }
#endif
    HeroMerchantController::Input input;
    input.stick = {x, y};
    input.roll = pad_.leftTrigger;
    input.attack = pad_.action1;
#ifdef MLGC_AUTOWALK
    input.roll = frame % 97 == 0;   // and a roll now and then,
    input.attack = frame % 41 == 0; // and an attack
#endif
    hero_->Update(deltaTime, input);
    if (!hero_->doorEntered) return;
    // DungeonDoor.OnPlayerEnter: into the next room, in by its opposite door.
    const std::string& door = hero_->doorEntered->path;
    static const char* const kDoors[4] = {"LevelDoorUp", "LevelDoorDown", "LevelDoorLeft", "LevelDoorRight"};
    static const int kStep[4][2] = {{0, 1}, {0, -1}, {-1, 0}, {1, 0}}, kOpposite[4] = {1, 0, 3, 2};
    const PlacedCell& c = floor_->dungeon->cells[static_cast<size_t>(cell_)];
    for (int d = 0; d < 4; d++) {
        if (door.find(kDoors[d]) == std::string::npos) continue;
        for (size_t i = 0; i < floor_->dungeon->cells.size(); i++) {
            const PlacedCell& n = floor_->dungeon->cells[i];
            if (n.x == c.x + kStep[d][0] && n.y == c.y + kStep[d][1]) {
                TOUR_REPORT("mlgc: through %s into room %d\n", door.c_str(), static_cast<int>(i));
                const DoorState& used = c.room->doors.doors[d];
                EnterRoom(static_cast<int>(i), kOpposite[d], used.hasArrival ? &used.arrival : nullptr);
                return;
            }
        }
    }
}

// Will's sprite where he stands, ordered like SpriteDepthOrderRender orders it.
bool MoonGame::HeroQuad(render::DrawQuad& q) const
{
    if (!hero_) return false;
    const std::string* name = hero_->animator.Sprite();
    const render::Sprite* s = name ? bank_.Find(*name) : nullptr;
    if (!s) return false;
    const float x = hero_->position.x, y = hero_->position.y;
    const float x0 = s->left, y0 = s->bottom;
    const float x1 = x0 + static_cast<float>(s->width) * s->unitsPerPixel, y1 = y0 + static_cast<float>(s->height) * s->unitsPerPixel;
    q.sprite = s;
    q.x[0] = x + x0, q.y[0] = y + y0;
    q.x[1] = x + x1, q.y[1] = y + y0;
    q.x[2] = x + x1, q.y[2] = y + y1;
    q.x[3] = x + x0, q.y[3] = y + y1;
    q.layer = defaultLayer_;
    q.order = 15000 + static_cast<int>((y + heroDepthOffset_) * 10.0f) * -1;
    q.sortY = y;
    return true;
}

// The weapon's MainAttack/Art over him: at his origin (its prefab sits there), its
// SpriteDepthOrderRender set over his (his order + 1 + overOffset 0), while the clips show it.
bool MoonGame::WeaponQuad(const render::DrawQuad& hero, render::DrawQuad& q) const
{
    if (!weapon_ || !weapon_->artEnabled) return false;
    const std::string* name = weapon_->Sprite();
    const render::Sprite* s = name && !name->empty() ? bank_.Find(*name) : nullptr;
    if (!s) return false;
    const float x = hero_->position.x, y = hero_->position.y;
    const float x0 = s->left, y0 = s->bottom;
    const float x1 = x0 + static_cast<float>(s->width) * s->unitsPerPixel, y1 = y0 + static_cast<float>(s->height) * s->unitsPerPixel;
    q.sprite = s;
    q.x[0] = x + x0, q.y[0] = y + y0;
    q.x[1] = x + x1, q.y[1] = y + y0;
    q.x[2] = x + x1, q.y[2] = y + y1;
    q.x[3] = x + x0, q.y[3] = y + y1;
    q.layer = hero.layer;
    q.order = hero.order + 1;
    q.sortY = hero.sortY;
    return true;
}

void MoonGame::DrawQuad(const render::DrawQuad& q, const GxTexture& tex)
{
    if (tex.paletted) GX_LoadTlut(const_cast<GXTlutObj*>(&tex.tlut), GX_TLUT0);
    GX_LoadTexObj(const_cast<GXTexObj*>(&tex.obj), GX_TEXMAP0);
    const u8 r = static_cast<u8>(q.color[0] * 255.0f), g = static_cast<u8>(q.color[1] * 255.0f), b = static_cast<u8>(q.color[2] * 255.0f),
             a = static_cast<u8>(q.color[3] * 255.0f);
    // Corners: image lower-left, lower-right, upper-right, upper-left (texture rows from the top).
    static const float u[4] = {0, 1, 1, 0}, v[4] = {1, 1, 0, 0};
    GX_Begin(GX_QUADS, kFormat, 4);
    for (int i = 0; i < 4; i++) {
        GX_Position2f32(q.x[i], q.y[i]);
        GX_Color4u8(r, g, b, a);
        GX_TexCoord2f32(u[i], v[i]);
    }
    GX_End();
}

void MoonGame::Render(float width, float height)
{
    if (!floor_) return;
    const PlacedCell& c = floor_->dungeon->cells[static_cast<size_t>(cell_)];
    const unity::Vector2 at = RoomPosition(c.x, c.y);

    // The game's view: 640 x 360 units around the room. On a 4:3 picture it's letterboxed (a
    // unit a pixel, black bars); widescreen squeezes it over the whole picture for a 16:9 TV
    // to stretch back (anamorphic).
    float top = 0.0f, viewHeight = height;
    if (!widescreen_) {
        viewHeight = width * 9.0f / 16.0f;
        if (viewHeight > height) viewHeight = height;
        top = static_cast<float>(static_cast<int>((height - viewHeight) / 2.0f) & ~1);
    }
    GX_SetViewport(0.0f, top, width, viewHeight, 0, 1);
    GX_SetScissor(0, static_cast<u32>(top), static_cast<u32>(width), static_cast<u32>(viewHeight));
    Mtx44 projection;
    guOrtho(projection, at.y + 180.0f, at.y - 180.0f, at.x - 320.0f, at.x + 320.0f, 0.0f, 1.0f);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    Mtx identity;
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(kFormat, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(kFormat, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(kFormat, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, GXColor{0, 0, 0, 0});
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    // The room's quads in their order, Will (and his weapon just over him) slotted in where the
    // same order puts him.
    render::DrawQuad heroQuad, weaponQuad;
    bool heroPending = HeroQuad(heroQuad);
    const bool weaponShown = heroPending && WeaponQuad(heroQuad, weaponQuad);
    auto drawHero = [&]() {
        if (const GxTexture* tex = SpriteTexture(*heroQuad.sprite)) DrawQuad(heroQuad, *tex);
        if (weaponShown)
            if (const GxTexture* tex = SpriteTexture(*weaponQuad.sprite)) DrawQuad(weaponQuad, *tex);
        heroPending = false;
    };
    for (const render::DrawQuad& q : room_.quads) {
        if (heroPending && (heroQuad.layer != q.layer ? heroQuad.layer < q.layer
                                                      : heroQuad.order != q.order ? heroQuad.order < q.order : heroQuad.sortY > q.sortY))
            drawHero();
        const GxTexture* tex = nullptr;
        if (q.sprite) tex = SpriteTexture(*q.sprite);
        else {
            auto it = roomTextures_.find(q.texture);
            if (it != roomTextures_.end()) tex = &it->second;
        }
        if (tex) DrawQuad(q, *tex);
    }
    if (heroPending) drawHero();
}
