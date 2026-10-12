// The room's enemies on the GameCube: the ones the port has (BabySlimes) run as the game runs
// them (game/Enemy.h's frame: the fixed steps with their bodies and attack triggers, DOTween,
// the behaviours' Updates, the delayed calls, PolyNav, the Animators, the agents' moves),
// against Will: their touch hurts him (DealDamageToPlayer) and his weapon's hitbox hurts them.
//
// Will's hits: a swing hits each enemy its hitbox overlaps once (the hitbox's activation per
// swing is WeaponCollisionDetection's new contact), with the weapon's strength plus his, doubled
// on the third of a combo, and knocks it back; Update returns how many it hit (the caller's
// hit-stop). Not yet: the damage numbers, drops.
#pragma once

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
    struct Data {
        const unity::AnimatorController* babySlime = nullptr; // animators/BabySlime.json
        const json::Value* enemyStats = nullptr;               // enemy_stats.json
        const unity::Physics2DSettings* physics = nullptr;      // physics2d.json
    };
    // Will's weapon hits: its strength (5 + his items') and the weapon's knockback.
    struct Attack {
        float strength = 5.0f;
        float pushForce = 450.0f; // mainAttackPushBackwardsForce
    };

    // An enemy prefab this runs (else the room's static sprite stands for it).
    static bool Runs(const std::string& prefab) { return prefab == "BabySlime"; }

    RoomEnemies(const Data& data, const game::RoomFill& room, unity::Vector2 roomPosition, const unity::Physics2D& roomColliders,
                game::HeroMerchantController& hero, const Attack& attack, int level, int dungeonCulture);
    ~RoomEnemies();
    RoomEnemies(const RoomEnemies&) = delete;
    RoomEnemies& operator=(const RoomEnemies&) = delete;

    bool Empty() const { return enemies_.empty(); }
    size_t Count() const { return enemies_.size(); }
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
    std::vector<int> depthOffset_; // per enemy: its art's SpriteDepthOrderRender.offset
    std::vector<unity::Vector2> artOffset_;
    std::vector<std::pair<std::string, unity::Vector2>> deadSprites_;
    std::vector<const game::Enemy*> hitThisSwing_;
    unity::Rigidbody2D* heroBody_ = nullptr;
    double time_ = 0.0, fixedTime_ = 0.0;
};
