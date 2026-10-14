// The room's enemies on the GameCube: the ones the port has (BabySlime, GolemSoldier, Tangle) run as the game runs
// them (game/Enemy.h's frame: the fixed steps with their bodies and attack triggers, DOTween,
// the behaviours' Updates, the delayed calls, PolyNav, the Animators, the agents' moves),
// against Will: their touch hurts him (DealDamageToPlayer) and his weapon's hitbox hurts them.
//
// Will's hits: a swing hits each enemy its hitbox overlaps once (the hitbox's activation per
// swing is WeaponCollisionDetection's new contact), with the weapon's strength plus his, doubled
// on the third of a combo, and knocks it back; Update returns how many it hit (the caller's
// hit-stop). Not yet: the damage numbers, drops.
//
// Their sounds: what the port plays itself (MasterAudio.PlaySound in their behaviours, their
// EventSounds) through `audio`, the port's MasterAudio; the hit sound (Enemy.DealDamage: the
// behaviour's soundWhenHit) and the death sound (EnemyBehaviour.SpawnDeathParticles:
// Enemy.PlayDeathSound, its enemyDeathSound or "enemy_death"), which the port doesn't play,
// through `playSound` (GameSound::Play).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/Enemy.h"
#include "game/HeroMerchantController.h"
#include "game/RoomFill.h"
#include "game/RoomPolyNav.h"
#include "unity/DelayedCalls.h"
#include "unity/Physics2D.h"
#include "unity/PhysicsWorld.h"
#include "unity/Tween.h"
#include "util/Json.h"

class RoomEnemies {
public:
    // The enemies the port runs (the Golem dungeon's so far), their prefabs' names.
    static constexpr const char* kPorted[3] = {"BabySlime", "GolemSoldier", "Tangle"};
    struct Data {
        const unity::AnimatorController* controllers[3] = {}; // animators/<name>.json, by kPorted
        const json::Value* enemyStats = nullptr;               // enemy_stats.json
        const unity::Physics2DSettings* physics = nullptr;      // physics2d.json
        game::MasterAudio* audio = nullptr;                     // the port's MasterAudio
        std::function<void(const std::string& group)> playSound;
    };
    // Will's weapon hits: its strength (5 + his items') and the weapon's knockback.
    struct Attack {
        float strength = 5.0f;
        float pushForce = 450.0f; // mainAttackPushBackwardsForce
    };

    // An enemy prefab this runs (else the room's static sprite stands for it).
    static int Ported(const std::string& prefab)
    {
        for (int i = 0; i < 3; i++)
            if (prefab == kPorted[i]) return i;
        return -1;
    }
    static bool Runs(const std::string& prefab) { return Ported(prefab) >= 0; }

    // `dead`: the spawn tiles of this room's enemies already killed (they stay dead). As the
    // room is entered (DungeonRoom.EnterRoom): the enemies appear after enemiesAppearDelay
    // (EnableEnemies) and go for Will a second after that.
    RoomEnemies(const Data& data, const game::RoomFill& room, unity::Vector2 roomPosition, const unity::Physics2D& roomColliders,
                game::HeroMerchantController& hero, const Attack& attack, int level, int dungeonCulture, const std::vector<int>& dead);
    ~RoomEnemies();
    RoomEnemies(const RoomEnemies&) = delete;
    RoomEnemies& operator=(const RoomEnemies&) = delete;

    bool Empty() const { return enemies_.empty(); }
    size_t Count() const { return enemies_.size(); }
    // The spawn tiles of the enemies killed (to keep them dead), and whether any is left.
    std::vector<int> DeadTiles() const;
    bool AllDefeated() const;
    const std::vector<std::unique_ptr<game::Enemy>>& Enemies() const { return enemies_; }
    int Update(float deltaTime);

    // What to draw: each enemy still there, and the dead sprites left behind.
    struct Shown {
        const std::string* sprite;
        unity::Vector2 position; // the sprite's object's
        int order;                // SpriteDepthOrderRender's
        float flash;              // _FlashAmount
        float color[4];           // _Color
    };
    std::vector<Shown> Visible() const;

private:
    game::HeroMerchantController& hero_;
    Attack attack_;
    unity::PhysicsWorld world_;
    std::unique_ptr<game::EnemyPhysics> physics_;
    std::unique_ptr<game::RoomPolyNav> nav_;
    unity::DelayedCalls calls_;
    unity::Tweens tweens_;
    game::EnemyHero heroView_;
    game::EnemyScene scene_;
    std::vector<std::unique_ptr<game::Enemy>> enemies_;
    std::vector<int> tiles_;       // per enemy: its spawn tile
    std::vector<int> depthOffset_; // per enemy: its art's SpriteDepthOrderRender.offset
    std::vector<std::string> hitSound_, deathSound_; // per enemy: its prefab's
    std::function<void(const std::string&)> playSound_;
    const std::string& SoundOf(const game::Enemy& e, const std::vector<std::string>& sounds) const;
    std::vector<unity::Vector2> artOffset_;
    std::vector<std::pair<std::string, unity::Vector2>> deadSprites_;
    std::vector<const game::Enemy*> hitThisSwing_;
    unity::Rigidbody2D* heroBody_ = nullptr;
    double time_ = 0.0, fixedTime_ = 0.0;
};
