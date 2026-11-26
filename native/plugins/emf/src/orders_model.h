// SPDX-License-Identifier: MIT
// EMF's v39 enemy orders and v40 move orders on Kiln (0.2.0-dev.31.13), without the game: api_orders.cpp gives it
// kiln.creature_control, kiln.creatures, kiln.spawn and kiln.world through Deps; tests/orders_test.cpp runs it against
// fakes of those services.
//
// The semantics are EMF's (sdk/include/emf/emf_api.h v39/v40; EMF's own src/core/enemy_orders.cpp and move_orders.cpp,
// upstream 0.6.9, read for the rules): the same checks and `why` texts, the same ends and statuses, the same limits.
// What Kiln does differently is how an order reaches the creature:
//  - every order and move holds a lease of kiln.creature_control, the one broker of a controlled creature body: an
//    enemy order an AI-on PetCombat lease, a move an AI-on EngineGuard lease (the game's own guard walks it). Nothing is
//    written or sent before the lease is Active and never while it is not (preempted by a ride, say); a lease the broker
//    ends ends the order. The broker's refusal is the order's `why` ("creature control: <its reason>").
//  - an enemy order is kiln.creatures' target hold (target_prefer: the pick, SelectedTargets slot 0, held with the
//    game's next target change at "never", re-asserted every Pre) and the game's AddThreatTargetEvent (kiln.creatures
//    command ATTACK), which puts an enemy at rest in Combat (docs/re/creatures.md §11 [rt]). EMF writes the pick, an
//    alarmed aggro entry and the Combat state itself.
//  - a move is the game's guard order (StartGuardingEvent, kiln.creatures command GUARD) on a hidden waypoint spawned
//    through kiln.spawn at each point, as on EMF; stop and the guard given back are StopGuardingEvent / GUARD, the home
//    of a creature that guarded nobody is kiln.creatures' home field (written without a layer: it lives there).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace kemf::orders {

constexpr size_t kOrdersMax = 256;
constexpr size_t kMovesMax = 256;
constexpr size_t kPointsMax = 16;
constexpr int32_t kDamageMax = 100000;
constexpr uint64_t kThreatResendMs = 3000;  // an ordered enemy out of its fight gets the threat again this often
constexpr float kOrderThreat = 100.0f;      // EMF's order aggro
constexpr uint64_t kResendMs = 3000;        // a guard order sent again (EMF kMoveResendMs)
constexpr double kStuckSeconds = 15.0;
constexpr double kProgress = 1.0;           // metres closer that count as progress
constexpr double kUpDown = 2.0;             // a point is reached within arrive + this up or down
constexpr double kGuardSlack = 1.0;         // a guard stops its guard distance short of its waypoint: + this
constexpr double kGuardMax = 8.0;           // a larger guard distance is not believed
// The game draws the guard distance from guardSetup.targetDistance times a random factor when Guard is entered
// (docs/re/creatures.md §12: 2.5-3.6 m for a Vukah's 3 m): the upper end is used, the dynamic value is not read.
constexpr double kGuardFactor = 1.2;
constexpr double kGuardFallback = 3.0;      // guardSetup.targetDistance when the template's value cannot be read
constexpr uint32_t kReplaceMax = 3;         // a vanished waypoint is placed again this often, then the move is stuck
// enemy01::State (sigdb/types; docs/re/creatures.md): the fight states, and Guard
enum : uint8_t {
    kCombat = 6, kFalling = 18, kHit = 19, kGuard = 20, kLanding = 22, kBlockProjectile = 23, kControlledByAttack = 14,
    kCircling = 26, kStunned = 29,
};
constexpr uint8_t kPlayerAlly = 11;  // keen::FactionType PlayerAlly

using Vec = std::array<double, 3>;

struct Creature {
    bool ai = false;    // it has the enemy AI (DynamicEnemy01BehaviorState)
    bool dead = false;  // its Actor is dead, or its health is known and 0
    bool boss = false;  // its template has Boss or BossHealthBar
    uint8_t faction = 255, state = 0;
    uint32_t target = 0, guard = 0;  // SelectedTargets slot 0, guardEntityId
};

enum class LeaseState { Pending, Active, Preempted, Ended };
struct LeaseView {
    LeaseState state = LeaseState::Ended;
    uint64_t sequence = 0;     // the transition it became Active with (a new one after a resume)
    bool body_gone = false;    // it ended because the body went (BodyGone, Retired)
    bool world_ended = false;  // ... because the world ended
    std::string reason;
};

