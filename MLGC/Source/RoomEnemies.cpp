#include "RoomEnemies.h"

#include <algorithm>

#include "game/EnemyFactory.h"
#include "game/EnemyStats.h"
#include "game/Tangle.h"
#include "game/Prefabs.h"
#include "game/RoomScene.h"

using namespace game;
using unity::Vector2;

RoomEnemies::RoomEnemies(const Data& data, const RoomFill& room, Vector2 roomPosition, const unity::Physics2D& roomColliders,
                         HeroMerchantController& hero, const Attack& attack, int level, int dungeonCulture, const std::vector<int>& dead)
    : hero_(hero), attack_(attack), world_(*data.physics), playSound_(data.playSound)
{
    std::vector<std::pair<int, std::string>> spawns;
    for (const auto& s : room.enemies)
        if (Runs(s.second) && data.controllers[Ported(s.second)] && PrefabLibrary::Find(s.second, "enemies_golem") &&
            std::find(dead.begin(), dead.end(), s.first) == dead.end())
            spawns.push_back(s);
    if (spawns.empty()) return;

    // The room's colliders (the rocks on their own Kinematic bodies), Will's body, the map.
    world_.AddColliders(roomColliders, 1 << unity::kLayerRisenObstacle);
    physics_ = std::make_unique<EnemyPhysics>(world_);
    unity::Rigidbody2DDef heroDef = HeroMerchantController::Settings::HeroBody();
    heroBody_ = &world_.AddBody(heroDef, hero.position, hero.layer, {1.0f, -3.5f}, {13.0f, 13.0f});
    physics_->SetHero(heroBody_);
    nav_ = BuildRoomPolyNav(room, roomPosition);
    PolyNav2D::current = &nav_->nav;

    heroView_.position = hero.position;
    heroView_.dealDamage = [this](float strength, const std::string& source, Enemy&) { hero_.DealDamageToPlayer(strength, source.c_str()); };
    scene_.calls = &calls_;
    scene_.tweens = &tweens_;
    scene_.hero = &heroView_;
    scene_.spawnEnemyDeadSprite = [this](const std::string& sprite, Vector2 at) { deadSprites_.emplace_back(sprite, at); };
    scene_.audio = data.audio;
    // Enemy.DealDamage's hit sound (not for an effect's damage), Enemy.PlayDeathSound.
    scene_.onEnemyReceivesDamage = [this](Enemy& e, float, EnemyDamageReceivedType type) {
        const std::string& sound = SoundOf(e, hitSound_);
        if (type != EnemyDamageReceivedType::Effect && !sound.empty() && playSound_) playSound_(sound);
    };
    scene_.spawnDeathParticles = [this](Enemy& e) {
        const std::string& sound = SoundOf(e, deathSound_);
        if (playSound_) playSound_(sound.empty() ? std::string("enemy_death") : sound);
    };

    EnemyStatsContext context;
    context.level = level;
    context.dungeonCulture = dungeonCulture;
    // Enemy.enemyDeadSprite (the prefabs' field isn't extracted yet; their banks have the sprite).
    static const char* const kDeadSprite[3] = {"deadenemy_slimes", "deadenemy_soldier", "deadenemy_tangle"};
    for (const auto& [tile, name] : spawns) {
        const int kind = Ported(name);
        const Prefab* prefab = PrefabLibrary::Find(name, "enemies_golem");
        // The art: the SpriteRenderer's object (its place below the root) and its depth offset.
        Vector2 art;
        int depth = 0;
        std::string artPath;
        for (const PrefabNode& n : prefab->nodes)
            if (artPath.empty() && n.HasRenderer() && !n.sprite.empty()) artPath = n.path;
        for (const PrefabNode& n : prefab->nodes) {
            if (n.path.empty() || artPath.compare(0, n.path.size(), n.path) != 0) continue;
            if (n.path.size() != artPath.size() && artPath[n.path.size()] != '/') continue;
            art = art + Vector2{n.x, n.y}; // each object on the way down to it
            if (n.path == artPath)
                for (const Behaviour& b : n.behaviours)
                    if (b.cls == "SpriteDepthOrderRender") depth = static_cast<int>(b.Field("offset"));
        }
        // Its sounds: the behaviour's soundWhenHit, the Enemy's enemyDeathSound.
        std::string hit, death;
        for (const PrefabNode& n : prefab->nodes)
            for (const Behaviour& b : n.behaviours) {
                auto h = b.raw.find("soundWhenHit");
                if (h != b.raw.end() && h->second.IsString() && hit.empty()) hit = h->second.s;
                auto d = b.raw.find("enemyDeathSound");
                if (d != b.raw.end() && d->second.IsString() && death.empty()) death = d->second.s;
            }
        hitSound_.push_back(hit);
        deathSound_.push_back(death);
        EnemyStatModificator m;
        EnemyStatModificator::FromJson(*data.enemyStats, name, m);
        // Under its EnemySpawn (at the tile), the room's enemy container, the room.
        const Vector2 at = TranslateTileToPosition(tile, room.gridSlicesX);
        auto enemy = MakeEnemy(*prefab, scene_, data.controllers[kind], {at, {0.0f, 0.0f}, roomPosition});
        enemy->enemyDeadSprite = kDeadSprite[kind];
        enemy->Init(name, m, context);
        physics_->Add(*enemy);
        enemy->SetActive(false); // (DisableEnemies: until the room's EnableEnemies)
        enemies_.push_back(std::move(enemy));
        tiles_.push_back(tile);
        depthOffset_.push_back(depth);
        artOffset_.push_back(art);
    }
    // EnterRoom: Invoke("EnableEnemies", enemiesAppearDelay); each one SetActive(true) and
    // StartTargetingPlayerAfterTime(1).
    calls_.now = 0.0;
    calls_.Invoke(this, "EnableEnemies", 0.5f, [this] {
        for (auto& e : enemies_) {
            e->SetActive(true);
            e->enemyBehaviour->StartTargetingPlayerAfterTime(1.0f);
        }
    });
}

