// SPDX-License-Identifier: MIT
// EMF v39 enemy orders and v40 move orders on Kiln, without the game (orders_model.h).
#include "orders_model.h"

#include <algorithm>
#include <cmath>

#include "emf/emf_api.h"

namespace kemf::orders {

namespace {
bool fighting(const Creature& c) {
    if (!c.target) return false;
    switch (c.state) {  // a fight passes through these (an attack, a hit, circling, a block, a stun, a fall)
        case kCombat: case kControlledByAttack: case kHit: case kCircling: case kBlockProjectile: case kStunned:
        case kFalling: case kLanding: return true;
        default: return false;
    }
}
bool is_ally(const Creature& c) { return c.faction == kPlayerAlly; }
}  // namespace

bool Orders::alive(uint32_t entity) const {
    Vec at{};
    if (!entity || !d_.position(entity, &at)) return false;
    Creature c;
    return !(d_.creature(entity, &c) && c.dead);
}

void Orders::call(std::vector<Done>& done) {
    for (Done& x : done)
        if (x.cb) x.cb(x.id, x.status, x.closed);
    done.clear();
}

// ------------------------------------------------------------------------------------------------ enemy orders -----
uint32_t Orders::order(const OrderRequest& r, std::string* why) {
    auto no = [why](const char* text) {
        if (why) *why = text;
        return 0u;
    };
    if (r.flags & ~static_cast<uint32_t>(EMF_ORDER_ALLIES)) return no("unknown flags (a newer mod?)");
    if (!std::isfinite(r.seconds) || r.seconds < 0) return no("seconds must be 0 or more (0: until it ends otherwise)");
    if (r.damage < 0 || r.damage > kDamageMax) return no("damage must be 0..100000");
    if (!r.enemy || !r.target || r.enemy == r.target) return no("an enemy and another entity to go for");
    if (d_.player(r.enemy)) return no("that is a player");
    Creature c;
    if (!d_.creature(r.enemy, &c) || !c.ai) return no("it has no enemy AI to send (DynamicAggro, SelectedTargets)");
    if (c.boss) return no("it is a boss: its fight and its leash stay the game's");
    if (is_ally(c) && !(r.flags & EMF_ORDER_ALLIES))
        return no("it is a player's ally (a tamed beast): pass EMF_ORDER_ALLIES to send it");
    Vec at{};
    if (!d_.position(r.target, &at)) return no("no such target (or it has no position)");
    if (!alive(r.target)) return no("the target is dead");
    // Kiln carries no hit rules (hit_rule_add answers 0): EMF's answer on a game build without them
    if (r.damage > 0) return no("its damage could not be added (hit rules are off on this game build)");
    std::vector<Done> done;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        // A new order on the same enemy ends its old one. The old one's live lease is handed over (a mod renewing its
        // order every few seconds keeps the body and its hold without a gap); a second lease would lose the tie.
        Order o;
        for (size_t i = 0; i < orders_.size(); ++i)
            if (orders_[i].enemy == r.enemy) {
                Order& old = orders_[i];
                if (!old.cancelled && d_.lease_view(old.lease).state != LeaseState::Ended) {
                    o.lease = old.lease;
                    o.asserted = old.asserted;  // still held (with the old target until the next step writes this one)
                    o.renewed = true;
                    old.lease = 0;
                    old.asserted = 0;
                }
                end_order(old);
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        if (orders_.size() >= kOrdersMax) {
            if (o.lease) end_order(o);
            return no("too many orders (256)");
        }
        std::string lease_why;
        if (!o.lease) o.lease = d_.lease(r.enemy, true, &lease_why);
        if (!o.lease) {
            if (why) *why = lease_why;
            return 0;
        }
        o.id = id = next_order_++;
        o.enemy = r.enemy;
        o.target = r.target;
        o.flags = r.flags;
        o.tag = r.tag;
        if (r.seconds > 0) o.until = d_.now_ms() + static_cast<uint64_t>(std::llround(r.seconds * 1000.0));
        orders_.push_back(o);
        cancel_move_locked(r.enemy, &done);  // a move on it ends, once the order holds
    }
    call(done);
    return id;
}

void Orders::end_order(Order& o) {
    if (o.asserted) d_.prefer(o.enemy, 0, 0);
    o.asserted = 0;
    if (o.lease) d_.release(o.lease);
    o.lease = 0;
}

void Orders::cancel_order_locked(uint32_t enemy) {
    for (size_t i = 0; i < orders_.size(); ++i)
        if (orders_[i].enemy == enemy) {
            end_order(orders_[i]);
            orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

bool Orders::order_cancel(uint32_t id, bool now) {
    std::lock_guard<std::mutex> l(m_);
    for (size_t i = 0; i < orders_.size(); ++i) {
        Order& o = orders_[i];
        if (o.id != id || o.cancelled) continue;
        if (!now) {
            o.cancelled = true;
            return true;
        }
        end_order(o);
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

int Orders::tick_order(Order& o, uint64_t now) {
    if (o.cancelled || !d_.tag_alive(o.tag) || (o.until && now >= o.until)) return 1;
    if (!alive(o.target)) return 1;  // the target is gone or dead
    Creature c;
    if (!d_.creature(o.enemy, &c) || !c.ai || c.dead || d_.player(o.enemy)) return 1;
    if (is_ally(c) && !(o.flags & EMF_ORDER_ALLIES)) return 1;  // it became a player's ally
    const LeaseView v = d_.lease_view(o.lease);
    if (v.state == LeaseState::Ended) return 1;
    if (v.state != LeaseState::Active) {  // preempted (a ride) or not yet granted: nothing is held meanwhile
        if (o.asserted) d_.prefer(o.enemy, 0, 0);
        o.asserted = 0;
        return 0;
    }
    if (o.asserted != v.sequence || o.renewed) {
        o.renewed = false;
        const double left = o.until ? static_cast<double>(o.until - now) / 1000.0 : 24.0 * 3600.0;
        d_.prefer(o.enemy, o.target, left);
        d_.threat(o.enemy, o.target);
        o.asserted = v.sequence;
        o.last_threat = now;
    } else if (!(fighting(c) && c.target == o.target) && now - o.last_threat >= kThreatResendMs) {
        d_.threat(o.enemy, o.target);  // out of its fight with the target: the threat puts it back in Combat
        o.last_threat = now;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ moves ------------
std::string Orders::check_flags(uint32_t flags) {
    if (flags & EMF_MOVE_HOLD) return "EMF_MOVE_HOLD is for villager_move";
    if (flags & ~static_cast<uint32_t>(EMF_MOVE_ALLIES | EMF_MOVE_PEACEFUL | EMF_MOVE_PEACEFUL_ALL | EMF_MOVE_CLEAR))
        return "unknown flags (a newer mod?)";
    if (flags & EMF_MOVE_PEACEFUL) return "EMF_MOVE_PEACEFUL is not available on Kiln yet (no fight is cleared on the way)";
    if (flags & EMF_MOVE_PEACEFUL_ALL)
        return "EMF_MOVE_PEACEFUL_ALL is not available on Kiln yet (no fight is cleared on the way)";
    if (flags & EMF_MOVE_CLEAR) return "EMF_MOVE_CLEAR is not available on Kiln yet (no wall check at a point)";
    return {};
}

std::string Orders::check_move(const MoveRequest& r) {
    if (std::string bad = check_flags(r.flags); !bad.empty()) return bad;
    if (r.points.empty() || r.points.size() > kPointsMax) return "1 to 16 points";
    for (const Vec& p : r.points)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return "a point is not a number";
    if (!(r.arrive >= 0.5 && r.arrive <= 50.0)) return "arrive must be 0.5 to 50 metres";
    if (!(r.seconds >= 0 && r.seconds <= 3600.0)) return "seconds must be 0 (no limit) to 3600";
    if (!(r.stuck_seconds >= 2 && r.stuck_seconds <= 120.0)) return "stuck_seconds must be 0 (15) or 2 to 120";
    return {};
}

uint32_t Orders::move(const MoveRequest& r, MoveDone cb, std::string* why) {
    auto no = [why](const std::string& text) {
        if (why) *why = text;
        return 0u;
    };
    if (std::string bad = check_move(r); !bad.empty()) return no(bad);
    if (!r.enemy) return no("no creature");
    if (d_.player(r.enemy)) return no("that is a player");
    Creature c;
    if (!d_.creature(r.enemy, &c) || !c.ai) return no("it has no enemy AI to move (DynamicEnemy01BehaviorState)");
    if (c.boss) return no("it is a boss: its fight and its leash stay the game's");
    if (is_ally(c) && !(r.flags & EMF_MOVE_ALLIES))
        return no("it is a player's ally (a tamed beast): pass EMF_MOVE_ALLIES to move it");
    if (c.dead) return no("it is dead");
    const uint64_t now = d_.now_ms();
    std::vector<Done> done;
    uint32_t id = 0;
    std::string refused;
    {
        std::lock_guard<std::mutex> l(m_);
        const bool replaces = moving_locked(r.enemy) != 0;
        if (!replaces && moves_.size() >= kMovesMax) return no("too many moves (256)");
        std::string lease_why;
        const uint64_t lease = d_.lease(r.enemy, false, &lease_why);
        if (!lease) return no(lease_why);
        // whom it guards now; a move it replaces hands over whom it guarded before that move
        uint32_t prior = c.guard;
        for (size_t i = 0; i < moves_.size(); ++i)
            if (moves_[i].enemy == r.enemy) {
                prior = moves_[i].prior_guard;
                moves_[i].prior_guard = 0;  // not given back: the new move takes it over
                Move old = std::move(moves_[i]);
                moves_.erase(moves_.begin() + static_cast<std::ptrdiff_t>(i));
                finish_move(old, EMF_MOVE_CANCELLED, false);
                done.push_back({old.done, old.id, EMF_MOVE_CANCELLED, false});
                break;
            }
        if (prior && !alive(prior)) prior = 0;
        Move m;
        m.enemy = r.enemy;
        m.flags = r.flags;
        m.tag = r.tag;
        m.points = r.points;
        m.arrive = r.arrive;
        m.stuck_ms = r.stuck_seconds * 1000.0;
        m.lease = lease;
        m.prior_guard = prior;
        m.done = std::move(cb);
        if (r.seconds > 0) m.until = now + static_cast<uint64_t>(std::llround(r.seconds * 1000.0));
        if (place_waypoint(m, now, true)) {
            m.id = id = next_move_++;
            moves_.push_back(std::move(m));
            cancel_order_locked(r.enemy);  // an enemy order on it ends, once the move holds
        } else {
            d_.release(lease);
            if (!done.empty() && prior) d_.guard(r.enemy, prior);  // the move it replaced is gone: its guard back
            refused = "its waypoint could not be spawned (spawn is off on this game build?)";
        }
    }
    call(done);
    return refused.empty() ? id : no(refused);
}

bool Orders::place_waypoint(Move& m, uint64_t now, bool fresh) {
    const Vec& p = m.points[m.next];
    const uint32_t w = d_.spawn_waypoint(p, m.tag);
    m.last_place = now;
    if (!w) return false;
    m.waypoint = w;
    if (!d_.position(w, &m.waypoint_at)) m.waypoint_at = p;
    m.guard_seq = 0;  // the guard order on it goes out with the lease Active
    if (fresh) {
        m.since = now;
        m.best = distance(m.enemy, p);
    }
    return true;
}

bool Orders::waypoint_alive(const Move& m) const {
    Vec at{};
    if (!m.waypoint || !d_.position(m.waypoint, &at)) return false;
    for (int i = 0; i < 3; ++i)
        if (std::abs(at[i] - m.waypoint_at[i]) > 0.05) return false;  // the id names another entity now
    return true;
}

double Orders::distance(uint32_t enemy, const Vec& p) const {
    Vec at{};
    if (!d_.position(enemy, &at)) return -1;
    const double dx = at[0] - p[0], dy = at[1] - p[1], dz = at[2] - p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Orders::send_guard(Move& m, uint64_t now) {
    d_.guard(m.enemy, m.waypoint);
    m.last_guard = now;
}

int Orders::tick_move(Move& m, uint64_t now) {
    if (m.cancelled) return EMF_MOVE_CANCELLED;
    const LeaseView v = d_.lease_view(m.lease);
    if (v.state == LeaseState::Ended)  // the broker ended it: the body went, or the game would not take it (stuck)
        return v.body_gone ? EMF_MOVE_GONE : v.world_ended ? EMF_MOVE_CANCELLED : EMF_MOVE_STUCK;
    Creature c;
    Vec at{};
    if (!d_.creature(m.enemy, &c) || !c.ai || c.dead || d_.player(m.enemy) || !d_.position(m.enemy, &at))
        return EMF_MOVE_GONE;
    if (is_ally(c) && !(m.flags & EMF_MOVE_ALLIES)) return EMF_MOVE_GONE;
    const bool active = v.state == LeaseState::Active;
    if (!active) {  // not granted yet, or preempted (a ride): nothing is sent or decided, the stuck clock waits
        if (m.until && now >= m.until) return EMF_MOVE_TIMEOUT;
        m.since = now;
        m.best = distance(m.enemy, m.points[m.next]);
        m.guard_seq = 0;
        return -1;
    }
    {
        const Vec& p = m.points[m.next];
        const double dx = at[0] - p[0], dz = at[2] - p[2];
        double radius = m.arrive;
        if (m.waypoint && c.guard == m.waypoint) {  // it guards this waypoint: it stops its guard distance short
            if (m.guard_distance < 0) {
                double g = d_.guard_target_distance ? d_.guard_target_distance(m.enemy) : -1;
                if (!(std::isfinite(g) && g > 0)) g = kGuardFallback;
                m.guard_distance = std::min(g * kGuardFactor, kGuardMax);
            }
            radius = std::max(radius, m.guard_distance + kGuardSlack);
        }
        if (std::sqrt(dx * dx + dz * dz) <= radius && std::abs(at[1] - p[1]) <= radius + kUpDown) {
            if (m.next + 1 >= m.points.size()) return EMF_MOVE_ARRIVED;
            if (waypoint_alive(m)) d_.destroy(m.waypoint);  // on to the next point
            m.old_waypoint = m.waypoint;
            m.waypoint = 0;
            ++m.next;
            m.replaced = 0;
            m.guard_distance = -1;
            if (!place_waypoint(m, now, true)) return EMF_MOVE_STUCK;
            send_guard(m, now);
            m.guard_seq = v.sequence;
            return -1;
        }
    }
    if (m.until && now >= m.until) return EMF_MOVE_TIMEOUT;
    const double dist = distance(m.enemy, m.points[m.next]);
    // its waypoint went (destroyed, or its id is another entity's now): a new one, at most kReplaceMax times
    if (!waypoint_alive(m)) {
        if (m.waypoint) m.old_waypoint = m.waypoint;
        m.waypoint = 0;
        if (now - m.last_place >= kResendMs) {
            if (++m.replaced > kReplaceMax || !place_waypoint(m, now, false)) return EMF_MOVE_STUCK;
        }
    }
    if (m.waypoint && m.guard_seq != v.sequence) {  // granted (or resumed): the guard order on its waypoint
        send_guard(m, now);
        m.guard_seq = v.sequence;
    }
    if (fighting(c)) {  // a fight on the way (the game's way): the stuck clock and the guard order wait
        m.since = now;
        m.best = dist;
        m.last_guard = now;
        return -1;
    }
    if (dist >= 0 && dist <= m.best - kProgress) {
        m.best = dist;
        m.since = now;
    } else if (static_cast<double>(now - m.since) >= m.stuck_ms) {
        return EMF_MOVE_STUCK;
    }
    const bool guarding = c.state == kGuard && c.guard == m.waypoint;
    if (m.waypoint && !guarding && now - m.last_guard >= kResendMs) send_guard(m, now);
    return -1;
}

// may_write: the lease may still drive (the guard orders and the home are sent only then)
void Orders::finish_move(Move& m, uint32_t status, bool may_write) {
    if (waypoint_alive(m)) d_.destroy(m.waypoint);
    if (may_write && status != EMF_MOVE_GONE) {
        if (m.waypoint) d_.stop_guard(m.enemy, m.waypoint);
        if (m.old_waypoint && m.old_waypoint != m.waypoint) d_.stop_guard(m.enemy, m.old_waypoint);
        if (alive(m.prior_guard)) {
            d_.guard(m.enemy, m.prior_guard);  // back to whom it guarded (a tamed beast: its owner)
        } else if (status == EMF_MOVE_ARRIVED) {
            d_.set_home(m.enemy, m.points.back());  // it lives there now (its way home leads back here)
        }
    }
    m.waypoint = 0;
    if (m.lease) d_.release(m.lease);
    m.lease = 0;
}

bool Orders::cancel_move_locked(uint32_t enemy, std::vector<Done>* done) {
    for (size_t i = 0; i < moves_.size(); ++i)
        if (moves_[i].enemy == enemy) {
            Move m = std::move(moves_[i]);
            moves_.erase(moves_.begin() + static_cast<std::ptrdiff_t>(i));
            const bool live = !m.cancelled;
            const LeaseView v = d_.lease_view(m.lease);
            finish_move(m, EMF_MOVE_CANCELLED, v.state == LeaseState::Active);
            if (done) done->push_back({m.done, m.id, EMF_MOVE_CANCELLED, false});
            return live;
        }
    return false;
}

bool Orders::move_cancel(uint32_t id, bool now) {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = std::find_if(moves_.begin(), moves_.end(), [&](const Move& m) { return m.id == id && !m.cancelled; });
        if (it == moves_.end()) return false;
        if (!now) {
            it->cancelled = true;
            return true;
        }
        cancel_move_locked(it->enemy, &done);
    }
    call(done);
    return true;
}

uint32_t Orders::moving(uint32_t enemy) const {
    std::lock_guard<std::mutex> l(m_);
    return moving_locked(enemy);
}

uint32_t Orders::moving_locked(uint32_t enemy) const {
    for (const Move& m : moves_)
        if (m.enemy == enemy && !m.cancelled) return m.id;
    return 0;
}

void Orders::step() {
    const uint64_t now = d_.now_ms();
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        for (size_t i = 0; i < orders_.size();) {
            if (tick_order(orders_[i], now)) {
                end_order(orders_[i]);
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        std::vector<Move> keep;
        keep.reserve(moves_.size());
        for (Move& m : moves_) {
            if (!d_.tag_alive(m.tag)) {  // the mod stopped: its move ends, no callback into it
                finish_move(m, EMF_MOVE_CANCELLED, d_.lease_view(m.lease).state == LeaseState::Active);
                continue;
            }
            const int status = tick_move(m, now);
            if (status < 0) {
                keep.push_back(std::move(m));
                continue;
            }
            finish_move(m, static_cast<uint32_t>(status), d_.lease_view(m.lease).state == LeaseState::Active);
            done.push_back({m.done, m.id, static_cast<uint32_t>(status), false});
        }
        moves_ = std::move(keep);
    }
    call(done);
}

void Orders::world_reset(bool callbacks) {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        // the broker ends every lease of the world (WorldEnded) and the entities go with it: nothing is written
        for (const Move& m : moves_)  // a move cancelled for the next step gets its answer here
            if (callbacks) done.push_back({m.done, m.id, EMF_MOVE_CANCELLED, true});
        moves_.clear();
        orders_.clear();
    }
    call(done);
}

size_t Orders::order_count() const {
    std::lock_guard<std::mutex> l(m_);
    return orders_.size();
}

size_t Orders::move_count() const {
    std::lock_guard<std::mutex> l(m_);
    return moves_.size();
}

}  // namespace kemf::orders