struct Deps {
    std::function<uint64_t()> now_ms;
    std::function<bool(uint32_t entity, Creature* out)> creature;  // false: no such entity (or not readable now)
    std::function<bool(uint32_t entity)> player;
    std::function<bool(uint32_t entity, Vec* out)> position;  // false: no such entity, or it has no position
    // kiln.creature_control: a lease (combat: PetCombat, else EngineGuard) on the creature; 0 and *why when refused
    std::function<uint64_t(uint32_t entity, bool combat, std::string* why)> lease;
    std::function<LeaseView(uint64_t lease)> lease_view;  // Active only while the broker says the lease may drive
    std::function<void(uint64_t lease)> release;
    // kiln.creatures
    std::function<void(uint32_t enemy, uint32_t target, double seconds)> prefer;  // target 0: let go
    std::function<void(uint32_t enemy, uint32_t target)> threat;
    std::function<void(uint32_t enemy, uint32_t guarded)> guard;
    std::function<void(uint32_t enemy, uint32_t guarded)> stop_guard;
    std::function<void(uint32_t enemy, const Vec& home)> set_home;
    std::function<double(uint32_t enemy)> guard_target_distance;  // guardSetup.targetDistance, <= 0 unknown
    // kiln.spawn: the hidden waypoint (0 when it could not be spawned), and its removal
    std::function<uint32_t(const Vec& at, const void* tag)> spawn_waypoint;
    std::function<void(uint32_t entity)> destroy;
    std::function<bool(const void* tag)> tag_alive;  // the mod that gave the order still runs
};

struct OrderRequest {
    uint32_t enemy = 0, target = 0, flags = 0;
    double seconds = 0;
    int32_t damage = 0;
    const void* tag = nullptr;  // the calling mod
};

struct MoveRequest {
    uint32_t enemy = 0, flags = 0;
    std::vector<Vec> points;
    double arrive = 2, seconds = 0, stuck_seconds = kStuckSeconds;
    const void* tag = nullptr;
};

// id, EMF_MOVE_* status, world_closed (the world closes: the callback gets world NULL)
using MoveDone = std::function<void(uint32_t id, uint32_t status, bool world_closed)>;

class Orders {
public:
    explicit Orders(Deps d) : d_(std::move(d)) {}

    // An enemy order (checked as EMF checks it); its id (from 1, never reused), or 0 and *why. A move on the enemy ends
    // (cancelled) once the order holds.
    uint32_t order(const OrderRequest& r, std::string* why);
    // false: no such order. now: at once (the server world's thread, inside a world callback), else at the next step.
    bool order_cancel(uint32_t id, bool now);
    // A move's flags, then its numbers and points, before any creature is read ("" = fine): enemy_move's first checks.
    static std::string check_flags(uint32_t flags);
    static std::string check_move(const MoveRequest& r);
    // A move; its id (from 1, never reused), or 0 and *why. A move or enemy order already on the creature ends
    // (cancelled) once it holds; `done` (may be empty) is called once when it ends, after it is gone.
    uint32_t move(const MoveRequest& r, MoveDone done, std::string* why);
    bool move_cancel(uint32_t id, bool now);
    uint32_t moving(uint32_t enemy) const;  // its move's id, 0 none (any thread)
    void step();                            // after a server step, on its thread
    void world_reset(bool callbacks);       // the world closes: every move ends cancelled (world closed), every order ends
    size_t order_count() const;
    size_t move_count() const;

private:
    struct Order {
        uint32_t id = 0, enemy = 0, target = 0, flags = 0;
        const void* tag = nullptr;
        uint64_t lease = 0, asserted = 0;  // the lease sequence the hold was written under (0: not written now)
        uint64_t until = 0, last_threat = 0;
        bool cancelled = false;
        bool renewed = false;  // took over the lease of the order it replaced: its target is written at the next step
    };
    struct Move {
        uint32_t id = 0, enemy = 0, flags = 0, waypoint = 0, old_waypoint = 0, prior_guard = 0, replaced = 0;
        const void* tag = nullptr;
        uint64_t lease = 0, guard_seq = 0;  // the lease sequence the guard order was sent under
        Vec waypoint_at{};
        std::vector<Vec> points;
        size_t next = 0;
        double arrive = 2, stuck_ms = kStuckSeconds * 1000, best = 0, guard_distance = -1;
        uint64_t until = 0, last_guard = 0, since = 0, last_place = 0;
        bool cancelled = false;
        MoveDone done;
    };
    struct Done {
        MoveDone cb;
        uint32_t id, status;
        bool closed;
    };
    bool alive(uint32_t entity) const;  // an entity with a position, not dead
    void end_order(Order& o);           // the hold let go, the lease released
    int tick_order(Order& o, uint64_t now);  // 1: it ended
    int tick_move(Move& m, uint64_t now);    // its EMF_MOVE_* status when it ends now, else -1
    bool place_waypoint(Move& m, uint64_t now, bool fresh);
    bool waypoint_alive(const Move& m) const;
    double distance(uint32_t enemy, const Vec& p) const;
    void send_guard(Move& m, uint64_t now);
    void finish_move(Move& m, uint32_t status, bool may_write);
    bool cancel_move_locked(uint32_t enemy, std::vector<Done>* done);  // the move on it ends (cancelled)
    void cancel_order_locked(uint32_t enemy);                          // the order on it ends
    uint32_t moving_locked(uint32_t enemy) const;
    static void call(std::vector<Done>& done);

    Deps d_;
    mutable std::mutex m_;  // a cancel may come from another thread; callbacks run with it released
    std::vector<Order> orders_;
    std::vector<Move> moves_;
    uint32_t next_order_ = 1, next_move_ = 1;
};

}  // namespace kemf::orders