const std::string& RoomEnemies::SoundOf(const Enemy& e, const std::vector<std::string>& sounds) const
{
    static const std::string none;
    for (size_t i = 0; i < enemies_.size() && i < sounds.size(); i++)
        if (enemies_[i].get() == &e) return sounds[i];
    return none;
}

std::vector<int> RoomEnemies::DeadTiles() const
{
    std::vector<int> out;
    for (size_t i = 0; i < enemies_.size(); i++)
        if (enemies_[i]->destroyed || !enemies_[i]->enemyStats.isAlive()) out.push_back(tiles_[i]);
    return out;
}

bool RoomEnemies::AllDefeated() const
{
    for (const auto& e : enemies_)
        if (!e->destroyed && e->enemyStats.isAlive()) return false;
    return true;
}

RoomEnemies::~RoomEnemies()
{
    if (nav_ && PolyNav2D::current == &nav_->nav) PolyNav2D::current = nullptr;
}

int RoomEnemies::Update(float deltaTime)
{
    if (enemies_.empty()) return 0;
    PolyNav2D::current = &nav_->nav;
    heroView_.position = hero_.position;
    heroView_.isDead = hero_.stats.isDead;

    time_ += static_cast<double>(deltaTime);
    nav_->nav.BeginFrame();
    while (fixedTime_ + 0.01 <= time_) {
        fixedTime_ += 0.01;
        calls_.now = fixedTime_;
        world_.SetLayer(*heroBody_, hero_.layer);
        world_.SyncTransform(*heroBody_, hero_.position);
        heroBody_->SetVelocity({0.0f, 0.0f});
        physics_->Step(0.01f);
    }
    calls_.now = time_;
    tweens_.Update(deltaTime);
    for (auto& e : enemies_)
        if (!e->destroyed && e->active && e->enemyBehaviour->enabled) e->enemyBehaviour->Update(deltaTime);
    calls_.Update(time_);
    nav_->nav.ResumeCoroutines();
    for (auto& e : enemies_)
        if (!e->destroyed && e->active) e->UpdateAnimator(deltaTime);
    nav_->nav.LateUpdate();
    // Each enemy's LateUpdate (the Tangle's weave: before its agent's, as the game ran them),
    // then its agent's.
    for (auto& e : enemies_) {
        if (e->destroyed || !e->active) continue;
        if (auto* tangle = dynamic_cast<TangleBehaviour*>(e->enemyBehaviour.get()))
            if (tangle->enabled) tangle->LateUpdate(deltaTime);
        e->navAgent.LateUpdate(deltaTime);
    }

    // Will's swing: each enemy its hitbox overlaps, once a swing.
    const Weapon* w = hero_.weapon;
    if (!w || !w->hitboxActive) {
        hitThisSwing_.clear();
        return 0;
    }
    int hits = 0;
    const Vector2 c = hero_.position + w->hitboxPosition + w->hitboxOffset;
    const Vector2 h{w->hitboxSize.x * 0.5f, w->hitboxSize.y * 0.5f};
    for (auto& e : enemies_) {
        if (e->destroyed || !e->active || !e->enemyBehaviour->enabled) continue;
        if (std::find(hitThisSwing_.begin(), hitThisSwing_.end(), e.get()) != hitThisSwing_.end()) continue;
        const Vector2 ec = e->position() + e->colliderOffset;
        const Vector2 eh{e->colliderSize.x * 0.5f, e->colliderSize.y * 0.5f};
        if (std::abs(ec.x - c.x) > h.x + eh.x || std::abs(ec.y - c.y) > h.y + eh.y) continue;
        hitThisSwing_.push_back(e.get());
        // Weapon.OnMainAttackHit: the knockback, then the damage (doubled on the combo's last).
        e->PushBackwards(hero_.position, attack_.pushForce);
        e->DealDamageToEnemy(hero_.comboCounter == 3 ? attack_.strength * 2.0f : attack_.strength);
        hits++;
    }
    return hits;
}

std::vector<RoomEnemies::Shown> RoomEnemies::Visible() const
{
    std::vector<Shown> out;
    for (const auto& [sprite, at] : deadSprites_) out.push_back({&sprite, at, 15000 + static_cast<int>(at.y * 10.0f) * -1, 0.0f, {1, 1, 1, 1}});
    for (size_t i = 0; i < enemies_.size(); i++) {
        const Enemy& e = *enemies_[i];
        if (e.destroyed || !e.active || e.sprite.empty()) continue;
        const Vector2 at = e.position() + artOffset_[i];
        Shown s{&e.sprite, at, 15000 + static_cast<int>((at.y + static_cast<float>(depthOffset_[i])) * 10.0f) * -1, e.flashAmount, {}};
        for (int k = 0; k < 4; k++) s.color[k] = e.color[k];
        out.push_back(s);
    }
    return out;
}
