#include "MoonGame.h"

#include <malloc.h>
#include <ogc/system.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "Input/Input.h"
#include "Log.h"
#include "game/RoomScene.h"

using namespace game;

// A test build's line in Dolphin's log (OSReport). Only in AUTOTOUR builds, which run in
// Dolphin: on a console SYS_Report overwrites the RTC counter (Octave's System_Dolphin.cpp).
#ifdef MLGC_AUTOTOUR
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
                   {"sprites/golem/", "sprites/forest/", "sprites/desert/", "sprites/tech/"});
    }
    // Start in the floor's first room (the initial room).
    cell_ = 0;
    for (size_t i = 0; i < floor_->dungeon->cells.size(); i++)
        if (floor_->dungeon->cells[i].cave == floor_->dungeon->layout.initCaveRoom) cell_ = static_cast<int>(i);
    EnterRoom(cell_);
}

void MoonGame::EnterRoom(int cell)
{
    FreeRoomTextures();
    cell_ = cell;
    const PlacedCell& c = floor_->dungeon->cells[static_cast<size_t>(cell)];
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
static unsigned FreeKb()
{
    const struct mallinfo info = mallinfo();
    return (unsigned(info.fordblks) + unsigned((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())) / 1024;
}

void MoonGame::Update(float)
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
        const PlacedCell& c = floor_->dungeon->cells[static_cast<size_t>(cell_)];
        auto go = [&](int dx, int dy) {
            for (size_t i = 0; i < floor_->dungeon->cells.size(); i++) {
                const PlacedCell& n = floor_->dungeon->cells[i];
                if (n.x == c.x + dx && n.y == c.y + dy) {
                    EnterRoom(static_cast<int>(i));
                    return;
                }
            }
        };
        if (INP_IsGamepadButtonJustDown(GAMEPAD_RIGHT, 0)) go(1, 0);
        else if (INP_IsGamepadButtonJustDown(GAMEPAD_LEFT, 0)) go(-1, 0);
        else if (INP_IsGamepadButtonJustDown(GAMEPAD_UP, 0)) go(0, 1);
        else if (INP_IsGamepadButtonJustDown(GAMEPAD_DOWN, 0)) go(0, -1);
        else if (INP_IsGamepadButtonJustDown(GAMEPAD_A, 0)) {
            level_ = (level_ + 1) % 3;
            if (level_ == 0) session_.Start(++seed_);
            NewFloor();
        } else if (INP_IsGamepadButtonJustDown(GAMEPAD_R1, 0) || INP_IsGamepadButtonJustDown(GAMEPAD_L1, 0)) {
            culture_ = INP_IsGamepadButtonJustDown(GAMEPAD_R1, 0) ? culture_ % 4 + 1 : (culture_ + 2) % 4 + 1;
            level_ = 0;
            session_.Start(seed_);
            NewFloor();
        } else if (INP_IsGamepadButtonJustDown(GAMEPAD_Z, 0)) {
            widescreen_ = !widescreen_;
        } else if (INP_IsGamepadButtonJustDown(GAMEPAD_START, 0)) {
            level_ = 0;
            session_.Start(++seed_);
            NewFloor();
        }
    } catch (const std::exception& e) {
        LogError("mlgc: %s", e.what());
        TOUR_REPORT("mlgc: error %s\n", e.what());
        status_ = std::string("error: ") + e.what();
    }
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

    for (const render::DrawQuad& q : room_.quads) {
        const GxTexture* tex = nullptr;
        if (q.sprite) tex = SpriteTexture(*q.sprite);
        else {
            auto it = roomTextures_.find(q.texture);
            if (it != roomTextures_.end()) tex = &it->second;
        }
        if (tex) DrawQuad(q, *tex);
    }
}
